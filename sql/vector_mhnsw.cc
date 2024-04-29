/*
   Copyright (c) 2024, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/
#include <random>

#include <my_global.h>
#include "vector_mhnsw.h"

#include "field.h"
#include "hash.h"
#include "item.h"
#include "item_vectorfunc.h"
#include "key.h"
#include "my_base.h"
#include "mysql/psi/psi_base.h"
#include "scope.h"
#include "sql_queue.h"
#include "unireg.h"

#define HNSW_MAX_M 10000

const LEX_CSTRING mhnsw_hlindex_table={STRING_WITH_LEN("\
  CREATE TABLE i (                                      \
    layer int not null,                                 \
    src varbinary(255) not null,                        \
    neighbors varbinary(10000) not null,                 \
    index (layer, src))                                 \
")};


class FVectorRef
{
public:
  // Shallow ref copy. Used for other ref lookups in HashSet
  FVectorRef(uchar *ref, size_t ref_len): ref{ref}, ref_len{ref_len} {}
  virtual ~FVectorRef() {}

  static uchar *get_key(const FVectorRef *elem, size_t *key_len, my_bool)
  {
    *key_len= elem->ref_len;
    return elem->ref;
  }

  static void free_vector(void *elem)
  {
    delete (FVectorRef *)elem;
  }

  size_t get_ref_len() const { return ref_len; }
  const uchar* get_ref() const { return ref; }

protected:
  FVectorRef() = default;
  uchar *ref;
  size_t ref_len;
};

class FVector: public FVectorRef
{
private:
  float *vec;
  size_t vec_len;
public:
  FVector() : vec(nullptr), vec_len(0) {}
  ~FVector()
  {
    my_free(ref);
  }

  bool init(const uchar *ref, size_t ref_length,
            const float *vec, size_t vec_length)
  {
    this->ref= (uchar *)my_malloc(PSI_NOT_INSTRUMENTED,
                                  (ref_length + vec_length * sizeof(float)),
                                  MYF(0));
    if (!this->ref)
      return true;
    this->vec= (float *)(this->ref + ref_length);
    ref_len= ref_length;
    vec_len= vec_length;
    memcpy(this->ref, ref, ref_len);
    memcpy(this->vec, vec, vec_len * sizeof(float));
    return false;
  }

  FVector *deep_copy() const
  {
    FVector *new_vec= new FVector();
    new_vec->init(this->get_ref(),
                  this->get_ref_len(),
                  this->get_vec(),
                  this->get_vec_len());

    return new_vec;
  }

  size_t get_vec_len() const { return vec_len; }
  const float* get_vec() const { return vec; }

  double distance_to(const FVector &other) const
  {
    return euclidean_vec_distance(vec, other.vec, vec_len);
  }
};


static int cmp_vec(const FVector *reference, FVector *a, FVector *b)
{
  double a_dist= reference->distance_to(*a);
  double b_dist= reference->distance_to(*b);

  if (a_dist < b_dist)
    return -1;
  if (a_dist > b_dist)
    return 1;
  return 0;
}


static FVector *get_fvector_from_source(TABLE *source,
                                        Field *vect_field,
                                        const FVectorRef &ref)
{
  FVector *new_vector= new FVector;
  if (!new_vector)
    return nullptr;

  source->file->ha_rnd_pos(source->record[0],
                           const_cast<uchar *>(ref.get_ref()));

  String buf, *vec;
  vec= vect_field->val_str(&buf);

  // TODO(cvicentiu) error checking
  new_vector->init(ref.get_ref(), ref.get_ref_len(),
                   reinterpret_cast<const float *>(vec->ptr()),
                   vec->length() / sizeof(float));

  return new_vector;
}


static bool select_neighbours(TABLE *source, TABLE *graph,
                              const FVector &target,
                              const List<FVector> &candidates,
                              size_t max_neighbour_connections,
                              List<FVector> *neighbours)
{
  Queue<FVector, const FVector> pq;
  pq.init(candidates.elements, 0, 0, cmp_vec, &target);

  // TODO(cvicentiu) error checking.
  for (const auto &candidate : candidates)
    pq.push(&candidate);

  for (size_t i = 0; i < max_neighbour_connections; i++)
  {
    if (!pq.elements())
      break;
    neighbours->push_back(pq.pop());
  }

  return false;
}


static bool write_neighbours(TABLE *graph,
                             size_t layer_number,
                             const FVectorRef &source_node,
                             const List<FVector> &new_neighbours)
{
  DBUG_ASSERT(new_neighbours.elements <= HNSW_MAX_M);

  // All ref should have same length
  uint ref_length= source_node.get_ref_len();

  size_t total_size= sizeof(uint16_t) + new_neighbours.elements * ref_length;

  // Allocate memory for the struct and the flexible array member
  char *neighbor_array_bytes= static_cast<char *>(alloca(total_size));

  DBUG_ASSERT(new_neighbours.elements <= INT16_MAX);
  *(uint16_t *) neighbor_array_bytes= new_neighbours.elements;
  char *pos= neighbor_array_bytes + sizeof(uint16_t);
  for (const auto &node: new_neighbours)
  {
    DBUG_ASSERT(node.get_ref_len() == source_node.get_ref_len());
    memcpy(pos, node.get_ref(), ref_length);
    pos+= ref_length;
  }

  graph->field[0]->store(layer_number);
  graph->field[1]->store_binary(
    reinterpret_cast<const char *>(source_node.get_ref()),
    ref_length);
  graph->field[2]->set_null();

  uchar *key= (uchar*)alloca(graph->key_info->key_length);
  key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);

  int err= graph->file->ha_index_read_map(graph->record[1], key,
                                          HA_WHOLE_KEY,
                                          HA_READ_KEY_EXACT);

  // no record
  if (err == HA_ERR_KEY_NOT_FOUND)
  {
    graph->field[2]->store_binary(neighbor_array_bytes, total_size);
    graph->file->ha_write_row(graph->record[0]);
    return false;
  }

  graph->field[2]->store_binary(neighbor_array_bytes, total_size);
  graph->file->ha_update_row(graph->record[1], graph->record[0]);
  return false;
}


static bool get_neighbours(TABLE *source,
                           Field *vec_field,
                           TABLE *graph,
                           size_t layer_number,
                           const FVector &source_node,
                           List<FVector> *neighbours)
{
  // TODO(cvicentiu) This allocation need not happen in this function.
  uchar *key= (uchar*)alloca(graph->key_info->key_length);

  graph->field[0]->store(layer_number);
  graph->field[1]->store_binary(
    reinterpret_cast<const char *>(source_node.get_ref()),
    source_node.get_ref_len());
  graph->field[2]->set_null();
  key_copy(key, graph->record[0],
           graph->key_info, graph->key_info->key_length);
  if ((graph->file->ha_index_read_map(graph->record[0], key,
                                      HA_WHOLE_KEY,
                                      HA_READ_KEY_EXACT)))
    return true;

  //TODO This does two memcpys, one should use str's buffer.
  String strbuf;
  String *str= graph->field[2]->val_str(&strbuf);

  // All ref should have same length
  uint ref_length= source->file->ref_length;

  const uchar *neigh_arr_bytes= reinterpret_cast<const uchar *>(str->ptr());
  uint16_t number_of_neighbours=
    *reinterpret_cast<const uint16_t*>(neigh_arr_bytes);
  if (number_of_neighbours != (str->length() - sizeof(uint16_t)) / ref_length)
  {
    /*
      neighbours number does not match the data length,
      should not happen, possible corrupted HNSW index
    */
    DBUG_ASSERT(0); // TODO(cvicentiu) remove this after testing.
    return true;
  }

  uchar *pos = const_cast<uchar *>(neigh_arr_bytes) + sizeof(uint16_t);
  for (uint16_t i= 0; i < number_of_neighbours; i++)
  {
    FVector *dst_node= get_fvector_from_source(source, vec_field,
                                               {pos, ref_length});
    neighbours->push_back(dst_node);
    pos+= ref_length;
  }

  return false;
}


static bool update_second_degree_neighbors(TABLE *source,
                                           Field *vec_field,
                                           TABLE *graph,
                                           size_t layer_number,
                                           uint max_neighbours,
                                           const FVector &source_node,
                                           const List<FVector> &neighbours)
{
  for (const FVector &neigh: neighbours)
  {
    List<FVector> new_neighbours;
    get_neighbours(source, vec_field, graph, layer_number, neigh,
                   &new_neighbours);
    // TODO(cvicentiu) One doesn't need a deep copy, they're all just references;
    // Why add the source node, shouldn't it come from source_node anyway?
    new_neighbours.push_back(source_node.deep_copy());

    write_neighbours(graph, layer_number, neigh, new_neighbours);
    if (new_neighbours.elements > max_neighbours)
    {
      // shrink the neighbours
      List<FVector> selected;
      select_neighbours(source, graph, neigh, new_neighbours,
                        max_neighbours, &selected);
      write_neighbours(graph, layer_number, neigh, selected);
    }

    // release memory
    new_neighbours.delete_elements();
  }

  return false;
}


static bool update_neighbours(TABLE *source,
                              Field *vec_field,
                              TABLE *graph,
                              size_t layer_number,
                              uint max_neighbours,
                              const FVector &source_node,
                              const List<FVector> &neighbours)
{
  // 1. update node's neighbours
  write_neighbours(graph, layer_number, source_node, neighbours);
  // 2. update node's neighbours' neighbours (shrink before update)
  update_second_degree_neighbors(source, vec_field, graph, layer_number,
                                 max_neighbours, source_node, neighbours);
  return false;
}


static bool search_layer(TABLE *source,
                         Field *vec_field,
                         TABLE *graph,
                         const FVector &target,
                         const List<FVector> &start_nodes,
                         uint max_candidates_return,
                         size_t layer,
                         List<FVector> *result)
{
  DBUG_ASSERT(start_nodes.elements > 0);
  // Result list must be empty, otherwise there's a risk of memory leak
  DBUG_ASSERT(result->elements == 0);

  Queue<FVector, const FVector> candidates;
  Queue<FVector, const FVector> best;
  //TODO(cvicentiu) Fix this hash method.
  Hash_set<FVectorRef> visited(PSI_INSTRUMENT_MEM, &my_charset_bin,
                               1000, 0, 0,
                               (my_hash_get_key)FVectorRef::get_key,
                               FVectorRef::free_vector,
                               HASH_UNIQUE);

  candidates.init(1000, 0, false, cmp_vec, &target);
  best.init(1000, 0, true, cmp_vec, &target);

  for (const FVector &node : start_nodes)
  {
    FVector *v= node.deep_copy();
    candidates.push(v);
    best.push(v);
    visited.insert(v);
  }

  while (candidates.elements())
  {
    const FVector &cur_vec= *candidates.pop();
    const FVector &furthest_best= *best.top();
    // TODO(cvicentiu) what if we haven't yet reached max_candidates_return?
    // Should we just quit now, maybe continue searching for candidates till
    // we at least get max_candidates_return.
    if (target.distance_to(cur_vec) > target.distance_to(furthest_best))
    {
      break; // All possible candidates are worse than what we have.
             // Can't get better.
    }

    List<FVector> neighbours;
    get_neighbours(source, vec_field, graph, layer, cur_vec, &neighbours);

    for (const FVector &neigh: neighbours)
    {
      if (visited.find(&neigh))
      {
        continue;
      }

      FVector *clone= neigh.deep_copy();
      visited.insert(clone);
      const FVector &furthest_best= *best.top();
      if (best.elements() < max_candidates_return)
      {
        candidates.push(clone);
        best.push(clone);
      }
      else if (target.distance_to(neigh) < target.distance_to(furthest_best))
      {
        best.replace_top(clone);
        candidates.push(clone);
      }
    }
    neighbours.delete_elements();
  }

  while (best.elements())
  {
    // TODO(cvicentiu) this is n*log(n), we need a queue iterator.
    result->push_back(best.pop()->deep_copy());
  }

  return false;
}


std::mt19937 gen(42); // TODO(cvicentiu) seeded with 42 for now, this should
                      // use a rnd service

int mhnsw_insert(TABLE *table, KEY *keyinfo)
{
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  Field *vec_field= keyinfo->key_part->field;
  String buf, *res= vec_field->val_str(&buf);
  handler *h= table->file;
  int err= 0;

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_MHNSW);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(vec_field->binary());
  DBUG_ASSERT(vec_field->cmp_type() == STRING_RESULT);
  DBUG_ASSERT(res); // ER_INDEX_CANNOT_HAVE_NULL
  DBUG_ASSERT(h->ref_length <= graph->field[1]->field_length);
  DBUG_ASSERT(h->ref_length <= graph->field[2]->field_length);

  if (res->length() == 0 || res->length() % 4)
    return 1;

  const uint EF_CONSTRUCTION = 200; // max candidate list size to connect to.
  const uint MAX_INSERT_NEIGHBOR_CONNECTIONS = 24;
  const uint MAX_NEIGHBORS_PER_LAYER = 24; //m
  const double NORMALIZATION_FACTOR = 2;

  if ((err= h->ha_rnd_init(1)))
    return err;

  if ((err= graph->file->ha_index_init(0, 1)))
    return err;

  longlong max_layer;
  if ((err= graph->file->ha_index_last(graph->record[0])))
  {
    if (err != HA_ERR_END_OF_FILE)
    {
      graph->file->ha_index_end();
      return err;
    }
    // First insert!
    h->position(table->record[0]);
    write_neighbours(graph, 0, {h->ref, h->ref_length}, {});

    h->ha_rnd_end();
    graph->file->ha_index_end();
    return 0; // TODO (error during store_link)
  }
  else
    max_layer= graph->field[0]->val_int();

  FVector target;
  h->position(table->record[0]);
  target.init(h->ref, h->ref_length,
              reinterpret_cast<const float *>(res->ptr()),
              res->length() / sizeof(float));


  std::uniform_real_distribution<> dis(0.0, 1.0);
  double new_num= dis(gen);
  double log= -std::log(new_num) * NORMALIZATION_FACTOR;
  longlong new_node_layer= std::floor(log);

  List<FVector> start_nodes;

  String ref_str, *ref_ptr;
  ref_ptr= graph->field[1]->val_str(&ref_str);

  FVector *start_node= get_fvector_from_source(table, vec_field,
                                               {(uchar *)ref_ptr->ptr(),
                                               ref_ptr->length()});
  // TODO(cvicentiu) error checking. Also make sure we use a random start node
  // in last layer.
  start_nodes.push_back(start_node);
  // TODO start_nodes needs to have one element in it.
  for (longlong cur_layer= max_layer; cur_layer > new_node_layer; cur_layer--)
  {
    List<FVector> candidates;
    search_layer(table, vec_field, graph, target, start_nodes, 1, cur_layer,
                 &candidates);
    start_nodes.delete_elements();
    start_nodes.push_back(candidates.head()->deep_copy());
    candidates.delete_elements();
  }

  for (longlong cur_layer= std::min(max_layer, new_node_layer);
       cur_layer >= 0; cur_layer--)
  {
    List<FVector> candidates;
    List<FVector> neighbours;
    search_layer(table, vec_field, graph, target, start_nodes, EF_CONSTRUCTION,
                 cur_layer, &candidates);
    // release vectors
    start_nodes.delete_elements();
    uint max_neighbours= (cur_layer == 0) ? MAX_NEIGHBORS_PER_LAYER * 2
                                          : MAX_NEIGHBORS_PER_LAYER;

    select_neighbours(table, graph, target, candidates,
                      MAX_INSERT_NEIGHBOR_CONNECTIONS, &neighbours);
    update_neighbours(table, vec_field, graph, cur_layer, max_neighbours,
                      target, neighbours);
    start_nodes= candidates;
  }
  start_nodes.delete_elements();

  for (longlong cur_layer= max_layer + 1; cur_layer <= new_node_layer;
       cur_layer++)
  {
    write_neighbours(graph, cur_layer, target, {});
  }


  h->ha_rnd_end();
  graph->file->ha_index_end();
  dbug_tmp_restore_column_map(&table->read_set, old_map);

  return err == HA_ERR_END_OF_FILE ? 0 : err;
}


int mhnsw_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit)
{
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  // TODO(cvicentiu) onlye one hlindex now.
  Field *vec_field= keyinfo->key_part->field;
  Item_func_vec_distance *fun= (Item_func_vec_distance *)dist;
  String buf, *res= fun->arguments()[1]->val_str(&buf);
  FVector target;
  handler *h= table->file;

  //TODO(scope_exit)
  int err;
  if ((err= h->ha_rnd_init(0)))
    return err;

  if ((err= graph->file->ha_index_init(0, 1)))
    return err;

  h->position(table->record[0]);
  target.init(h->ref, h->ref_length,
              reinterpret_cast<const float *>(res->ptr()),
              res->length() / sizeof(float));

  List<FVector> candidates;
  List<FVector> start_nodes;

  longlong max_layer;
  if ((err= graph->file->ha_index_last(graph->record[0])))
  {
    if (err != HA_ERR_END_OF_FILE)
    {
      graph->file->ha_index_end();
      return err;
    }
    h->ha_rnd_end();
    graph->file->ha_index_end();
    return 0; // TODO (error during store_link)
  }
  else
    max_layer= graph->field[0]->val_int();

  String ref_str, *ref_ptr;
  ref_ptr= graph->field[1]->val_str(&ref_str);
  FVector *start_node= get_fvector_from_source(table, vec_field,
                                               {(uchar *)ref_ptr->ptr(),
                                               ref_ptr->length()});
  // TODO(cvicentiu) error checking. Also make sure we use a random start node
  // in last layer.
  start_nodes.push_back(start_node);


  for (size_t cur_layer= max_layer; cur_layer > 0; cur_layer--)
  {
    search_layer(table, vec_field, graph, target, start_nodes, 1,
                 cur_layer, &candidates);
    start_nodes.delete_elements();
    start_nodes.push_back(candidates.head()->deep_copy());
    candidates.delete_elements();
  }

  search_layer(table, vec_field, graph, target, start_nodes,
               40, 0, &candidates);
  start_nodes.delete_elements();

  // 8. return results
  FVectorRef **context= (FVectorRef**)table->in_use->alloc(
    sizeof(FVectorRef*) * (candidates.elements + 1));
  graph->context= context;

  FVectorRef **ptr= context + candidates.elements;
  *ptr= nullptr;
  while (candidates.elements)
    *--ptr= candidates.pop();

  err= mhnsw_next(table);
  graph->file->ha_index_end();

  // TODO release vectors after query

  dbug_tmp_restore_column_map(&table->read_set, old_map);
  return err;
}

int mhnsw_next(TABLE *table)
{
  FVectorRef ***context= (FVectorRef ***)&table->hlindex->context;
  FVectorRef *cur_vec= **context;
  if (cur_vec)
  {
    int err= table->file->ha_rnd_pos(table->record[0],
                                     (uchar *)(cur_vec)->get_ref());
    // release vectors
    delete cur_vec;

    (*context)++;
    return err;
  }
  return HA_ERR_END_OF_FILE;
}
