/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file trx/trx0purge.cc
Purge old versions

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0purge.h"
#include "fsp0fsp.h"
#include "fut0fut.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "os0thread.h"
#include "que0que.h"
#include "row0purge.h"
#include "row0upd.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "sync0sync.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include <mysql/service_wsrep.h>

#include <unordered_map>

/** Maximum allowable purge history length.  <=0 means 'infinite'. */
ulong		srv_max_purge_lag = 0;

/** Max DML user threads delay in micro-seconds. */
ulong		srv_max_purge_lag_delay = 0;

/** The global data structure coordinating a purge */
purge_sys_t	purge_sys;

/** A dummy undo record used as a return value when we have a whole undo log
which needs no purge */
trx_undo_rec_t	trx_purge_dummy_rec;

#ifdef UNIV_DEBUG
my_bool		srv_purge_view_update_only_debug;
#endif /* UNIV_DEBUG */

/** Sentinel value */
static const TrxUndoRsegs NullElement;

/** Default constructor */
TrxUndoRsegsIterator::TrxUndoRsegsIterator()
	: m_rsegs(NullElement), m_iter(m_rsegs.begin())
{
}

/** Sets the next rseg to purge in purge_sys.
Executed in the purge coordinator thread.
@return whether anything is to be purged */
inline bool TrxUndoRsegsIterator::set_next()
{
	mutex_enter(&purge_sys.pq_mutex);

	/* Only purge consumes events from the priority queue, user
	threads only produce the events. */

	/* Check if there are more rsegs to process in the
	current element. */
	if (m_iter != m_rsegs.end()) {
		/* We are still processing rollback segment from
		the same transaction and so expected transaction
		number shouldn't increase. Undo the increment of
		expected commit done by caller assuming rollback
		segments from given transaction are done. */
		purge_sys.tail.trx_no = (*m_iter)->last_trx_no();
	} else if (!purge_sys.purge_queue.empty()) {
		m_rsegs = purge_sys.purge_queue.top();
		purge_sys.purge_queue.pop();
		ut_ad(purge_sys.purge_queue.empty()
		      || purge_sys.purge_queue.top() != m_rsegs);
		m_iter = m_rsegs.begin();
	} else {
		/* Queue is empty, reset iterator. */
		purge_sys.rseg = NULL;
		mutex_exit(&purge_sys.pq_mutex);
		m_rsegs = NullElement;
		m_iter = m_rsegs.begin();
		return false;
	}

	purge_sys.rseg = *m_iter++;
	mutex_exit(&purge_sys.pq_mutex);
	mutex_enter(&purge_sys.rseg->mutex);

	ut_a(purge_sys.rseg->last_page_no != FIL_NULL);
	ut_ad(purge_sys.rseg->last_trx_no() == m_rsegs.trx_no);

	/* We assume in purge of externally stored fields that space id is
	in the range of UNDO tablespace space ids */
	ut_ad(purge_sys.rseg->space->id == TRX_SYS_SPACE
	      || srv_is_undo_tablespace(purge_sys.rseg->space->id));

	ut_a(purge_sys.tail.trx_no <= purge_sys.rseg->last_trx_no());

	purge_sys.tail.trx_no = purge_sys.rseg->last_trx_no();
	purge_sys.hdr_offset = purge_sys.rseg->last_offset();
	purge_sys.hdr_page_no = purge_sys.rseg->last_page_no;

	mutex_exit(&purge_sys.rseg->mutex);

	return(true);
}

/** Build a purge 'query' graph. The actual purge is performed by executing
this query graph.
@return own: the query graph */
static
que_t*
purge_graph_build()
{
	ut_a(srv_n_purge_threads > 0);

	trx_t* trx = trx_create();
	ut_ad(!trx->id);
	trx->start_time = time(NULL);
	trx->start_time_micro = microsecond_interval_timer();
	trx->state = TRX_STATE_ACTIVE;
	trx->op_info = "purge trx";

	mem_heap_t*	heap = mem_heap_create(512);
	que_fork_t*	fork = que_fork_create(
		NULL, NULL, QUE_FORK_PURGE, heap);
	fork->trx = trx;

	for (auto i = innodb_purge_threads_MAX; i; i--) {
		que_thr_t*	thr = que_thr_create(fork, heap, NULL);
		thr->child = new(mem_heap_alloc(heap, sizeof(purge_node_t)))
			purge_node_t(thr);
	}

	return(fork);
}

/** Initialise the purge system. */
void purge_sys_t::create()
{
  ut_ad(this == &purge_sys);
  ut_ad(!heap);
  ut_ad(!enabled());
  m_paused= 0;
  query= purge_graph_build();
  next_stored= false;
  rseg= NULL;
  page_no= 0;
  offset= 0;
  hdr_page_no= 0;
  hdr_offset= 0;
  rw_lock_create(trx_purge_latch_key, &latch, SYNC_PURGE_LATCH);
  mutex_create(LATCH_ID_PURGE_SYS_PQ, &pq_mutex);
  truncate.current= NULL;
  truncate.last= NULL;
  heap= mem_heap_create(4096);
}

/** Close the purge subsystem on shutdown. */
void purge_sys_t::close()
{
  ut_ad(this == &purge_sys);
  if (!heap)
    return;

  ut_ad(!enabled());
  trx_t* trx = query->trx;
  que_graph_free(query);
  ut_ad(!trx->id);
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  trx->state= TRX_STATE_NOT_STARTED;
  trx->free();
  rw_lock_free(&latch);
  mutex_free(&pq_mutex);
  mem_heap_free(heap);
  heap= nullptr;
}

/*================ UNDO LOG HISTORY LIST =============================*/

/** Prepend the history list with an undo log.
Remove the undo log segment from the rseg slot if it is too big for reuse.
@param[in]	trx		transaction
@param[in,out]	undo		undo log
@param[in,out]	mtr		mini-transaction */
void
trx_purge_add_undo_to_history(const trx_t* trx, trx_undo_t*& undo, mtr_t* mtr)
{
	DBUG_PRINT("trx", ("commit(" TRX_ID_FMT "," TRX_ID_FMT ")",
			   trx->id, trx_id_t{trx->rw_trx_hash_element->no}));
	ut_ad(undo == trx->rsegs.m_redo.undo);
	trx_rseg_t*	rseg		= trx->rsegs.m_redo.rseg;
	ut_ad(undo->rseg == rseg);
	buf_block_t*	rseg_header	= trx_rsegf_get(
		rseg->space, rseg->page_no, mtr);
	buf_block_t*	undo_page	= trx_undo_set_state_at_finish(
		undo, mtr);
	trx_ulogf_t*	undo_header	= undo_page->frame + undo->hdr_offset;

	ut_ad(mach_read_from_2(undo_header + TRX_UNDO_NEEDS_PURGE) <= 1);
	ut_ad(rseg->needs_purge > trx->id);

	if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT
					   + rseg_header->frame))) {
		/* This database must have been upgraded from
		before MariaDB 10.3.5. */
		trx_rseg_format_upgrade(rseg_header, mtr);
	}

	if (undo->state != TRX_UNDO_CACHED) {
		/* The undo log segment will not be reused */
		ut_a(undo->id < TRX_RSEG_N_SLOTS);
		compile_time_assert(FIL_NULL == 0xffffffff);
		mtr->memset(rseg_header,
			    TRX_RSEG + TRX_RSEG_UNDO_SLOTS
			    + undo->id * TRX_RSEG_SLOT_SIZE, 4, 0xff);

		MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);

		uint32_t hist_size = mach_read_from_4(TRX_RSEG_HISTORY_SIZE
						      + TRX_RSEG
						      + rseg_header->frame);

		ut_ad(undo->size == flst_get_len(TRX_UNDO_SEG_HDR
						 + TRX_UNDO_PAGE_LIST
						 + undo_page->frame));

		mtr->write<4>(*rseg_header, TRX_RSEG + TRX_RSEG_HISTORY_SIZE
			      + rseg_header->frame,
			      hist_size + undo->size);
		mtr->write<8>(*rseg_header, TRX_RSEG + TRX_RSEG_MAX_TRX_ID
			      + rseg_header->frame,
			      trx_sys.get_max_trx_id());
	}

	/* After the purge thread has been given permission to exit,
	we may roll back transactions (trx->undo_no==0)
	in THD::cleanup() invoked from unlink_thd() in fast shutdown,
	or in trx_rollback_recovered() in slow shutdown.

	Before any transaction-generating background threads or the
	purge have been started, we can
	start transactions in row_merge_drop_temp_indexes() and
	fts_drop_orphaned_tables(), and roll back recovered transactions.

	Arbitrary user transactions may be executed when all the undo log
	related background processes (including purge) are disabled due to
	innodb_force_recovery=2 or innodb_force_recovery=3.
	DROP TABLE may be executed at any innodb_force_recovery	level.

	During fast shutdown, we may also continue to execute
	user transactions. */
	ut_ad(srv_undo_sources
	      || trx->undo_no == 0
	      || (!purge_sys.enabled()
		  && (srv_is_being_started
		      || trx_rollback_is_active
		      || srv_force_recovery >= SRV_FORCE_NO_BACKGROUND))
	      || ((trx->mysql_thd || trx->internal)
		  && srv_fast_shutdown));

#ifdef	WITH_WSREP
	if (wsrep_is_wsrep_xid(trx->xid)) {
		trx_rseg_update_wsrep_checkpoint(rseg_header, trx->xid, mtr);
	}
#endif

	if (trx->mysql_log_file_name && *trx->mysql_log_file_name) {
		/* Update the latest MySQL binlog name and offset info
		in rollback segment header if MySQL binlogging is on
		or the database server is a MySQL replication save. */
		trx_rseg_update_binlog_offset(rseg_header, trx, mtr);
	}

	/* Add the log as the first in the history list */
	flst_add_first(rseg_header, TRX_RSEG + TRX_RSEG_HISTORY, undo_page,
		       static_cast<uint16_t>(undo->hdr_offset
					     + TRX_UNDO_HISTORY_NODE), mtr);

	mtr->write<8,mtr_t::MAYBE_NOP>(*undo_page,
				       undo_header + TRX_UNDO_TRX_NO,
				       trx->rw_trx_hash_element->no);
	mtr->write<2,mtr_t::MAYBE_NOP>(*undo_page, undo_header
				       + TRX_UNDO_NEEDS_PURGE, 1U);

	if (rseg->last_page_no == FIL_NULL) {
		rseg->last_page_no = undo->hdr_page_no;
		rseg->set_last_commit(undo->hdr_offset,
				      trx->rw_trx_hash_element->no);
	}

	trx_sys.rseg_history_len++;

	if (undo->state == TRX_UNDO_CACHED) {
		UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
	} else {
		ut_ad(undo->state == TRX_UNDO_TO_PURGE);
		ut_free(undo);
	}

	undo = NULL;
}

/** Remove undo log header from the history list.
@param[in,out]  rseg    rollback segment header page
@param[in]      log     undo log segment header page
@param[in]      offset  byte offset in the undo log segment header page
@param[in,out]  mtr     mini-transaction */
static void trx_purge_remove_log_hdr(buf_block_t *rseg, buf_block_t* log,
                                     uint16_t offset, mtr_t *mtr)
{
  flst_remove(rseg, TRX_RSEG + TRX_RSEG_HISTORY,
              log, static_cast<uint16_t>(offset + TRX_UNDO_HISTORY_NODE), mtr);
  trx_sys.rseg_history_len--;
}

/** Free an undo log segment, and remove the header from the history list.
@param[in,out]	mtr		mini-transaction
@param[in,out]	rseg		rollback segment
@param[in]	hdr_addr	file address of log_hdr */
static
void trx_purge_free_segment(mtr_t &mtr, trx_rseg_t* rseg, fil_addr_t hdr_addr)
{
  mtr.commit();
  mtr.start();
  ut_ad(mutex_own(&rseg->mutex));

  buf_block_t *rseg_hdr= trx_rsegf_get(rseg->space, rseg->page_no, &mtr);
  buf_block_t *block=
    trx_undo_page_get(page_id_t(rseg->space->id, hdr_addr.page), &mtr);
  const uint32_t seg_size=
    flst_get_len(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST + block->frame);
  ut_ad(rseg->curr_size >= seg_size);
  rseg->curr_size-= seg_size;

  trx_purge_remove_log_hdr(rseg_hdr, block, hdr_addr.boffset, &mtr);
  byte *hist= TRX_RSEG + TRX_RSEG_HISTORY_SIZE + rseg_hdr->frame;
  ut_ad(mach_read_from_4(hist) >= seg_size);
  mtr.write<4>(*rseg_hdr, hist, mach_read_from_4(hist) - seg_size);

  while (!fseg_free_step_not_header(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER +
                                    block->frame, &mtr))
  {
    buf_block_buf_fix_inc(rseg_hdr, __FILE__, __LINE__);
    buf_block_buf_fix_inc(block, __FILE__, __LINE__);
    ut_d(const page_id_t rseg_hdr_id{rseg_hdr->page.id()});
    ut_d(const page_id_t id{block->page.id()});
    mtr.commit();
    /* NOTE: If the server is killed after the log that was produced
    up to this point was written, and before the log from the mtr.commit()
    in our caller is written, then the pages belonging to the
    undo log will become unaccessible garbage.

    This does not matter when using multiple innodb_undo_tablespaces;
    innodb_undo_log_truncate=ON will be able to reclaim the space. */
    mtr.start();
    rw_lock_x_lock(&rseg_hdr->lock);
    rw_lock_x_lock(&block->lock);
    ut_ad(rseg_hdr->page.id() == rseg_hdr_id);
    ut_ad(block->page.id() == id);
    mtr_memo_push(&mtr, rseg_hdr, MTR_MEMO_PAGE_X_FIX);
    mtr_memo_push(&mtr, block, MTR_MEMO_PAGE_X_FIX);
  }

  while (!fseg_free_step(TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER +
                         block->frame, &mtr));
}

/** Remove unnecessary history data from a rollback segment.
@param[in,out]	rseg		rollback segment
@param[in]	limit		truncate anything before this
@param[in]	all		whether everything can be truncated */
static
void
trx_purge_truncate_rseg_history(
	trx_rseg_t&			rseg,
	const purge_sys_t::iterator&	limit,
	bool				all)
{
	fil_addr_t	hdr_addr;
	fil_addr_t	prev_hdr_addr;
	mtr_t		mtr;
	trx_id_t	undo_trx_no;

	mtr.start();

	buf_block_t* rseg_hdr = trx_rsegf_get(rseg.space, rseg.page_no, &mtr);

	hdr_addr = flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY
				 + rseg_hdr->frame);
	hdr_addr.boffset = static_cast<uint16_t>(hdr_addr.boffset
						 - TRX_UNDO_HISTORY_NODE);

loop:
	if (hdr_addr.page == FIL_NULL) {
func_exit:
		mtr.commit();
		return;
	}

	buf_block_t* block = trx_undo_page_get(page_id_t(rseg.space->id,
							 hdr_addr.page),
					       &mtr);
	undo_trx_no = mach_read_from_8(block->frame + hdr_addr.boffset
				       + TRX_UNDO_TRX_NO);

	if (undo_trx_no >= limit.trx_no) {
		if (undo_trx_no == limit.trx_no) {
			trx_undo_truncate_start(
				&rseg, hdr_addr.page,
				hdr_addr.boffset, limit.undo_no);
		}

		goto func_exit;
	}

	if (!all) {
		goto func_exit;
	}

	prev_hdr_addr = flst_get_prev_addr(block->frame + hdr_addr.boffset
					   + TRX_UNDO_HISTORY_NODE);
	prev_hdr_addr.boffset = static_cast<uint16_t>(prev_hdr_addr.boffset
						      - TRX_UNDO_HISTORY_NODE);

	if (mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE + block->frame)
	    == TRX_UNDO_TO_PURGE
	    && !mach_read_from_2(block->frame + hdr_addr.boffset
				 + TRX_UNDO_NEXT_LOG)) {
		/* We can free the whole log segment.
		This will call trx_purge_remove_log_hdr(). */
		trx_purge_free_segment(mtr, &rseg, hdr_addr);
	} else {
		/* Remove the log hdr from the rseg history. */
		trx_purge_remove_log_hdr(rseg_hdr, block, hdr_addr.boffset,
					 &mtr);
	}

	mtr.commit();
	mtr.start();

	rseg_hdr = trx_rsegf_get(rseg.space, rseg.page_no, &mtr);

	hdr_addr = prev_hdr_addr;

	goto loop;
}

/** Cleanse purge queue to remove the rseg that reside in undo-tablespace
marked for truncate.
@param[in]	space	undo tablespace being truncated */
static void trx_purge_cleanse_purge_queue(const fil_space_t& space)
{
	typedef	std::vector<TrxUndoRsegs>	purge_elem_list_t;
	purge_elem_list_t			purge_elem_list;

	mutex_enter(&purge_sys.pq_mutex);

	/* Remove rseg instances that are in the purge queue before we start
	truncate of corresponding UNDO truncate. */
	while (!purge_sys.purge_queue.empty()) {
		purge_elem_list.push_back(purge_sys.purge_queue.top());
		purge_sys.purge_queue.pop();
	}

	for (purge_elem_list_t::iterator it = purge_elem_list.begin();
	     it != purge_elem_list.end();
	     ++it) {

		for (TrxUndoRsegs::iterator it2 = it->begin();
		     it2 != it->end();
		     ++it2) {
			if ((*it2)->space == &space) {
				it->erase(it2);
				break;
			}
		}

		if (!it->empty()) {
			purge_sys.purge_queue.push(*it);
		}
	}

	mutex_exit(&purge_sys.pq_mutex);
}

#if defined __GNUC__ && __GNUC__ == 4 && !defined __clang__
# if defined __arm__ || defined __aarch64__
/* Work around an internal compiler error in GCC 4.8.5 */
__attribute__((optimize(0)))
# endif
#endif
/**
Remove unnecessary history data from rollback segments. NOTE that when this
function is called, the caller (purge_coordinator_callback)
must not have any latches on undo log pages!
*/
void trx_purge_truncate_history()
{
  ut_ad(purge_sys.head <= purge_sys.tail);
  purge_sys_t::iterator &head= purge_sys.head.trx_no
    ? purge_sys.head : purge_sys.tail;

  if (head.trx_no >= purge_sys.low_limit_no())
  {
    /* This is sometimes necessary. TODO: find out why. */
    head.trx_no= purge_sys.low_limit_no();
    head.undo_no= 0;
  }

  for (ulint i= 0; i < TRX_SYS_N_RSEGS; ++i)
  {
    if (trx_rseg_t *rseg= trx_sys.rseg_array[i])
    {
      ut_ad(rseg->id == i);
      ut_ad(rseg->is_persistent());
      log_free_check();
      mutex_enter(&rseg->mutex);
      trx_purge_truncate_rseg_history(*rseg, head,
                                      !rseg->trx_ref_count &&
                                      purge_sys.sees(rseg->needs_purge));
      mutex_exit(&rseg->mutex);
    }
  }

  if (srv_undo_tablespaces_active < 2)
    return;

  while (srv_undo_log_truncate)
  {
    if (!purge_sys.truncate.current)
    {
      const ulint threshold=
        ulint(srv_max_undo_log_size >> srv_page_size_shift);
      for (ulint i= purge_sys.truncate.last
           ? purge_sys.truncate.last->id - srv_undo_space_id_start : 0,
           j= i;; )
      {
        const auto space_id= srv_undo_space_id_start + i;
        ut_ad(srv_is_undo_tablespace(space_id));
        fil_space_t *space= fil_space_get(space_id);
        ut_a(UT_LIST_GET_LEN(space->chain) == 1);

        if (space && space->get_size() > threshold)
        {
          purge_sys.truncate.current= space;
          break;
        }

        ++i;
        i %= srv_undo_tablespaces_active;
        if (i == j)
          return;
      }
    }

    fil_space_t &space= *purge_sys.truncate.current;
    /* Undo tablespace always are a single file. */
    fil_node_t *file= UT_LIST_GET_FIRST(space.chain);
    /* The undo tablespace files are never closed. */
    ut_ad(file->is_open());

    DBUG_LOG("undo", "marking for truncate: " << file->name);

    for (ulint i= 0; i < TRX_SYS_N_RSEGS; ++i)
    {
      trx_rseg_t *rseg= trx_sys.rseg_array[i];
      if (!rseg || rseg->space != &space)
        continue;
      ut_ad(rseg->is_persistent());

      mutex_enter(&rseg->mutex);
      /* Once set, this rseg will not be allocated to subsequent
      transactions, but we will wait for existing active
      transactions to finish and to be purged. */
      rseg->skip_allocation = true;

      if (rseg->trx_ref_count || !purge_sys.sees(rseg->needs_purge))
      {
      not_free:
        mutex_exit(&rseg->mutex);
        return;
      }

      ut_ad(UT_LIST_GET_LEN(rseg->undo_list) == 0);
      /* Check if all segments are cached and safe to remove. */
      ulint cached= 0;

      for (const trx_undo_t *undo= UT_LIST_GET_FIRST(rseg->undo_cached); undo;
           undo= UT_LIST_GET_NEXT(undo_list, undo))
      {
        if (head.trx_no && head.trx_no < undo->trx_id)
          goto not_free;
        else
          cached+= undo->size;
      }

      ut_ad(rseg->curr_size > cached);

      if (rseg->curr_size > cached + 1 &&
          (srv_fast_shutdown || srv_undo_sources || trx_sys.rseg_history_len))
        goto not_free;

      mutex_exit(&rseg->mutex);
    }

    ib::info() << "Truncating " << file->name;
    trx_purge_cleanse_purge_queue(space);

    log_free_check();

    mtr_t mtr;
    mtr.start();
    mtr_x_lock_space(&space, &mtr);

    /* Lock all modified pages of the tablespace.

    During truncation, we do not want any writes to the file.

    If a log checkpoint was completed at LSN earlier than our
    mini-transaction commit and the server was killed, then
    discarding the to-be-trimmed pages without flushing would
    break crash recovery. */

  rescan:
    if (UNIV_UNLIKELY(srv_shutdown_state != SRV_SHUTDOWN_NONE) &&
        srv_fast_shutdown)
    {
    fast_shutdown:
      mtr.commit();
      return;
    }

    mysql_mutex_lock(&buf_pool.flush_list_mutex);

    for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list); bpage; )
    {
      ut_ad(bpage->oldest_modification());
      ut_ad(bpage->in_file());

      buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);

      if (bpage->oldest_modification() > 2 &&
          bpage->id().space() == space.id)
      {
        ut_ad(bpage->state() == BUF_BLOCK_FILE_PAGE);
        auto block= reinterpret_cast<buf_block_t*>(bpage);
        buf_block_buf_fix_inc(block, __FILE__, __LINE__);
        buf_pool.flush_hp.set(prev);
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);

#ifdef BTR_CUR_HASH_ADAPT
        ut_ad(!block->index); /* There is no AHI on undo tablespaces. */
#endif
        rw_lock_x_lock(&block->lock);
        mysql_mutex_lock(&buf_pool.flush_list_mutex);
        ut_ad(bpage->io_fix() == BUF_IO_NONE);

        if (bpage->oldest_modification() > 2 &&
            !mtr.have_x_latch(*reinterpret_cast<buf_block_t*>(bpage)))
          mtr.memo_push(block, MTR_MEMO_PAGE_X_FIX);
        else
        {
          buf_block_buf_fix_dec(block);
          rw_lock_x_unlock(&block->lock);
        }

        if (prev != buf_pool.flush_hp.get())
        {
          /* The functions buf_pool_t::release_freed_page() or
          buf_do_flush_list_batch() may be right now holding
          buf_pool.mutex and waiting to acquire
          buf_pool.flush_list_mutex. Ensure that they can proceed,
          to avoid extreme waits. */
          mysql_mutex_unlock(&buf_pool.flush_list_mutex);
          goto rescan;
        }
      }

      bpage= prev;
    }

    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (UNIV_UNLIKELY(srv_shutdown_state != SRV_SHUTDOWN_NONE) &&
        srv_fast_shutdown)
      goto fast_shutdown;

    /* Adjust the tablespace metadata. */
    if (!fil_truncate_prepare(space.id))
    {
      ib::error() << "Failed to find UNDO tablespace " << file->name;
      mtr.commit();
      return;
    }

    /* Re-initialize tablespace, in a single mini-transaction. */
    const ulint size= SRV_UNDO_TABLESPACE_SIZE_IN_PAGES;
    /* Associate the undo tablespace with mtr.
    During mtr::commit_shrink(), InnoDB can use the undo
    tablespace object to clear all freed ranges */
    mtr.set_named_space(&space);
    mtr.trim_pages(page_id_t(space.id, size));
    fsp_header_init(&space, size, &mtr);
    mutex_enter(&fil_system.mutex);
    space.size= file->size= size;
    mutex_exit(&fil_system.mutex);

    buf_block_t *sys_header= trx_sysf_get(&mtr);

    for (ulint i= 0; i < TRX_SYS_N_RSEGS; ++i)
    {
      trx_rseg_t *rseg= trx_sys.rseg_array[i];
      if (!rseg || rseg->space != &space)
        continue;

      ut_ad(rseg->id == i);
      ut_ad(rseg->is_persistent());
      ut_ad(!rseg->trx_ref_count);
      ut_ad(!head.trx_no || rseg->needs_purge <= head.trx_no);
      ut_d(const auto old_page= rseg->page_no);

      buf_block_t *rblock= trx_rseg_header_create(&space, i,
                                                  trx_sys.get_max_trx_id(),
                                                  sys_header, &mtr);
      ut_ad(rblock);
      rseg->page_no= rblock ? rblock->page.id().page_no() : FIL_NULL;
      ut_ad(old_page == rseg->page_no);

      /* Before re-initialization ensure that we free the existing
      structure. There can't be any active transactions. */
      ut_a(UT_LIST_GET_LEN(rseg->undo_list) == 0);

      for (trx_undo_t *undo= UT_LIST_GET_FIRST(rseg->undo_cached), *next_undo;
           undo; undo= next_undo)
      {
        next_undo= UT_LIST_GET_NEXT(undo_list, undo);
        UT_LIST_REMOVE(rseg->undo_cached, undo);
        MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
        ut_free(undo);
      }

      /* These were written by trx_rseg_header_create(). */
      ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT + rblock->frame));
      ut_ad(!mach_read_from_4(TRX_RSEG + TRX_RSEG_HISTORY_SIZE +
                              rblock->frame));
      /* Initialize the undo log lists according to
      the rseg header */
      rseg->curr_size= 1;
      rseg->trx_ref_count= 0;
      rseg->needs_purge= 0;
      rseg->skip_allocation= false;
      rseg->last_page_no= FIL_NULL;
      rseg->last_commit_and_offset= 0;
    }

    mtr.commit_shrink(space);

    /* No mutex; this is only updated by the purge coordinator. */
    export_vars.innodb_undo_truncations++;

    if (purge_sys.rseg && purge_sys.rseg->last_page_no == FIL_NULL)
    {
      /* If purge_sys.rseg is pointing to rseg that was recently
      truncated then move to next rseg element.

      Note: Ideally purge_sys.rseg should be NULL because purge should
      complete processing of all the records but srv_purge_batch_size
      can force the purge loop to exit before all the records are purged. */
      purge_sys.rseg= nullptr;
      purge_sys.next_stored= false;
    }

    DBUG_EXECUTE_IF("ib_undo_trunc", ib::info() << "ib_undo_trunc";
                    log_buffer_flush_to_disk();
                    DBUG_SUICIDE(););

    ib::info() << "Truncated " << file->name;
    purge_sys.truncate.last= purge_sys.truncate.current;
    ut_ad(&space == purge_sys.truncate.current);
    purge_sys.truncate.current= nullptr;
  }
}

/***********************************************************************//**
Updates the last not yet purged history log info in rseg when we have purged
a whole undo log. Advances also purge_sys.purge_trx_no past the purged log. */
static void trx_purge_rseg_get_next_history_log(
	ulint*		n_pages_handled)/*!< in/out: number of UNDO pages
					handled */
{
	fil_addr_t	prev_log_addr;
	trx_id_t	trx_no;
	mtr_t		mtr;

	mutex_enter(&purge_sys.rseg->mutex);

	ut_a(purge_sys.rseg->last_page_no != FIL_NULL);

	purge_sys.tail.trx_no = purge_sys.rseg->last_trx_no() + 1;
	purge_sys.tail.undo_no = 0;
	purge_sys.next_stored = false;

	mtr.start();

	const buf_block_t* undo_page = trx_undo_page_get_s_latched(
		page_id_t(purge_sys.rseg->space->id,
			  purge_sys.rseg->last_page_no), &mtr);

	const trx_ulogf_t* log_hdr = undo_page->frame
		+ purge_sys.rseg->last_offset();

	/* Increase the purge page count by one for every handled log */

	(*n_pages_handled)++;

	prev_log_addr = flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE);
	prev_log_addr.boffset = static_cast<uint16_t>(prev_log_addr.boffset
						      - TRX_UNDO_HISTORY_NODE);


	const bool empty = prev_log_addr.page == FIL_NULL;

	if (empty) {
		/* No logs left in the history list */
		purge_sys.rseg->last_page_no = FIL_NULL;
	}

	mutex_exit(&purge_sys.rseg->mutex);
	mtr.commit();

	if (empty) {
		return;
	}

	/* Read the previous log header. */
	mtr.start();

	log_hdr = trx_undo_page_get_s_latched(
		page_id_t(purge_sys.rseg->space->id, prev_log_addr.page),
		&mtr)->frame
		+ prev_log_addr.boffset;

	trx_no = mach_read_from_8(log_hdr + TRX_UNDO_TRX_NO);
	ut_ad(mach_read_from_2(log_hdr + TRX_UNDO_NEEDS_PURGE) <= 1);

	mtr.commit();

	mutex_enter(&purge_sys.rseg->mutex);

	purge_sys.rseg->last_page_no = prev_log_addr.page;
	purge_sys.rseg->set_last_commit(prev_log_addr.boffset, trx_no);

	/* Purge can also produce events, however these are already ordered
	in the rollback segment and any user generated event will be greater
	than the events that Purge produces. ie. Purge can never produce
	events from an empty rollback segment. */

	mutex_enter(&purge_sys.pq_mutex);

	purge_sys.purge_queue.push(*purge_sys.rseg);

	mutex_exit(&purge_sys.pq_mutex);

	mutex_exit(&purge_sys.rseg->mutex);
}

/** Position the purge sys "iterator" on the undo record to use for purging. */
static void trx_purge_read_undo_rec()
{
	uint16_t	offset;
	uint32_t	page_no;
	ib_uint64_t	undo_no;

	purge_sys.hdr_offset = purge_sys.rseg->last_offset();
	page_no = purge_sys.hdr_page_no = purge_sys.rseg->last_page_no;

	if (purge_sys.rseg->needs_purge) {
		mtr_t		mtr;
		mtr.start();
		buf_block_t* undo_page;
		if (trx_undo_rec_t* undo_rec = trx_undo_get_first_rec(
			    *purge_sys.rseg->space, purge_sys.hdr_page_no,
			    purge_sys.hdr_offset, RW_S_LATCH,
			    undo_page, &mtr)) {

			offset = page_offset(undo_rec);
			undo_no = trx_undo_rec_get_undo_no(undo_rec);
			page_no = undo_page->page.id().page_no();
		} else {
			offset = 0;
			undo_no = 0;
		}

		mtr.commit();
	} else {
		offset = 0;
		undo_no = 0;
	}

	purge_sys.offset = offset;
	purge_sys.page_no = page_no;
	purge_sys.tail.undo_no = undo_no;

	purge_sys.next_stored = true;
}

/***********************************************************************//**
Chooses the next undo log to purge and updates the info in purge_sys. This
function is used to initialize purge_sys when the next record to purge is
not known, and also to update the purge system info on the next record when
purge has handled the whole undo log for a transaction. */
static
void
trx_purge_choose_next_log(void)
/*===========================*/
{
	ut_ad(!purge_sys.next_stored);

	if (purge_sys.rseg_iter.set_next()) {
		trx_purge_read_undo_rec();
	} else {
		/* There is nothing to do yet. */
		os_thread_yield();
	}
}

/***********************************************************************//**
Gets the next record to purge and updates the info in the purge system.
@return copy of an undo log record or pointer to the dummy undo log record */
static
trx_undo_rec_t*
trx_purge_get_next_rec(
/*===================*/
	ulint*		n_pages_handled,/*!< in/out: number of UNDO pages
					handled */
	mem_heap_t*	heap)		/*!< in: memory heap where copied */
{
	mtr_t		mtr;

	ut_ad(purge_sys.next_stored);
	ut_ad(purge_sys.tail.trx_no < purge_sys.low_limit_no());

	const ulint space = purge_sys.rseg->space->id;
	const uint32_t page_no = purge_sys.page_no;
	const uint16_t offset = purge_sys.offset;

	if (offset == 0) {
		/* It is the dummy undo log record, which means that there is
		no need to purge this undo log */

		trx_purge_rseg_get_next_history_log(n_pages_handled);

		/* Look for the next undo log and record to purge */

		trx_purge_choose_next_log();

		return(&trx_purge_dummy_rec);
	}

	mtr_start(&mtr);

	buf_block_t* undo_page = trx_undo_page_get_s_latched(
		page_id_t(space, page_no), &mtr);
	buf_block_t* rec2_page = undo_page;

	const trx_undo_rec_t* rec2 = trx_undo_page_get_next_rec(
		undo_page, offset, purge_sys.hdr_page_no, purge_sys.hdr_offset);

	if (rec2 == NULL) {
		rec2 = trx_undo_get_next_rec(rec2_page, offset,
					     purge_sys.hdr_page_no,
					     purge_sys.hdr_offset, &mtr);
	}

	if (rec2 == NULL) {
		mtr_commit(&mtr);

		trx_purge_rseg_get_next_history_log(n_pages_handled);

		/* Look for the next undo log and record to purge */

		trx_purge_choose_next_log();

		mtr_start(&mtr);

		undo_page = trx_undo_page_get_s_latched(
			page_id_t(space, page_no), &mtr);
	} else {
		purge_sys.offset = page_offset(rec2);
		purge_sys.page_no = rec2_page->page.id().page_no();
		purge_sys.tail.undo_no = trx_undo_rec_get_undo_no(rec2);

		if (undo_page != rec2_page) {
			/* We advance to a new page of the undo log: */
			(*n_pages_handled)++;
		}
	}

	trx_undo_rec_t*	rec_copy = trx_undo_rec_copy(undo_page->frame + offset,
						     heap);

	mtr_commit(&mtr);

	return(rec_copy);
}

/********************************************************************//**
Fetches the next undo log record from the history list to purge. It must be
released with the corresponding release function.
@return copy of an undo log record or pointer to trx_purge_dummy_rec,
if the whole undo log can skipped in purge; NULL if none left */
static MY_ATTRIBUTE((warn_unused_result))
trx_undo_rec_t*
trx_purge_fetch_next_rec(
/*=====================*/
	roll_ptr_t*	roll_ptr,	/*!< out: roll pointer to undo record */
	ulint*		n_pages_handled,/*!< in/out: number of UNDO log pages
					handled */
	mem_heap_t*	heap)		/*!< in: memory heap where copied */
{
	if (!purge_sys.next_stored) {
		trx_purge_choose_next_log();

		if (!purge_sys.next_stored) {
			DBUG_PRINT("ib_purge",
				   ("no logs left in the history list"));
			return(NULL);
		}
	}

	if (purge_sys.tail.trx_no >= purge_sys.low_limit_no()) {

		return(NULL);
	}

	/* fprintf(stderr, "Thread %lu purging trx %llu undo record %llu\n",
	os_thread_get_curr_id(), iter->trx_no, iter->undo_no); */

	*roll_ptr = trx_undo_build_roll_ptr(
		/* row_purge_record_func() will later set
		ROLL_PTR_INSERT_FLAG for TRX_UNDO_INSERT_REC */
		false,
		purge_sys.rseg->id,
		purge_sys.page_no, purge_sys.offset);

	/* The following call will advance the stored values of the
	purge iterator. */

	return(trx_purge_get_next_rec(n_pages_handled, heap));
}

/** Run a purge batch.
@param n_purge_threads	number of purge threads
@return number of undo log pages handled in the batch */
static
ulint
trx_purge_attach_undo_recs(ulint n_purge_threads)
{
	que_thr_t*	thr;
	ulint		i;
	ulint		n_pages_handled = 0;
	ulint		n_thrs = UT_LIST_GET_LEN(purge_sys.query->thrs);

	ut_a(n_purge_threads > 0);

	purge_sys.head = purge_sys.tail;

#ifdef UNIV_DEBUG
	i = 0;
	/* Debug code to validate some pre-requisites and reset done flag. */
	for (thr = UT_LIST_GET_FIRST(purge_sys.query->thrs);
	     thr != NULL && i < n_purge_threads;
	     thr = UT_LIST_GET_NEXT(thrs, thr), ++i) {

		purge_node_t*		node;

		/* Get the purge node. */
		node = (purge_node_t*) thr->child;

		ut_ad(que_node_get_type(node) == QUE_NODE_PURGE);
		ut_ad(node->undo_recs.empty());
		ut_ad(!node->in_progress);
		ut_d(node->in_progress = true);
	}

	/* There should never be fewer nodes than threads, the inverse
	however is allowed because we only use purge threads as needed. */
	ut_ad(i == n_purge_threads);
#endif

	/* Fetch and parse the UNDO records. The UNDO records are added
	to a per purge node vector. */
	thr = UT_LIST_GET_FIRST(purge_sys.query->thrs);
	ut_a(n_thrs > 0 && thr != NULL);

	ut_ad(purge_sys.head <= purge_sys.tail);

	i = 0;

	const ulint		batch_size = srv_purge_batch_size;
	std::unordered_map<table_id_t, purge_node_t*> table_id_map;
	mem_heap_empty(purge_sys.heap);

	while (UNIV_LIKELY(srv_undo_sources) || !srv_fast_shutdown) {
		purge_node_t*		node;
		trx_purge_rec_t		purge_rec;

		ut_a(!thr->is_active);

		/* Get the purge node. */
		node = (purge_node_t*) thr->child;
		ut_a(que_node_get_type(node) == QUE_NODE_PURGE);

		/* Track the max {trx_id, undo_no} for truncating the
		UNDO logs once we have purged the records. */

		if (purge_sys.head <= purge_sys.tail) {
			purge_sys.head = purge_sys.tail;
		}

		/* Fetch the next record, and advance the purge_sys.tail. */
		purge_rec.undo_rec = trx_purge_fetch_next_rec(
			&purge_rec.roll_ptr, &n_pages_handled,
			purge_sys.heap);

		if (purge_rec.undo_rec == NULL) {
			break;
		} else if (purge_rec.undo_rec == &trx_purge_dummy_rec) {
			continue;
		}

		table_id_t table_id = trx_undo_rec_get_table_id(
			purge_rec.undo_rec);

		purge_node_t *& table_node = table_id_map[table_id];

		if (table_node) {
			node = table_node;
		} else {
			thr = UT_LIST_GET_NEXT(thrs, thr);

			if (!(++i % n_purge_threads)) {
				thr = UT_LIST_GET_FIRST(
					purge_sys.query->thrs);
			}

			ut_a(thr != NULL);
			table_node = node;
		}

		node->undo_recs.push(purge_rec);

		if (n_pages_handled >= batch_size) {
			break;
		}
	}

	ut_ad(purge_sys.head <= purge_sys.tail);

	return(n_pages_handled);
}

/*******************************************************************//**
Calculate the DML delay required.
@return delay in microseconds or ULINT_MAX */
static
ulint
trx_purge_dml_delay(void)
/*=====================*/
{
	/* Determine how much data manipulation language (DML) statements
	need to be delayed in order to reduce the lagging of the purge
	thread. */
	ulint	delay = 0; /* in microseconds; default: no delay */

	/* If purge lag is set then calculate the new DML delay. */

	if (srv_max_purge_lag > 0) {
		double ratio = static_cast<double>(trx_sys.rseg_history_len) /
			static_cast<double>(srv_max_purge_lag);

		if (ratio > 1.0) {
			/* If the history list length exceeds the
			srv_max_purge_lag, the data manipulation
			statements are delayed by at least 5000
			microseconds. */
			delay = (ulint) ((ratio - .5) * 10000);
		}

		if (delay > srv_max_purge_lag_delay) {
			delay = srv_max_purge_lag_delay;
		}

		MONITOR_SET(MONITOR_DML_PURGE_DELAY, delay);
	}

	return(delay);
}

extern tpool::waitable_task purge_worker_task;

/** Wait for pending purge jobs to complete. */
static void trx_purge_wait_for_workers_to_complete()
{
  bool notify_wait = purge_worker_task.is_running();

  if (notify_wait)
   tpool::tpool_wait_begin();

  purge_worker_task.wait();

  if(notify_wait)
    tpool::tpool_wait_end();

  /* There should be no outstanding tasks as long
  as the worker threads are active. */
  ut_ad(srv_get_task_queue_length() == 0);
}

/**
Run a purge batch.
@param n_tasks   number of purge tasks to submit to the queue
@return number of undo log pages handled in the batch */
ulint trx_purge(ulint n_tasks)
{
	que_thr_t*	thr = NULL;
	ulint		n_pages_handled;

	ut_ad(n_tasks > 0);

	srv_dml_needed_delay = trx_purge_dml_delay();

	purge_sys.clone_oldest_view();

#ifdef UNIV_DEBUG
	if (srv_purge_view_update_only_debug) {
		return(0);
	}
#endif /* UNIV_DEBUG */

	/* Fetch the UNDO recs that need to be purged. */
	n_pages_handled = trx_purge_attach_undo_recs(n_tasks);

	/* Submit tasks to workers queue if using multi-threaded purge. */
	for (ulint i = n_tasks; --i; ) {
		thr = que_fork_scheduler_round_robin(purge_sys.query, thr);
		ut_a(thr);
		srv_que_task_enqueue_low(thr);
		srv_thread_pool->submit_task(&purge_worker_task);
	}

	thr = que_fork_scheduler_round_robin(purge_sys.query, thr);

	que_run_threads(thr);

	trx_purge_wait_for_workers_to_complete();

	MONITOR_INC_VALUE(MONITOR_PURGE_INVOKED, 1);
	MONITOR_INC_VALUE(MONITOR_PURGE_N_PAGE_HANDLED, n_pages_handled);

	return(n_pages_handled);
}
