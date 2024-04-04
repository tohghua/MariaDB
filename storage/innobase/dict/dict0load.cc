/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2023, MariaDB Corporation.

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
@file dict/dict0load.cc
Loads to the memory cache database object definitions
from dictionary tables

Created 4/24/1996 Heikki Tuuri
*******************************************************/

#include "dict0load.h"

#include "log.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "dict0dict.h"
#include "dict0stats.h"
#include "ibuf0ibuf.h"
#include "fsp0file.h"
#include "fts0priv.h"
#include "mach0data.h"
#include "page0page.h"
#include "rem0cmp.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "fts0opt.h"
#include "row0vers.h"
#include "ha_innodb.h"

/** Loads a table definition and also all its index definitions.

Loads those foreign key constraints whose referenced table is already in
dictionary cache.

@param[in]	name		Table name in the db/tablename format
@param[in]	ignore_err	Error to be ignored when loading table
				and its index definition
@return table, possibly with file_unreadable flag set
@retval nullptr if the table does not exist */
static dict_table_t *dict_load_table_one(const span<const char> &name,
                                         dict_err_ignore_t ignore_err);

/** Load an index definition from a SYS_INDEXES record to dict_index_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_index_low(
	byte*		table_id,	/*!< in/out: table id (8 bytes),
					an "in" value if mtr
					and "out" when !mtr */
	bool		uncommitted,	/*!< in: false=READ COMMITTED,
					true=READ UNCOMMITTED */
	mem_heap_t*	heap,		/*!< in/out: temporary memory heap */
	const rec_t*	rec,		/*!< in: SYS_INDEXES record */
	mtr_t*		mtr,		/*!< in/out: mini-transaction,
					or nullptr if a pre-allocated
					*index is to be filled in */
	dict_table_t*	table,		/*!< in/out: table, or NULL */
	dict_index_t**	index);		/*!< out,own: index, or NULL */

/** Load a table column definition from a SYS_COLUMNS record to dict_table_t.
@param table           table, or nullptr if the output will be in column
@param use_uncommitted 0=READ COMMITTED, 1=detect, 2=READ UNCOMMITTED
@param heap            memory heap for temporary storage
@param column          pointer to output buffer, or nullptr if table!=nullptr
@param table_id        table identifier
@param col_name        column name
@param rec             SYS_COLUMNS record
@param mtr             mini-transaction
@param nth_v_col       nullptr, or pointer to a counter of virtual columns
@return error message
@retval nullptr on success */
static const char *dict_load_column_low(dict_table_t *table,
                                        unsigned use_uncommitted,
                                        mem_heap_t *heap, dict_col_t *column,
                                        table_id_t *table_id,
                                        const char **col_name,
                                        const rec_t *rec,
                                        mtr_t *mtr,
                                        ulint *nth_v_col);

/** Load a virtual column "mapping" (to base columns) information
from a SYS_VIRTUAL record
@param[in,out]	table		table
@param[in]	uncommitted	false=READ COMMITTED, true=READ UNCOMMITTED
@param[in,out]	column		mapped base column's dict_column_t
@param[in,out]	table_id	table id
@param[in,out]	pos		virtual column position
@param[in,out]	base_pos	base column position
@param[in]	rec		SYS_VIRTUAL record
@return	error message
@retval	NULL on success */
static
const char*
dict_load_virtual_low(
	dict_table_t*	table,
	bool		uncommitted,
	dict_col_t**	column,
	table_id_t*	table_id,
	ulint*		pos,
	ulint*		base_pos,
	const rec_t*	rec);

/** Load an index field definition from a SYS_FIELDS record to dict_index_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_field_low(
	byte*		index_id,	/*!< in/out: index id (8 bytes)
					an "in" value if index != NULL
					and "out" if index == NULL */
	bool		uncommitted,	/*!< in: false=READ COMMITTED,
					true=READ UNCOMMITTED */
	dict_index_t*	index,		/*!< in/out: index, could be NULL
					if we just populate a dict_field_t
					struct with information from
					a SYS_FIELDS record */
	dict_field_t*	sys_field,	/*!< out: dict_field_t to be
					filled */
	ulint*		pos,		/*!< out: Field position */
	byte*		last_index_id,	/*!< in: last index id */
	mem_heap_t*	heap,		/*!< in/out: memory heap
					for temporary storage */
	mtr_t*		mtr,		/*!< in/out: mini-transaction */
	const rec_t*	rec);		/*!< in: SYS_FIELDS record */

#ifdef UNIV_DEBUG
/****************************************************************//**
Compare the name of an index column.
@return TRUE if the i'th column of index is 'name'. */
static
ibool
name_of_col_is(
/*===========*/
	const dict_table_t*	table,	/*!< in: table */
	const dict_index_t*	index,	/*!< in: index */
	ulint			i,	/*!< in: index field offset */
	const char*		name)	/*!< in: name to compare to */
{
	ulint	tmp = dict_col_get_no(dict_field_get_col(
					      dict_index_get_nth_field(
						      index, i)));

	return(strcmp(name, dict_table_get_col_name(table, tmp)) == 0);
}
#endif /* UNIV_DEBUG */

/********************************************************************//**
This function gets the next system table record as it scans the table.
@return the next record if found, NULL if end of scan */
static
const rec_t*
dict_getnext_system_low(
/*====================*/
	btr_pcur_t*	pcur,		/*!< in/out: persistent cursor to the
					record*/
	mtr_t*		mtr)		/*!< in: the mini-transaction */
{
	rec_t*	rec = NULL;

	while (!rec) {
		btr_pcur_move_to_next_user_rec(pcur, mtr);

		rec = btr_pcur_get_rec(pcur);

		if (!btr_pcur_is_on_user_rec(pcur)) {
			/* end of index */
			btr_pcur_close(pcur);

			return(NULL);
		}
	}

	/* Get a record, let's save the position */
	btr_pcur_store_position(pcur, mtr);

	return(rec);
}

/********************************************************************//**
This function opens a system table, and returns the first record.
@return first record of the system table */
const rec_t*
dict_startscan_system(
/*==================*/
	btr_pcur_t*	pcur,		/*!< out: persistent cursor to
					the record */
	mtr_t*		mtr,		/*!< in: the mini-transaction */
	dict_table_t*	table)		/*!< in: system table */
{
  btr_pcur_init(pcur);
  if (pcur->open_leaf(true, table->indexes.start, BTR_SEARCH_LEAF, mtr) !=
      DB_SUCCESS)
    return nullptr;
  const rec_t *rec;
  do
    rec= dict_getnext_system_low(pcur, mtr);
  while (rec && rec_get_deleted_flag(rec, 0));
  return rec;
}

/********************************************************************//**
This function gets the next system table record as it scans the table.
@return the next record if found, NULL if end of scan */
const rec_t*
dict_getnext_system(
/*================*/
	btr_pcur_t*	pcur,		/*!< in/out: persistent cursor
					to the record */
	mtr_t*		mtr)		/*!< in: the mini-transaction */
{
  const rec_t *rec=nullptr;
  if (pcur->restore_position(BTR_SEARCH_LEAF, mtr) != btr_pcur_t::CORRUPTED)
    do
      rec= dict_getnext_system_low(pcur, mtr);
    while (rec && rec_get_deleted_flag(rec, 0));
  return rec;
}

/********************************************************************//**
This function parses a SYS_INDEXES record and populate a dict_index_t
structure with the information from the record. For detail information
about SYS_INDEXES fields, please refer to dict_boot() function.
@return error message, or NULL on success */
const char*
dict_process_sys_indexes_rec(
/*=========================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_INDEXES rec */
	dict_index_t*	index,		/*!< out: index to be filled */
	table_id_t*	table_id)	/*!< out: index table id */
{
  byte buf[8];

  ut_d(index->is_dummy = true);
  ut_d(index->in_instant_init = false);

  /* Parse the record, and get "dict_index_t" struct filled */
  const char *err_msg= dict_load_index_low(buf, false, heap, rec,
                                           nullptr, nullptr, &index);
  *table_id= mach_read_from_8(buf);
  return err_msg;
}

/********************************************************************//**
This function parses a SYS_COLUMNS record and populate a dict_column_t
structure with the information from the record.
@return error message, or NULL on success */
const char*
dict_process_sys_columns_rec(
/*=========================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_COLUMNS rec */
	dict_col_t*	column,		/*!< out: dict_col_t to be filled */
	table_id_t*	table_id,	/*!< out: table id */
	const char**	col_name,	/*!< out: column name */
	ulint*		nth_v_col)	/*!< out: if virtual col, this is
					record's sequence number */
{
	const char*	err_msg;

	/* Parse the record, and get "dict_col_t" struct filled */
	err_msg = dict_load_column_low(NULL, 0, heap, column,
				       table_id, col_name, rec, nullptr,
				       nth_v_col);

	return(err_msg);
}

/** This function parses a SYS_VIRTUAL record and extracts virtual column
information
@param[in]	rec		current SYS_COLUMNS rec
@param[in,out]	table_id	table id
@param[in,out]	pos		virtual column position
@param[in,out]	base_pos	base column position
@return error message, or NULL on success */
const char*
dict_process_sys_virtual_rec(
	const rec_t*	rec,
	table_id_t*	table_id,
	ulint*		pos,
	ulint*		base_pos)
{
  return dict_load_virtual_low(nullptr, false, nullptr, table_id,
                               pos, base_pos, rec);
}

/********************************************************************//**
This function parses a SYS_FIELDS record and populates a dict_field_t
structure with the information from the record.
@return error message, or NULL on success */
const char*
dict_process_sys_fields_rec(
/*========================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_FIELDS rec */
	dict_field_t*	sys_field,	/*!< out: dict_field_t to be
					filled */
	ulint*		pos,		/*!< out: Field position */
	index_id_t*	index_id,	/*!< out: current index id */
	index_id_t	last_id)	/*!< in: previous index id */
{
	byte		buf[8];
	byte		last_index_id[8];
	const char*	err_msg;

	mach_write_to_8(last_index_id, last_id);

	err_msg = dict_load_field_low(buf, false, nullptr, sys_field,
				      pos, last_index_id, heap, nullptr, rec);

	*index_id = mach_read_from_8(buf);

	return(err_msg);

}

/** Check the validity of a SYS_TABLES record
Make sure the fields are the right length and that they
do not contain invalid contents.
@param[in]	rec	SYS_TABLES record
@return error message, or NULL on success */
static
const char*
dict_sys_tables_rec_check(
	const rec_t*	rec)
{
	const byte*	field;
	ulint		len;

	ut_ad(dict_sys.locked());

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_TABLES) {
		return("wrong number of columns in SYS_TABLES record");
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
err_len:
		return("incorrect column length in SYS_TABLES");
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(rec, DICT_FLD__SYS_TABLES__ID, &len);
	if (len != 8) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__N_COLS, &len);
	if (field == NULL || len != 4) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(rec, DICT_FLD__SYS_TABLES__TYPE, &len);
	if (len != 4) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__MIX_ID, &len);
	if (len != 8) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__MIX_LEN, &len);
	if (field == NULL || len != 4) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__CLUSTER_ID, &len);
	if (len != UNIV_SQL_NULL) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__SPACE, &len);
	if (field == NULL || len != 4) {
		goto err_len;
	}

	return(NULL);
}

/** Check if SYS_TABLES.TYPE is valid
@param[in]	type		SYS_TABLES.TYPE
@param[in]	not_redundant	whether ROW_FORMAT=REDUNDANT is not used
@return	whether the SYS_TABLES.TYPE value is valid */
static
bool
dict_sys_tables_type_valid(ulint type, bool not_redundant)
{
	/* The DATA_DIRECTORY flag can be assigned fully independently
	of all other persistent table flags. */
	type &= ~DICT_TF_MASK_DATA_DIR;

	if (type == 1) {
		return(true); /* ROW_FORMAT=REDUNDANT or ROW_FORMAT=COMPACT */
	}

	if (!(type & 1)) {
		/* For ROW_FORMAT=REDUNDANT and ROW_FORMAT=COMPACT,
		SYS_TABLES.TYPE=1. Else, it is the same as
		dict_table_t::flags, and the least significant bit
		would be set. So, the bit never can be 0. */
		return(false);
	}

	if (!not_redundant) {
		/* SYS_TABLES.TYPE must be 1 or 1|DICT_TF_MASK_NO_ROLLBACK
		for ROW_FORMAT=REDUNDANT. */
		return !(type & ~(1U | DICT_TF_MASK_NO_ROLLBACK));
	}

	if (type >= 1U << DICT_TF_POS_UNUSED) {
		/* Some unknown bits are set. */
		return(false);
	}

	return(dict_tf_is_valid_not_redundant(type));
}

/** Convert SYS_TABLES.TYPE to dict_table_t::flags.
@param[in]	type		SYS_TABLES.TYPE
@param[in]	not_redundant	whether ROW_FORMAT=REDUNDANT is not used
@return	table flags */
static
uint32_t dict_sys_tables_type_to_tf(uint32_t type, bool not_redundant)
{
	ut_ad(dict_sys_tables_type_valid(type, not_redundant));
	uint32_t flags = not_redundant ? 1 : 0;

	/* ZIP_SSIZE, ATOMIC_BLOBS, DATA_DIR, PAGE_COMPRESSION,
	PAGE_COMPRESSION_LEVEL are the same. */
	flags |= type & (DICT_TF_MASK_ZIP_SSIZE
			 | DICT_TF_MASK_ATOMIC_BLOBS
			 | DICT_TF_MASK_DATA_DIR
			 | DICT_TF_MASK_PAGE_COMPRESSION
			 | DICT_TF_MASK_PAGE_COMPRESSION_LEVEL
			 | DICT_TF_MASK_NO_ROLLBACK);

	ut_ad(dict_tf_is_valid(flags));
	return(flags);
}

/** Outcome of dict_sys_tables_rec_read() */
enum table_read_status { READ_OK= 0, READ_ERROR, READ_NOT_FOUND };

/** Read and return 5 integer fields from a SYS_TABLES record.
@param[in]	rec		A record of SYS_TABLES
@param[in]	uncommitted	true=use READ UNCOMMITTED, false=READ COMMITTED
@param[in]	mtr		mini-transaction
@param[out]	table_id	Pointer to the table_id for this table
@param[out]	space_id	Pointer to the space_id for this table
@param[out]	n_cols		Pointer to number of columns for this table.
@param[out]	flags		Pointer to table flags
@param[out]	flags2		Pointer to table flags2
@param[out]	trx_id		DB_TRX_ID of the committed SYS_TABLES record,
				or nullptr to perform READ UNCOMMITTED
@return whether the record was read correctly */
MY_ATTRIBUTE((warn_unused_result))
static
table_read_status
dict_sys_tables_rec_read(
	const rec_t*		rec,
	bool			uncommitted,
	mtr_t*			mtr,
	table_id_t*		table_id,
	uint32_t*		space_id,
	uint32_t*		n_cols,
	uint32_t*		flags,
	uint32_t*		flags2,
	trx_id_t*		trx_id)
{
	const byte*	field;
	ulint		len;
	mem_heap_t*	heap = nullptr;

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__DB_TRX_ID, &len);
	ut_ad(len == 6 || len == UNIV_SQL_NULL);
	trx_id_t id = len == 6 ? trx_read_trx_id(field) : 0;
	if (id && !uncommitted && trx_sys.find(nullptr, id, false)) {
		const auto savepoint = mtr->get_savepoint();
		heap = mem_heap_create(1024);
		dict_index_t* index = UT_LIST_GET_FIRST(
			dict_sys.sys_tables->indexes);
		rec_offs* offsets = rec_get_offsets(
			rec, index, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, mtr, index, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr->rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!rec) {
			mem_heap_free(heap);
			return READ_NOT_FOUND;
		}
		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_TABLES__DB_TRX_ID, &len);
		if (UNIV_UNLIKELY(len != 6)) {
			mem_heap_free(heap);
			return READ_ERROR;
		}
		id = trx_read_trx_id(field);
	}

	if (rec_get_deleted_flag(rec, 0)) {
		ut_ad(id);
		if (trx_id) {
			return READ_NOT_FOUND;
		}
	}

	if (trx_id) {
		*trx_id = id;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__ID, &len);
	ut_ad(len == 8);
	*table_id = static_cast<table_id_t>(mach_read_from_8(field));

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__SPACE, &len);
	ut_ad(len == 4);
	*space_id = mach_read_from_4(field);

	/* Read the 4 byte flags from the TYPE field */
	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__TYPE, &len);
	ut_a(len == 4);
	uint32_t type = mach_read_from_4(field);

	/* Handle MDEV-12873 InnoDB SYS_TABLES.TYPE incompatibility
	for PAGE_COMPRESSED=YES in MariaDB 10.2.2 to 10.2.6.

	MariaDB 10.2.2 introduced the SHARED_SPACE flag from MySQL 5.7,
	shifting the flags PAGE_COMPRESSION, PAGE_COMPRESSION_LEVEL,
	ATOMIC_WRITES (repurposed to NO_ROLLBACK in 10.3.1) by one bit.
	The SHARED_SPACE flag would always
	be written as 0 by MariaDB, because MariaDB does not support
	CREATE TABLESPACE or CREATE TABLE...TABLESPACE for InnoDB.

	So, instead of the bits AALLLLCxxxxxxx we would have
	AALLLLC0xxxxxxx if the table was created with MariaDB 10.2.2
	to 10.2.6. (AA=ATOMIC_WRITES, LLLL=PAGE_COMPRESSION_LEVEL,
	C=PAGE_COMPRESSED, xxxxxxx=7 bits that were not moved.)

	The case LLLLC=00000 is not a problem. The problem is the case
	AALLLL10DB00001 where D is the (mostly ignored) DATA_DIRECTORY
	flag and B is the ATOMIC_BLOBS flag (1 for ROW_FORMAT=DYNAMIC
	and 0 for ROW_FORMAT=COMPACT in this case). Other low-order
	bits must be so, because PAGE_COMPRESSED=YES is only allowed
	for ROW_FORMAT=DYNAMIC and ROW_FORMAT=COMPACT, not for
	ROW_FORMAT=REDUNDANT or ROW_FORMAT=COMPRESSED.

	Starting with MariaDB 10.2.4, the flags would be
	00LLLL10DB00001, because ATOMIC_WRITES is always written as 0.

	We will concentrate on the PAGE_COMPRESSION_LEVEL and
	PAGE_COMPRESSED=YES. PAGE_COMPRESSED=NO implies
	PAGE_COMPRESSION_LEVEL=0, and in that case all the affected
	bits will be 0. For PAGE_COMPRESSED=YES, the values 1..9 are
	allowed for PAGE_COMPRESSION_LEVEL. That is, we must interpret
	the bits AALLLL10DB00001 as AALLLL1DB00001.

	If someone created a table in MariaDB 10.2.2 or 10.2.3 with
	the attribute ATOMIC_WRITES=OFF (value 2) and without
	PAGE_COMPRESSED=YES or PAGE_COMPRESSION_LEVEL, that should be
	rejected. The value ATOMIC_WRITES=ON (1) would look like
	ATOMIC_WRITES=OFF, but it would be ignored starting with
	MariaDB 10.2.4. */
	compile_time_assert(DICT_TF_POS_PAGE_COMPRESSION == 7);
	compile_time_assert(DICT_TF_POS_UNUSED == 14);

	if ((type & 0x19f) != 0x101) {
		/* The table cannot have been created with MariaDB
		10.2.2 to 10.2.6, because they would write the
		low-order bits of SYS_TABLES.TYPE as 0b10xx00001 for
		PAGE_COMPRESSED=YES. No adjustment is applicable. */
	} else if (type >= 3 << 13) {
		/* 10.2.2 and 10.2.3 write ATOMIC_WRITES less than 3,
		and no other flags above that can be set for the
		SYS_TABLES.TYPE to be in the 10.2.2..10.2.6 format.
		This would in any case be invalid format for 10.2 and
		earlier releases. */
		ut_ad(!dict_sys_tables_type_valid(type, true));
	} else {
		/* SYS_TABLES.TYPE is of the form AALLLL10DB00001.  We
		must still validate that the LLLL bits are between 0
		and 9 before we can discard the extraneous 0 bit. */
		ut_ad(!DICT_TF_GET_PAGE_COMPRESSION(type));

		if ((((type >> 9) & 0xf) - 1) < 9) {
			ut_ad(DICT_TF_GET_PAGE_COMPRESSION_LEVEL(type) & 1);

			type = (type & 0x7fU) | (type >> 1 & ~0x7fU);

			ut_ad(DICT_TF_GET_PAGE_COMPRESSION(type));
			ut_ad(DICT_TF_GET_PAGE_COMPRESSION_LEVEL(type) >= 1);
			ut_ad(DICT_TF_GET_PAGE_COMPRESSION_LEVEL(type) <= 9);
		} else {
			ut_ad(!dict_sys_tables_type_valid(type, true));
		}
	}

	/* The low order bit of SYS_TABLES.TYPE is always set to 1. But in
	dict_table_t::flags the low order bit is used to determine if the
	ROW_FORMAT=REDUNDANT (0) or anything else (1).
	Read the 4 byte N_COLS field and look at the high order bit.  It
	should be set for COMPACT and later.  It should not be set for
	REDUNDANT. */
	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__N_COLS, &len);
	ut_a(len == 4);
	*n_cols = mach_read_from_4(field);

	const bool not_redundant = 0 != (*n_cols & DICT_N_COLS_COMPACT);

	if (!dict_sys_tables_type_valid(type, not_redundant)) {
		sql_print_error("InnoDB: Table %.*s in InnoDB"
				" data dictionary contains invalid flags."
				" SYS_TABLES.TYPE=" UINT32PF
				" SYS_TABLES.N_COLS=" UINT32PF,
				int(rec_get_field_start_offs(rec, 1)), rec,
				type, *n_cols);
err_exit:
		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
		return READ_ERROR;
	}

	*flags = dict_sys_tables_type_to_tf(type, not_redundant);

	/* For tables created before MySQL 4.1, there may be
	garbage in SYS_TABLES.MIX_LEN where flags2 are found. Such tables
	would always be in ROW_FORMAT=REDUNDANT which do not have the
	high bit set in n_cols, and flags would be zero.
	MySQL 4.1 was the first version to support innodb_file_per_table,
	that is, *space_id != 0. */
	if (not_redundant || *space_id != 0 || *n_cols & DICT_N_COLS_COMPACT
	    || fil_system.sys_space->full_crc32()) {

		/* Get flags2 from SYS_TABLES.MIX_LEN */
		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_TABLES__MIX_LEN, &len);
		*flags2 = mach_read_from_4(field);

		if (!dict_tf2_is_valid(*flags, *flags2)) {
			sql_print_error("InnoDB: Table %.*s in InnoDB"
					" data dictionary"
					" contains invalid flags."
					" SYS_TABLES.TYPE=" UINT32PF
					" SYS_TABLES.MIX_LEN=" UINT32PF,
					int(rec_get_field_start_offs(rec, 1)),
					rec,
					type, *flags2);
			goto err_exit;
		}

		/* DICT_TF2_FTS will be set when indexes are being loaded */
		*flags2 &= ~DICT_TF2_FTS;

		/* Now that we have used this bit, unset it. */
		*n_cols &= ~DICT_N_COLS_COMPACT;
	} else {
		*flags2 = 0;
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return READ_OK;
}

/** @return SELECT MAX(space) FROM sys_tables */
static uint32_t dict_find_max_space_id(btr_pcur_t *pcur, mtr_t *mtr)
{
  uint32_t max_space_id= 0;

  for (const rec_t *rec= dict_startscan_system(pcur, mtr, dict_sys.sys_tables);
       rec; rec= dict_getnext_system_low(pcur, mtr))
    if (!dict_sys_tables_rec_check(rec))
    {
      ulint len;
      const byte *field=
        rec_get_nth_field_old(rec, DICT_FLD__SYS_TABLES__SPACE, &len);
      ut_ad(len == 4);
      max_space_id= std::max(max_space_id, mach_read_from_4(field));
    }

  return max_space_id;
}

/** Check MAX(SPACE) FROM SYS_TABLES and store it in fil_system.
Open each data file if an encryption plugin has been loaded.

@param spaces  set of tablespace files to open
@param upgrade whether we need to invoke ibuf_upgrade() */
void dict_load_tablespaces(const std::set<uint32_t> *spaces, bool upgrade)
{
	uint32_t	max_space_id = 0;
	btr_pcur_t	pcur;
	mtr_t		mtr;

	mtr.start();

	dict_sys.lock(SRW_LOCK_CALL);

	if (!spaces && !upgrade
	    && !encryption_key_id_exists(FIL_DEFAULT_ENCRYPTION_KEY)) {
		max_space_id = dict_find_max_space_id(&pcur, &mtr);
		goto done;
	}

	for (const rec_t *rec = dict_startscan_system(&pcur, &mtr,
						      dict_sys.sys_tables);
	     rec; rec = dict_getnext_system_low(&pcur, &mtr)) {
		ulint		len;
		table_id_t	table_id;
		uint32_t	space_id;
		uint32_t	n_cols;
		uint32_t	flags;
		uint32_t	flags2;

		/* If a table record is not useable, ignore it and continue
		on to the next record. Error messages were logged. */
		if (dict_sys_tables_rec_check(rec)) {
			continue;
		}

		const char *field = reinterpret_cast<const char*>(
			rec_get_nth_field_old(rec, DICT_FLD__SYS_TABLES__NAME,
					      &len));

		DBUG_PRINT("dict_check_sys_tables",
			   ("name: %*.s", static_cast<int>(len), field));

		if (dict_sys_tables_rec_read(rec, false,
					     &mtr, &table_id, &space_id,
					     &n_cols, &flags, &flags2, nullptr)
		    != READ_OK
		    || space_id == TRX_SYS_SPACE) {
			continue;
		}

		/* For tables or partitions using .ibd files, the flag
		DICT_TF2_USE_FILE_PER_TABLE was not set in MIX_LEN
		before MySQL 5.6.5. The flag should not have been
		introduced in persistent storage. MariaDB will keep
		setting the flag when writing SYS_TABLES entries for
		newly created or rebuilt tables or partitions, but
		will otherwise ignore the flag. */

		if (fil_space_for_table_exists_in_mem(space_id, flags)) {
			continue;
		}

		if (spaces && spaces->find(uint32_t(space_id))
                    == spaces->end()) {
			continue;
		}

		if (flags2 & DICT_TF2_DISCARDED) {
			sql_print_information("InnoDB: Ignoring tablespace"
					      " for %.*s because "
					      "the DISCARD flag is set",
					      static_cast<int>(len), field);
			continue;
		}

		const span<const char> name{field, len};

		char*	filepath = fil_make_filepath(nullptr, name,
						     IBD, false);

		const bool not_dropped{!rec_get_deleted_flag(rec, 0)};

		/* Check that the .ibd file exists. */
		if (fil_ibd_open(not_dropped, FIL_TYPE_TABLESPACE,
				 space_id, dict_tf_to_fsp_flags(flags),
				 name, filepath)) {
		} else if (!not_dropped) {
		} else if (srv_operation == SRV_OPERATION_NORMAL
			   && srv_start_after_restore
			   && srv_force_recovery < SRV_FORCE_NO_BACKGROUND
			   && dict_table_t::is_temporary_name(filepath)) {
			/* Mariabackup will not copy files whose
			names start with #sql-. This table ought to
			be dropped by drop_garbage_tables_after_restore()
			a little later. */
		} else {
			sql_print_warning("InnoDB: Ignoring tablespace for"
					  " %.*s because it"
					  " could not be opened.",
					  static_cast<int>(len), field);
		}

		max_space_id = ut_max(max_space_id, space_id);

		ut_free(filepath);
	}

done:
	mtr.commit();

	fil_set_max_space_id_if_bigger(max_space_id);

	dict_sys.unlock();
}

/** Error message for a delete-marked record in dict_load_column_low() */
static const char *dict_load_column_del= "delete-marked record in SYS_COLUMNS";
/** Error message for a missing record in dict_load_column_low() */
static const char *dict_load_column_none= "SYS_COLUMNS record not found";
/** Message for incomplete instant ADD/DROP in dict_load_column_low() */
static const char *dict_load_column_instant= "incomplete instant ADD/DROP";

/** Load a table column definition from a SYS_COLUMNS record to dict_table_t.
@param table           table, or nullptr if the output will be in column
@param use_uncommitted 0=READ COMMITTED, 1=detect, 2=READ UNCOMMITTED
@param heap            memory heap for temporary storage
@param column          pointer to output buffer, or nullptr if table!=nullptr
@param table_id        table identifier
@param col_name        column name
@param rec             SYS_COLUMNS record
@param mtr             mini-transaction
@param nth_v_col       nullptr, or pointer to a counter of virtual columns
@return error message
@retval nullptr on success */
static const char *dict_load_column_low(dict_table_t *table,
                                        unsigned use_uncommitted,
                                        mem_heap_t *heap, dict_col_t *column,
                                        table_id_t *table_id,
                                        const char **col_name,
                                        const rec_t *rec,
                                        mtr_t *mtr,
                                        ulint *nth_v_col)
{
	char*		name;
	const byte*	field;
	ulint		len;
	ulint		mtype;
	ulint		prtype;
	ulint		col_len;
	ulint		pos;
	ulint		num_base;

	ut_ad(!table == !!column);

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_COLUMNS) {
		return("wrong number of columns in SYS_COLUMNS record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__TABLE_ID, &len);
	if (len != 8) {
err_len:
		return("incorrect column length in SYS_COLUMNS");
	}

	if (table_id) {
		*table_id = mach_read_from_8(field);
	} else if (table->id != mach_read_from_8(field)) {
		return dict_load_column_none;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__POS, &len);
	if (len != 4) {
		goto err_len;
	}

	pos = mach_read_from_4(field);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	const trx_id_t trx_id = trx_read_trx_id(field);

	if (trx_id && mtr && use_uncommitted < 2
	    && trx_sys.find(nullptr, trx_id, false)) {
		if (use_uncommitted) {
			return dict_load_column_instant;
		}
		const auto savepoint = mtr->get_savepoint();
		dict_index_t* index = UT_LIST_GET_FIRST(
			dict_sys.sys_columns->indexes);
		rec_offs* offsets = rec_get_offsets(
			rec, index, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, mtr, index, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr->rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!old_vers) {
			return dict_load_column_none;
		}
		ut_ad(!rec_get_deleted_flag(rec, 0));
	}

	if (rec_get_deleted_flag(rec, 0)) {
		ut_ad(trx_id);
		return dict_load_column_del;
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_COLUMNS__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}

	*col_name = name = mem_heap_strdupl(heap, (const char*) field, len);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__MTYPE, &len);
	if (len != 4) {
		goto err_len;
	}

	mtype = mach_read_from_4(field);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__PRTYPE, &len);
	if (len != 4) {
		goto err_len;
	}
	prtype = mach_read_from_4(field);

	if (dtype_get_charset_coll(prtype) == 0
	    && dtype_is_string_type(mtype)) {
		/* The table was created with < 4.1.2. */

		if (dtype_is_binary_string_type(mtype, prtype)) {
			/* Use the binary collation for
			string columns of binary type. */

			prtype = dtype_form_prtype(
				prtype,
				DATA_MYSQL_BINARY_CHARSET_COLL);
		} else {
			/* Use the default charset for
			other than binary columns. */

			prtype = dtype_form_prtype(
				prtype,
				default_charset_info->number);
		}
	}

	if (table && table->n_def != pos && !(prtype & DATA_VIRTUAL)) {
		return("SYS_COLUMNS.POS mismatch");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__LEN, &len);
	if (len != 4) {
		goto err_len;
	}
	col_len = mach_read_from_4(field);
	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__PREC, &len);
	if (len != 4) {
		goto err_len;
	}
	num_base = mach_read_from_4(field);

	if (table) {
		if (prtype & DATA_VIRTUAL) {
#ifdef UNIV_DEBUG
			dict_v_col_t*	vcol =
#endif
			dict_mem_table_add_v_col(
				table, heap, name, mtype,
				prtype, col_len,
				dict_get_v_col_mysql_pos(pos), num_base);
			ut_ad(vcol->v_pos == dict_get_v_col_pos(pos));
		} else {
			ut_ad(num_base == 0);
			dict_mem_table_add_col(table, heap, name, mtype,
					       prtype, col_len);
		}

		if (trx_id > table->def_trx_id) {
			table->def_trx_id = trx_id;
		}
	} else {
		dict_mem_fill_column_struct(column, pos, mtype,
					    prtype, col_len);
	}

	/* Report the virtual column number */
	if ((prtype & DATA_VIRTUAL) && nth_v_col != NULL) {
		*nth_v_col = dict_get_v_col_pos(pos);
	}

	return(NULL);
}

/** Error message for a delete-marked record in dict_load_virtual_low() */
static const char *dict_load_virtual_del= "delete-marked record in SYS_VIRTUAL";
static const char *dict_load_virtual_none= "SYS_VIRTUAL record not found";

/** Load a virtual column "mapping" (to base columns) information
from a SYS_VIRTUAL record
@param[in,out]	table		table
@param[in]	uncommitted	false=READ COMMITTED, true=READ UNCOMMITTED
@param[in,out]	column		mapped base column's dict_column_t
@param[in,out]	table_id	table id
@param[in,out]	pos		virtual column position
@param[in,out]	base_pos	base column position
@param[in]	rec		SYS_VIRTUAL record
@return	error message
@retval	NULL on success */
static
const char*
dict_load_virtual_low(
	dict_table_t*	table,
	bool		uncommitted,
	dict_col_t**	column,
	table_id_t*	table_id,
	ulint*		pos,
	ulint*		base_pos,
	const rec_t*	rec)
{
	const byte*	field;
	ulint		len;
	ulint		base;

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_VIRTUAL) {
		return("wrong number of columns in SYS_VIRTUAL record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_VIRTUAL__TABLE_ID, &len);
	if (len != 8) {
err_len:
		return("incorrect column length in SYS_VIRTUAL");
	}

	if (table_id != NULL) {
		*table_id = mach_read_from_8(field);
	} else if (table->id != mach_read_from_8(field)) {
		return dict_load_virtual_none;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_VIRTUAL__POS, &len);
	if (len != 4) {
		goto err_len;
	}

	if (pos != NULL) {
		*pos = mach_read_from_4(field);
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_VIRTUAL__BASE_POS, &len);
	if (len != 4) {
		goto err_len;
	}

	base = mach_read_from_4(field);

	if (base_pos != NULL) {
		*base_pos = base;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_VIRTUAL__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_VIRTUAL__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	const trx_id_t trx_id = trx_read_trx_id(field);

	if (trx_id && column && !uncommitted
	    && trx_sys.find(nullptr, trx_id, false)) {
		if (!rec_get_deleted_flag(rec, 0)) {
			return dict_load_virtual_none;
		}
	} else if (rec_get_deleted_flag(rec, 0)) {
		ut_ad(trx_id != 0);
		return dict_load_virtual_del;
	}

	if (column != NULL) {
		*column = dict_table_get_nth_col(table, base);
	}

	return(NULL);
}

/** Load the definitions for table columns.
@param table           table
@param use_uncommitted 0=READ COMMITTED, 1=detect, 2=READ UNCOMMITTED
@param heap            memory heap for temporary storage
@return error code
@retval DB_SUCCESS on success
@retval DB_SUCCESS_LOCKED_REC on success if use_uncommitted=1
and instant ADD/DROP/reorder was detected */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static dberr_t dict_load_columns(dict_table_t *table, unsigned use_uncommitted,
                                 mem_heap_t *heap)
{
	btr_pcur_t	pcur;
	mtr_t		mtr;
	ulint		n_skipped = 0;

	ut_ad(dict_sys.locked());

	mtr.start();

	dict_index_t* sys_index = dict_sys.sys_columns->indexes.start;
	ut_ad(!dict_sys.sys_columns->not_redundant());

	ut_ad(name_of_col_is(dict_sys.sys_columns, sys_index,
			     DICT_FLD__SYS_COLUMNS__NAME, "NAME"));
	ut_ad(name_of_col_is(dict_sys.sys_columns, sys_index,
			     DICT_FLD__SYS_COLUMNS__PREC, "PREC"));

	dfield_t dfield;
	dtuple_t tuple{
		0,1,1,&dfield,0,nullptr
#ifdef UNIV_DEBUG
		, DATA_TUPLE_MAGIC_N
#endif
	};
	byte table_id[8];
	mach_write_to_8(table_id, table->id);
	dfield_set_data(&dfield, table_id, 8);
	dict_index_copy_types(&tuple, sys_index, 1);
	pcur.btr_cur.page_cur.index = sys_index;

	dberr_t err = btr_pcur_open_on_user_rec(&tuple,
						BTR_SEARCH_LEAF, &pcur, &mtr);
	if (err != DB_SUCCESS) {
		goto func_exit;
	}

	ut_ad(table->n_t_cols == static_cast<ulint>(
	      table->n_cols) + static_cast<ulint>(table->n_v_cols));

	for (ulint i = 0;
	     i + DATA_N_SYS_COLS < table->n_t_cols + n_skipped;
	     i++) {
		const char*	err_msg;
		const char*	name = NULL;
		ulint		nth_v_col = ULINT_UNDEFINED;
		const rec_t*	rec = btr_pcur_get_rec(&pcur);

		err_msg = btr_pcur_is_on_user_rec(&pcur)
			? dict_load_column_low(table, use_uncommitted,
					       heap, NULL, NULL,
					       &name, rec, &mtr, &nth_v_col)
			: dict_load_column_none;

		if (!err_msg) {
		} else if (err_msg == dict_load_column_del) {
			n_skipped++;
			goto next_rec;
		} else if (err_msg == dict_load_column_instant) {
			err = DB_SUCCESS_LOCKED_REC;
			goto func_exit;
		} else if (err_msg == dict_load_column_none
			   && strstr(table->name.m_name,
				     "/" TEMP_FILE_PREFIX_INNODB)) {
			break;
		} else {
			ib::error() << err_msg << " for table " << table->name;
			err = DB_CORRUPTION;
			goto func_exit;
		}

		/* Note: Currently we have one DOC_ID column that is
		shared by all FTS indexes on a table. And only non-virtual
		column can be used for FULLTEXT index */
		if (innobase_strcasecmp(name,
					FTS_DOC_ID_COL_NAME) == 0
		    && nth_v_col == ULINT_UNDEFINED) {
			dict_col_t*	col;
			/* As part of normal loading of tables the
			table->flag is not set for tables with FTS
			till after the FTS indexes are loaded. So we
			create the fts_t instance here if there isn't
			one already created.

			This case does not arise for table create as
			the flag is set before the table is created. */
			if (table->fts == NULL) {
				table->fts = fts_create(table);
				table->fts->cache = fts_cache_create(table);
				DICT_TF2_FLAG_SET(table, DICT_TF2_FTS_AUX_HEX_NAME);
			}

			ut_a(table->fts->doc_col == ULINT_UNDEFINED);

			col = dict_table_get_nth_col(table, i - n_skipped);

			ut_ad(col->len == sizeof(doc_id_t));

			if (col->prtype & DATA_FTS_DOC_ID) {
				DICT_TF2_FLAG_SET(
					table, DICT_TF2_FTS_HAS_DOC_ID);
				DICT_TF2_FLAG_UNSET(
					table, DICT_TF2_FTS_ADD_DOC_ID);
			}

			table->fts->doc_col = i - n_skipped;
		}
next_rec:
		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

func_exit:
	mtr.commit();
	return err;
}

/** Loads SYS_VIRTUAL info for one virtual column
@param table	   table definition
@param uncommitted false=READ COMMITTED, true=READ UNCOMMITTED
@param nth_v_col   virtual column position */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
dberr_t
dict_load_virtual_col(dict_table_t *table, bool uncommitted, ulint nth_v_col)
{
	const dict_v_col_t* v_col = dict_table_get_nth_v_col(table, nth_v_col);

	if (v_col->num_base == 0) {
		return DB_SUCCESS;
	}

	dict_index_t*	sys_virtual_index;
	btr_pcur_t	pcur;
	mtr_t		mtr;

	ut_ad(dict_sys.locked());

	mtr.start();

	sys_virtual_index = dict_sys.sys_virtual->indexes.start;
	ut_ad(!dict_sys.sys_virtual->not_redundant());

	ut_ad(name_of_col_is(dict_sys.sys_virtual, sys_virtual_index,
			     DICT_FLD__SYS_VIRTUAL__POS, "POS"));

	dfield_t dfield[2];
	dtuple_t tuple{
		0,2,2,dfield,0,nullptr
#ifdef UNIV_DEBUG
		, DATA_TUPLE_MAGIC_N
#endif
	};
	byte table_id[8], vcol_pos[4];
	mach_write_to_8(table_id, table->id);
	dfield_set_data(&dfield[0], table_id, 8);
	mach_write_to_4(vcol_pos,
			dict_create_v_col_pos(nth_v_col, v_col->m_col.ind));
	dfield_set_data(&dfield[1], vcol_pos, 4);

	dict_index_copy_types(&tuple, sys_virtual_index, 2);
	pcur.btr_cur.page_cur.index = sys_virtual_index;

	dberr_t err = btr_pcur_open_on_user_rec(&tuple,
						BTR_SEARCH_LEAF, &pcur, &mtr);
	if (err != DB_SUCCESS) {
		goto func_exit;
	}

	for (ulint i = 0, skipped = 0;
	     i < unsigned{v_col->num_base} + skipped; i++) {
		ulint		pos;
		const char*	err_msg
			= btr_pcur_is_on_user_rec(&pcur)
			? dict_load_virtual_low(table, uncommitted,
						&v_col->base_col[i - skipped],
						NULL,
					        &pos, NULL,
						btr_pcur_get_rec(&pcur))
			: dict_load_virtual_none;

		if (!err_msg) {
			ut_ad(pos == mach_read_from_4(vcol_pos));
		} else if (err_msg == dict_load_virtual_del) {
			skipped++;
		} else if (err_msg == dict_load_virtual_none
			   && strstr(table->name.m_name,
				     "/" TEMP_FILE_PREFIX_INNODB)) {
			break;
		} else {
			ib::error() << err_msg << " for table " << table->name;
			err = DB_CORRUPTION;
			break;
		}

		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

func_exit:
	mtr.commit();
	return err;
}

/** Loads info from SYS_VIRTUAL for virtual columns.
@param table	   table definition
@param uncommitted false=READ COMMITTED, true=READ UNCOMMITTED */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static dberr_t dict_load_virtual(dict_table_t *table, bool uncommitted)
{
  for (ulint i= 0; i < table->n_v_cols; i++)
    if (dberr_t err= dict_load_virtual_col(table, uncommitted, i))
      return err;
  return DB_SUCCESS;
}

/** Error message for a delete-marked record in dict_load_field_low() */
static const char *dict_load_field_del= "delete-marked record in SYS_FIELDS";

static const char *dict_load_field_none= "SYS_FIELDS record not found";

/** Load an index field definition from a SYS_FIELDS record to dict_index_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_field_low(
	byte*		index_id,	/*!< in/out: index id (8 bytes)
					an "in" value if index != NULL
					and "out" if index == NULL */
	bool		uncommitted,	/*!< in: false=READ COMMITTED,
					true=READ UNCOMMITTED */
	dict_index_t*	index,		/*!< in/out: index, could be NULL
					if we just populate a dict_field_t
					struct with information from
					a SYS_FIELDS record */
	dict_field_t*	sys_field,	/*!< out: dict_field_t to be
					filled */
	ulint*		pos,		/*!< out: Field position */
	byte*		last_index_id,	/*!< in: last index id */
	mem_heap_t*	heap,		/*!< in/out: memory heap
					for temporary storage */
	mtr_t*		mtr,		/*!< in/out: mini-transaction */
	const rec_t*	rec)		/*!< in: SYS_FIELDS record */
{
	const byte*	field;
	ulint		len;
	unsigned	pos_and_prefix_len;
	unsigned	prefix_len;
	bool		descending;
	bool		first_field;
	ulint		position;

	/* Either index or sys_field is supplied, not both */
	ut_ad((!index) != (!sys_field));
	ut_ad((!index) == !mtr);

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_FIELDS) {
		return("wrong number of columns in SYS_FIELDS record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FIELDS__INDEX_ID, &len);
	if (len != 8) {
err_len:
		return("incorrect column length in SYS_FIELDS");
	}

	if (!index) {
		ut_a(last_index_id);
		memcpy(index_id, (const char*) field, 8);
		first_field = memcmp(index_id, last_index_id, 8);
	} else {
		first_field = (index->n_def == 0);
		if (memcmp(field, index_id, 8)) {
			return dict_load_field_none;
		}
	}

	/* The next field stores the field position in the index and a
	possible column prefix length if the index field does not
	contain the whole column. The storage format is like this: if
	there is at least one prefix field in the index, then the HIGH
	2 bytes contain the field number (index->n_def) and the low 2
	bytes the prefix length for the field. Otherwise the field
	number (index->n_def) is contained in the 2 LOW bytes. */

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FIELDS__POS, &len);
	if (len != 4) {
		goto err_len;
	}

	pos_and_prefix_len = mach_read_from_4(field);

	if (index && UNIV_UNLIKELY
	    ((pos_and_prefix_len & 0xFFFFUL) != index->n_def
	     && (pos_and_prefix_len >> 16 & 0xFFFF) != index->n_def)) {
		return("SYS_FIELDS.POS mismatch");
	}

	if (first_field || pos_and_prefix_len > 0xFFFFUL) {
		prefix_len = pos_and_prefix_len & 0x7FFFUL;
		descending = (pos_and_prefix_len & 0x8000UL);
		position = (pos_and_prefix_len & 0xFFFF0000UL)  >> 16;
	} else {
		prefix_len = 0;
		descending = false;
		position = pos_and_prefix_len & 0xFFFFUL;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FIELDS__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_FIELDS__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	const trx_id_t trx_id = trx_read_trx_id(field);

	if (!trx_id) {
		ut_ad(!rec_get_deleted_flag(rec, 0));
	} else if (!mtr || uncommitted) {
	} else if (trx_sys.find(nullptr, trx_id, false)) {
		const auto savepoint = mtr->get_savepoint();
		dict_index_t* sys_field = UT_LIST_GET_FIRST(
			dict_sys.sys_fields->indexes);
		rec_offs* offsets = rec_get_offsets(
			rec, sys_field, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, mtr, sys_field, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr->rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!old_vers || rec_get_deleted_flag(rec, 0)) {
			return dict_load_field_none;
		}
	}

	if (rec_get_deleted_flag(rec, 0)) {
		return(dict_load_field_del);
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FIELDS__COL_NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}

	if (index) {
		dict_mem_index_add_field(
			index, mem_heap_strdupl(heap, (const char*) field, len),
			prefix_len, descending);
	} else {
		sys_field->name = mem_heap_strdupl(
			heap, (const char*) field, len);
		sys_field->prefix_len = prefix_len & ((1U << 12) - 1);
		sys_field->descending = descending;
		*pos = position;
	}

	return(NULL);
}

/**
Load definitions for index fields.
@param index       index whose fields are to be loaded
@param uncommitted false=READ COMMITTED, true=READ UNCOMMITTED
@param heap        memory heap for temporary storage
@return error code
@return DB_SUCCESS if the fields were loaded successfully */
static dberr_t dict_load_fields(dict_index_t *index, bool uncommitted,
                                mem_heap_t *heap)
{
	btr_pcur_t	pcur;
	mtr_t		mtr;

	ut_ad(dict_sys.locked());

	mtr.start();

	dict_index_t* sys_index = dict_sys.sys_fields->indexes.start;
	ut_ad(!dict_sys.sys_fields->not_redundant());
	ut_ad(name_of_col_is(dict_sys.sys_fields, sys_index,
			     DICT_FLD__SYS_FIELDS__COL_NAME, "COL_NAME"));

	dfield_t dfield;
	dtuple_t tuple{
		0,1,1,&dfield,0,nullptr
#ifdef UNIV_DEBUG
		, DATA_TUPLE_MAGIC_N
#endif
	};
	byte index_id[8];
	mach_write_to_8(index_id, index->id);
	dfield_set_data(&dfield, index_id, 8);
	dict_index_copy_types(&tuple, sys_index, 1);
	pcur.btr_cur.page_cur.index = sys_index;

	dberr_t error = btr_pcur_open_on_user_rec(&tuple, BTR_SEARCH_LEAF,
						  &pcur, &mtr);
	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	for (ulint i = 0; i < index->n_fields; i++) {
		const char *err_msg = btr_pcur_is_on_user_rec(&pcur)
			? dict_load_field_low(index_id, uncommitted, index,
					      nullptr, nullptr, nullptr,
					      heap, &mtr,
					      btr_pcur_get_rec(&pcur))
			: dict_load_field_none;

		if (!err_msg) {
		} else if (err_msg == dict_load_field_del) {
			/* There could be delete marked records in
			SYS_FIELDS because SYS_FIELDS.INDEX_ID can be
			updated by ALTER TABLE ADD INDEX. */
		} else {
			if (err_msg != dict_load_field_none
			    || strstr(index->table->name.m_name,
				      "/" TEMP_FILE_PREFIX_INNODB)) {
				ib::error() << err_msg << " for index "
					    << index->name
					    << " of table "
					    << index->table->name;
			}
			error = DB_CORRUPTION;
			break;
		}

		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

func_exit:
	mtr.commit();
	return error;
}

/** Error message for a delete-marked record in dict_load_index_low() */
static const char *dict_load_index_del= "delete-marked record in SYS_INDEXES";
/** Error message for table->id mismatch in dict_load_index_low() */
static const char *dict_load_index_none= "SYS_INDEXES record not found";
/** Error message for SYS_TABLES flags mismatch in dict_load_table_low() */
static const char *dict_load_table_flags= "incorrect flags in SYS_TABLES";

/** Load an index definition from a SYS_INDEXES record to dict_index_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_index_low(
	byte*		table_id,	/*!< in/out: table id (8 bytes),
					an "in" value if mtr
					and "out" when !mtr */
	bool		uncommitted,	/*!< in: false=READ COMMITTED,
					true=READ UNCOMMITTED */
	mem_heap_t*	heap,		/*!< in/out: temporary memory heap */
	const rec_t*	rec,		/*!< in: SYS_INDEXES record */
	mtr_t*		mtr,		/*!< in/out: mini-transaction,
					or nullptr if a pre-allocated
					*index is to be filled in */
	dict_table_t*	table,		/*!< in/out: table, or NULL */
	dict_index_t**	index)		/*!< out,own: index, or NULL */
{
	const byte*	field;
	ulint		len;
	index_id_t	id;
	ulint		n_fields;
	ulint		type;
	unsigned	merge_threshold;

	if (mtr) {
		*index = NULL;
	}

	if (rec_get_n_fields_old(rec) == DICT_NUM_FIELDS__SYS_INDEXES) {
		/* MERGE_THRESHOLD exists */
		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__MERGE_THRESHOLD, &len);
		switch (len) {
		case 4:
			merge_threshold = mach_read_from_4(field);
			break;
		case UNIV_SQL_NULL:
			merge_threshold = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
			break;
		default:
			return("incorrect MERGE_THRESHOLD length"
			       " in SYS_INDEXES");
		}
	} else if (rec_get_n_fields_old(rec)
		   == DICT_NUM_FIELDS__SYS_INDEXES - 1) {
		/* MERGE_THRESHOLD doesn't exist */

		merge_threshold = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
	} else {
		return("wrong number of columns in SYS_INDEXES record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__TABLE_ID, &len);
	if (len != 8) {
err_len:
		return("incorrect column length in SYS_INDEXES");
	}

	if (!mtr) {
		/* We are reading a SYS_INDEXES record. Copy the table_id */
		memcpy(table_id, (const char*) field, 8);
	} else if (memcmp(field, table_id, 8)) {
		/* Caller supplied table_id, verify it is the same
		id as on the index record */
		return dict_load_index_none;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__ID, &len);
	if (len != 8) {
		goto err_len;
	}

	id = mach_read_from_8(field);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_INDEXES__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	const trx_id_t trx_id = trx_read_trx_id(field);
	if (!trx_id) {
		ut_ad(!rec_get_deleted_flag(rec, 0));
	} else if (!mtr || uncommitted) {
	} else if (trx_sys.find(nullptr, trx_id, false)) {
		const auto savepoint = mtr->get_savepoint();
		dict_index_t* sys_index = UT_LIST_GET_FIRST(
			dict_sys.sys_indexes->indexes);
		rec_offs* offsets = rec_get_offsets(
			rec, sys_index, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, mtr, sys_index, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr->rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!old_vers || rec_get_deleted_flag(rec, 0)) {
			return dict_load_index_none;
		}
	} else if (rec_get_deleted_flag(rec, 0)
		   && rec[8 + 8 + DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]
		   != static_cast<byte>(*TEMP_INDEX_PREFIX_STR)
		   && table->def_trx_id < trx_id) {
		table->def_trx_id = trx_id;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__N_FIELDS, &len);
	if (len != 4) {
		goto err_len;
	}
	n_fields = mach_read_from_4(field);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__TYPE, &len);
	if (len != 4) {
		goto err_len;
	}
	type = mach_read_from_4(field);
	if (type & (~0U << DICT_IT_BITS)) {
		return("unknown SYS_INDEXES.TYPE bits");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__PAGE_NO, &len);
	if (len != 4) {
		goto err_len;
	}

	ut_d(const auto name_offs =)
	rec_get_nth_field_offs_old(rec, DICT_FLD__SYS_INDEXES__NAME, &len);
	ut_ad(name_offs == 8 + 8 + DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}

	if (rec_get_deleted_flag(rec, 0)) {
		return dict_load_index_del;
	}

	char* name = mem_heap_strdupl(heap, reinterpret_cast<const char*>(rec)
				      + (8 + 8 + DATA_TRX_ID_LEN
					 + DATA_ROLL_PTR_LEN),
				      len);

	if (mtr) {
		*index = dict_mem_index_create(table, name, type, n_fields);
	} else {
		dict_mem_fill_index_struct(*index, nullptr, name,
					   type, n_fields);
	}

	(*index)->id = id;
	(*index)->page = mach_read_from_4(field);
	ut_ad((*index)->page);
	(*index)->merge_threshold = merge_threshold & ((1U << 6) - 1);

	return(NULL);
}

/** Load definitions for table indexes. Adds them to the data dictionary cache.
@param table       table definition
@param uncommitted false=READ COMMITTED, true=READ UNCOMMITTED
@param heap        memory heap for temporary storage
@param ignore_err  errors to be ignored when loading the index definition
@return error code
@retval DB_SUCCESS if all indexes were successfully loaded
@retval DB_CORRUPTION if corruption of dictionary table
@retval DB_UNSUPPORTED if table has unknown index type */
static MY_ATTRIBUTE((nonnull))
dberr_t dict_load_indexes(dict_table_t *table, bool uncommitted,
                          mem_heap_t *heap, dict_err_ignore_t ignore_err)
{
	dict_index_t*	sys_index;
	btr_pcur_t	pcur;
	byte		table_id[8];
	mtr_t		mtr;

	ut_ad(dict_sys.locked());

	mtr.start();

	sys_index = dict_sys.sys_indexes->indexes.start;
	ut_ad(!dict_sys.sys_indexes->not_redundant());
	ut_ad(name_of_col_is(dict_sys.sys_indexes, sys_index,
			     DICT_FLD__SYS_INDEXES__NAME, "NAME"));
	ut_ad(name_of_col_is(dict_sys.sys_indexes, sys_index,
			     DICT_FLD__SYS_INDEXES__PAGE_NO, "PAGE_NO"));

	dfield_t dfield;
	dtuple_t tuple{
		0,1,1,&dfield,0,nullptr
#ifdef UNIV_DEBUG
		, DATA_TUPLE_MAGIC_N
#endif
	};
	mach_write_to_8(table_id, table->id);
	dfield_set_data(&dfield, table_id, 8);
	dict_index_copy_types(&tuple, sys_index, 1);
	pcur.btr_cur.page_cur.index = sys_index;

	dberr_t error = btr_pcur_open_on_user_rec(&tuple, BTR_SEARCH_LEAF,
						  &pcur, &mtr);
	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	while (btr_pcur_is_on_user_rec(&pcur)) {
		dict_index_t*	index = NULL;
		const char*	err_msg;
		const rec_t*	rec = btr_pcur_get_rec(&pcur);
		if ((ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK)
		    && (rec_get_n_fields_old(rec)
			== DICT_NUM_FIELDS__SYS_INDEXES
			/* a record for older SYS_INDEXES table
			(missing merge_threshold column) is acceptable. */
			|| rec_get_n_fields_old(rec)
			   == DICT_NUM_FIELDS__SYS_INDEXES - 1)) {
			const byte*	field;
			ulint		len;
			field = rec_get_nth_field_old(
				rec, DICT_FLD__SYS_INDEXES__NAME, &len);

			if (len != UNIV_SQL_NULL
			    && static_cast<char>(*field)
			    == static_cast<char>(*TEMP_INDEX_PREFIX_STR)) {
				/* Skip indexes whose name starts with
				TEMP_INDEX_PREFIX_STR, because they will
				be dropped by row_merge_drop_temp_indexes()
				during crash recovery. */
				goto next_rec;
			}
		}

		err_msg = dict_load_index_low(table_id, uncommitted, heap, rec,
					      &mtr, table, &index);
		ut_ad(!index == !!err_msg);

		if (err_msg == dict_load_index_none) {
			/* We have ran out of index definitions for
			the table. */
			break;
		}

		if (err_msg == dict_load_index_del) {
			goto next_rec;
		} else if (err_msg) {
			ib::error() << err_msg;
			if (ignore_err & DICT_ERR_IGNORE_INDEX) {
				goto next_rec;
			}
			error = DB_CORRUPTION;
			goto func_exit;
		} else if (rec[8 + 8 + DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]
			   == static_cast<byte>(*TEMP_INDEX_PREFIX_STR)) {
			dict_mem_index_free(index);
			goto next_rec;
		} else {
			const trx_id_t id = trx_read_trx_id(rec + 8 + 8);
			if (id > table->def_trx_id) {
				table->def_trx_id = id;
			}
		}

		ut_ad(index);
		ut_ad(!dict_index_is_online_ddl(index));

		/* Check whether the index is corrupted */
		if (ignore_err != DICT_ERR_IGNORE_DROP
		    && index->is_corrupted() && index->is_clust()) {
			dict_mem_index_free(index);
			error = DB_TABLE_CORRUPT;
			goto func_exit;
		}

		if (index->type & DICT_FTS
		    && !dict_table_has_fts_index(table)) {
			/* This should have been created by now. */
			ut_a(table->fts != NULL);
			DICT_TF2_FLAG_SET(table, DICT_TF2_FTS);
		}

		/* We check for unsupported types first, so that the
		subsequent checks are relevant for the supported types. */
		if (index->type & ~(DICT_CLUSTERED | DICT_UNIQUE
				    | DICT_CORRUPT | DICT_FTS
				    | DICT_SPATIAL | DICT_VIRTUAL)) {

			ib::error() << "Unknown type " << index->type
				<< " of index " << index->name
				<< " of table " << table->name;

			error = DB_UNSUPPORTED;
			dict_mem_index_free(index);
			goto func_exit;
		} else if (index->page == FIL_NULL
			   && table->is_readable()
			   && (!(index->type & DICT_FTS))) {
			if (!uncommitted
			    && ignore_err != DICT_ERR_IGNORE_DROP) {
				ib::error_or_warn(!(ignore_err
						    & DICT_ERR_IGNORE_INDEX))
					<< "Index " << index->name
					<< " for table " << table->name
					<< " has been freed!";
			}

			if (!(ignore_err & DICT_ERR_IGNORE_INDEX)) {
corrupted:
				dict_mem_index_free(index);
				error = DB_CORRUPTION;
				goto func_exit;
			}
			/* If caller can tolerate this error,
			we will continue to load the index and
			let caller deal with this error. However
			mark the index and table corrupted. We
			only need to mark such in the index
			dictionary cache for such metadata corruption,
			since we would always be able to set it
			when loading the dictionary cache */
			if (index->is_clust()) {
				index->table->corrupted = true;
				index->table->file_unreadable = true;
			}
			index->type |= DICT_CORRUPT;
		} else if (!dict_index_is_clust(index)
			   && NULL == dict_table_get_first_index(table)) {

			ib::error() << "Trying to load index " << index->name
				<< " for table " << table->name
				<< ", but the first index is not clustered!";

			goto corrupted;
		} else if (dict_is_sys_table(table->id)
			   && (dict_index_is_clust(index)
			       || ((table == dict_sys.sys_tables)
				   && !strcmp("ID_IND", index->name)))) {

			/* The index was created in memory already at booting
			of the database server */
			dict_mem_index_free(index);
		} else {
			error = dict_load_fields(index, uncommitted, heap);
			if (error != DB_SUCCESS) {
				goto func_exit;
			}

			/* The data dictionary tables should never contain
			invalid index definitions.  If we ignored this error
			and simply did not load this index definition, the
			.frm file would disagree with the index definitions
			inside InnoDB. */
			if ((error = dict_index_add_to_cache(index,
							     index->page))
			    != DB_SUCCESS) {
				goto func_exit;
			}

#ifdef UNIV_DEBUG
			// The following assertion doesn't hold for FTS indexes
			// as it may have prefix_len=1 with any charset
			if (index->type != DICT_FTS) {
				for (uint i = 0; i < index->n_fields; i++) {
					dict_field_t &f = index->fields[i];
					ut_ad(f.col->mbmaxlen == 0
					      || f.prefix_len
					      % f.col->mbmaxlen == 0);
				}
			}
#endif /* UNIV_DEBUG */
		}
next_rec:
		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	if (!dict_table_get_first_index(table)
	    && !(ignore_err & DICT_ERR_IGNORE_INDEX)) {
		ib::warn() << "No indexes found for table " << table->name;
		error = DB_CORRUPTION;
		goto func_exit;
	}

	ut_ad(table->fts_doc_id_index == NULL);

	if (table->fts != NULL) {
		dict_index_t *idx = dict_table_get_index_on_name(
			table, FTS_DOC_ID_INDEX_NAME);
		if (idx && dict_index_is_unique(idx)) {
			table->fts_doc_id_index = idx;
		}
	}

	/* If the table contains FTS indexes, populate table->fts->indexes */
	if (dict_table_has_fts_index(table)) {
		ut_ad(table->fts_doc_id_index != NULL);
		/* table->fts->indexes should have been created. */
		ut_a(table->fts->indexes != NULL);
		dict_table_get_all_fts_indexes(table, table->fts->indexes);
	}

func_exit:
	mtr.commit();
	return error;
}

/** Load a table definition from a SYS_TABLES record to dict_table_t.
Do not load any columns or indexes.
@param[in,out]	mtr		mini-transaction
@param[in]	uncommitted	whether to use READ UNCOMMITTED isolation level
@param[in]	rec		SYS_TABLES record
@param[out,own]	table		table, or nullptr
@return	error message
@retval	nullptr on success */
const char *dict_load_table_low(mtr_t *mtr, bool uncommitted,
                                const rec_t *rec, dict_table_t **table)
{
	table_id_t	table_id;
	uint32_t	space_id, t_num, flags, flags2;
	ulint		n_cols, n_v_col;
	trx_id_t	trx_id;

	if (const char* error_text = dict_sys_tables_rec_check(rec)) {
		*table = NULL;
		return(error_text);
	}

	if (auto r = dict_sys_tables_rec_read(rec, uncommitted, mtr,
					      &table_id, &space_id,
					      &t_num, &flags, &flags2,
					      &trx_id)) {
		*table = NULL;
		return r == READ_ERROR ? dict_load_table_flags : nullptr;
	}

	dict_table_decode_n_col(t_num, &n_cols, &n_v_col);

	*table = dict_table_t::create(
		span<const char>(reinterpret_cast<const char*>(rec),
				 rec_get_field_start_offs(rec, 1)),
		nullptr, n_cols + n_v_col, n_v_col, flags, flags2);
	(*table)->space_id = space_id;
	(*table)->id = table_id;
	(*table)->file_unreadable = !!(flags2 & DICT_TF2_DISCARDED);
	(*table)->def_trx_id = trx_id;
	return(NULL);
}

/** Make sure the data_file_name is saved in dict_table_t if needed.
@param[in,out]	table		Table object */
void dict_get_and_save_data_dir_path(dict_table_t *table)
{
  ut_ad(!table->is_temporary());
  ut_ad(!table->space || table->space->id == table->space_id);

  if (!table->data_dir_path && table->space_id && table->space)
  {
    const char *filepath= table->space->chain.start->name;
    if (strncmp(fil_path_to_mysql_datadir, filepath,
                strlen(fil_path_to_mysql_datadir)))
    {
      table->lock_mutex_lock();
      table->flags|= 1 << DICT_TF_POS_DATA_DIR & ((1U << DICT_TF_BITS) - 1);
      table->data_dir_path= mem_heap_strdup(table->heap, filepath);
      os_file_make_data_dir_path(table->data_dir_path);
      table->lock_mutex_unlock();
    }
  }
}

/** Opens a tablespace for dict_load_table_one()
@param[in,out]	table		A table that refers to the tablespace to open
@param[in]	ignore_err	Whether to ignore an error. */
UNIV_INLINE
void
dict_load_tablespace(
	dict_table_t*		table,
	dict_err_ignore_t	ignore_err)
{
	ut_ad(!table->is_temporary());
	ut_ad(!table->space);
	ut_ad(table->space_id < SRV_SPACE_ID_UPPER_BOUND);
	ut_ad(fil_system.sys_space);

	if (table->space_id == TRX_SYS_SPACE) {
		table->space = fil_system.sys_space;
		return;
	}

	if (table->flags2 & DICT_TF2_DISCARDED) {
		ib::warn() << "Tablespace for table " << table->name
			<< " is set as discarded.";
		table->file_unreadable = true;
		return;
	}

	/* The tablespace may already be open. */
	table->space = fil_space_for_table_exists_in_mem(table->space_id,
							 table->flags);
	if (table->space || table->file_unreadable) {
		return;
	}

	/* Use the remote filepath if needed. This parameter is optional
	in the call to fil_ibd_open(). If not supplied, it will be built
	from the table->name. */
	char* filepath = NULL;
	if (DICT_TF_HAS_DATA_DIR(table->flags)) {
		/* This will set table->data_dir_path from fil_system */
		dict_get_and_save_data_dir_path(table);

		if (table->data_dir_path) {
			filepath = fil_make_filepath(
				table->data_dir_path, table->name, IBD, true);
		}
	}

	table->space = fil_ibd_open(
		2, FIL_TYPE_TABLESPACE, table->space_id,
		dict_tf_to_fsp_flags(table->flags),
		{table->name.m_name, strlen(table->name.m_name)}, filepath);

	if (!table->space) {
		/* We failed to find a sensible tablespace file */
		table->file_unreadable = true;

		if (!(ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK)) {
			sql_print_error("InnoDB: Failed to load tablespace "
					ULINTPF " for table %s",
					table->space_id, table->name);
		}
	}

	ut_free(filepath);
}

/** Loads a table definition and also all its index definitions.

Loads those foreign key constraints whose referenced table is already in
dictionary cache.

@param[in]	name		Table name in the db/tablename format
@param[in]	ignore_err	Error to be ignored when loading table
				and its index definition
@return table, possibly with file_unreadable flag set
@retval nullptr if the table does not exist */
static dict_table_t *dict_load_table_one(const span<const char> &name,
                                         dict_err_ignore_t ignore_err)
{
	btr_pcur_t	pcur;
	mtr_t		mtr;

	DBUG_ENTER("dict_load_table_one");
	DBUG_PRINT("dict_load_table_one",
		   ("table: %.*s", int(name.size()), name.data()));

	ut_ad(dict_sys.locked());

	dict_index_t *sys_index = dict_sys.sys_tables->indexes.start;
	ut_ad(!dict_sys.sys_tables->not_redundant());
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__ID, "ID"));
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__N_COLS, "N_COLS"));
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__TYPE, "TYPE"));
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__MIX_LEN, "MIX_LEN"));
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__SPACE, "SPACE"));

	dfield_t dfield;
	dtuple_t tuple{
		0,1,1,&dfield,0,nullptr
#ifdef UNIV_DEBUG
		, DATA_TUPLE_MAGIC_N
#endif
	};
	dfield_set_data(&dfield, name.data(), name.size());
	dict_index_copy_types(&tuple, sys_index, 1);
	pcur.btr_cur.page_cur.index = sys_index;

	bool uncommitted = false;
reload:
	mtr.start();
	dberr_t err = btr_pcur_open_on_user_rec(&tuple,
						BTR_SEARCH_LEAF, &pcur, &mtr);

	if (err != DB_SUCCESS || !btr_pcur_is_on_user_rec(&pcur)) {
		/* Not found */
err_exit:
		mtr.commit();
		DBUG_RETURN(nullptr);
	}

	const rec_t* rec = btr_pcur_get_rec(&pcur);

	/* Check if the table name in record is the searched one */
	if (rec_get_field_start_offs(rec, 1) != name.size()
            || memcmp(name.data(), rec, name.size())) {
		goto err_exit;
	}

	dict_table_t* table;
	if (const char* err_msg =
	    dict_load_table_low(&mtr, uncommitted, rec, &table)) {
		if (err_msg != dict_load_table_flags) {
			ib::error() << err_msg;
		}
		goto err_exit;
	}
	if (!table) {
		goto err_exit;
	}

        const unsigned use_uncommitted = uncommitted
		? 2
		: table->id == mach_read_from_8(
			rec + rec_get_field_start_offs(
				rec, DICT_FLD__SYS_TABLES__ID));

	mtr.commit();

	mem_heap_t* heap = mem_heap_create(32000);

	dict_load_tablespace(table, ignore_err);

	switch (dict_load_columns(table, use_uncommitted, heap)) {
	case DB_SUCCESS_LOCKED_REC:
		ut_ad(!uncommitted);
		uncommitted = true;
		dict_mem_table_free(table);
		mem_heap_free(heap);
		goto reload;
	case DB_SUCCESS:
		if (!dict_load_virtual(table, uncommitted)) {
			break;
		}
		/* fall through */
	default:
		dict_mem_table_free(table);
		mem_heap_free(heap);
		DBUG_RETURN(nullptr);
	}

	dict_table_add_system_columns(table, heap);

	table->can_be_evicted = true;
	table->add_to_cache();

	mem_heap_empty(heap);

	ut_ad(dict_tf2_is_valid(table->flags, table->flags2));

	/* If there is no tablespace for the table then we only need to
	load the index definitions. So that we can IMPORT the tablespace
	later. When recovering table locks for resurrected incomplete
	transactions, the tablespace should exist, because DDL operations
	were not allowed while the table is being locked by a transaction. */
	dict_err_ignore_t index_load_err =
		!(ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK)
		&& !table->is_readable()
		? DICT_ERR_IGNORE_ALL
		: ignore_err;

	err = dict_load_indexes(table, uncommitted, heap, index_load_err);

	if (err == DB_TABLE_CORRUPT) {
		/* Refuse to load the table if the table has a corrupted
		cluster index */
		ut_ad(index_load_err != DICT_ERR_IGNORE_DROP);
		ib::error() << "Refusing to load corrupted table "
			    << table->name;
evict:
		dict_sys.remove(table);
		mem_heap_free(heap);
		DBUG_RETURN(nullptr);
	}

	if (err != DB_SUCCESS || !table->is_readable()) {
	} else if (dict_index_t* pk = dict_table_get_first_index(table)) {
		ut_ad(pk->is_primary());
		if (pk->is_corrupted()
		    || pk->page >= table->space->get_size()) {
corrupted:
			table->corrupted = true;
			table->file_unreadable = true;
			err = DB_TABLE_CORRUPT;
		} else if (table->space->id
			   && ignore_err == DICT_ERR_IGNORE_DROP) {
			/* Do not bother to load data from .ibd files
			only to delete the .ibd files. */
			goto corrupted;
		} else {
			const page_id_t page_id{table->space->id, pk->page};
			mtr.start();
			buf_block_t* block = buf_page_get(
				page_id, table->space->zip_size(),
				RW_S_LATCH, &mtr);
			const bool corrupted = !block
				|| page_get_space_id(block->page.frame)
				!= page_id.space()
				|| page_get_page_no(block->page.frame)
				!= page_id.page_no()
				|| (mach_read_from_2(FIL_PAGE_TYPE
						    + block->page.frame)
				    != FIL_PAGE_INDEX
				    && mach_read_from_2(FIL_PAGE_TYPE
							+ block->page.frame)
				    != FIL_PAGE_TYPE_INSTANT);
			mtr.commit();
			if (corrupted) {
				goto corrupted;
			}

			err = btr_cur_instant_init(table);
		}
	} else {
		ut_ad(ignore_err & DICT_ERR_IGNORE_INDEX);
		if (ignore_err != DICT_ERR_IGNORE_DROP) {
			err = DB_CORRUPTION;
			goto evict;
		}
	}

	/* Initialize table foreign_child value.
	   FIXME: is it needed?
	*/
	table->fk_max_recusive_level = 0;

	mem_heap_free(heap);

	ut_ad(!table
	      || (ignore_err & ~DICT_ERR_IGNORE_FK_NOKEY)
	      || !table->is_readable()
	      || !table->corrupted);

	if (table && table->fts) {
		if (!(dict_table_has_fts_index(table)
		      || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
		      || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID))) {
			/* the table->fts could be created in dict_load_column
			when a user defined FTS_DOC_ID is present, but no
			FTS */
			table->fts->~fts_t();
			table->fts = nullptr;
		} else if (fts_optimize_wq) {
			fts_optimize_add_table(table);
		} else if (table->can_be_evicted) {
			/* fts_optimize_thread is not started yet.
			So make the table as non-evictable from cache. */
			dict_sys.prevent_eviction(table);
		}
	}

	ut_ad(err != DB_SUCCESS || dict_foreign_set_validate(*table));

	DBUG_RETURN(table);
}

dict_table_t *dict_sys_t::load_table(const span<const char> &name,
                                     dict_err_ignore_t ignore)
{
  if (dict_table_t *table= find_table(name))
    return table;
  // FIXME: test MDEV-25743
  dict_table_t *table= dict_load_table_one(name, ignore);

  return table;
}

/***********************************************************************//**
Loads a table object based on the table id.
@return table; NULL if table does not exist */
dict_table_t*
dict_load_table_on_id(
/*==================*/
	table_id_t		table_id,	/*!< in: table id */
	dict_err_ignore_t	ignore_err)	/*!< in: errors to ignore
						when loading the table */
{
	byte		id_buf[8];
	btr_pcur_t	pcur;
	const byte*	field;
	ulint		len;
	mtr_t		mtr;

	ut_ad(dict_sys.locked());

	/* NOTE that the operation of this function is protected by
	dict_sys.latch, and therefore no deadlocks can occur
	with other dictionary operations. */

	mtr.start();
	/*---------------------------------------------------*/
	/* Get the secondary index based on ID for table SYS_TABLES */
	dict_index_t *sys_table_ids =
		dict_sys.sys_tables->indexes.start->indexes.next;

	dfield_t dfield;
	dtuple_t tuple{
		0,1,1,&dfield,0,nullptr
#ifdef UNIV_DEBUG
		, DATA_TUPLE_MAGIC_N
#endif
	};

	/* Write the table id in byte format to id_buf */
	mach_write_to_8(id_buf, table_id);
	dfield_set_data(&dfield, id_buf, 8);
	dict_index_copy_types(&tuple, sys_table_ids, 1);
	pcur.btr_cur.page_cur.index = sys_table_ids;

	dict_table_t* table = nullptr;

	if (btr_pcur_open_on_user_rec(&tuple, BTR_SEARCH_LEAF, &pcur, &mtr)
	    == DB_SUCCESS
	    && btr_pcur_is_on_user_rec(&pcur)) {
		/*---------------------------------------------------*/
		/* Now we have the record in the secondary index
		containing the table ID and NAME */
		const rec_t* rec = btr_pcur_get_rec(&pcur);
check_rec:
		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_TABLE_IDS__ID, &len);
		ut_ad(len == 8);

		/* Check if the table id in record is the one searched for */
		if (table_id == mach_read_from_8(field)) {
			field = rec_get_nth_field_old(rec,
				DICT_FLD__SYS_TABLE_IDS__NAME, &len);
			table = dict_sys.load_table(
				{reinterpret_cast<const char*>(field),
				 len}, ignore_err);
			if (table && table->id != table_id) {
				ut_ad(rec_get_deleted_flag(rec, 0));
				table = nullptr;
			}
			if (!table) {
				while (btr_pcur_move_to_next(&pcur, &mtr)) {
					rec = btr_pcur_get_rec(&pcur);

					if (page_rec_is_user_rec(rec)) {
						goto check_rec;
					}
				}
			}
		}
	}

	mtr.commit();
	return table;
}

/********************************************************************//**
This function is called when the database is booted. Loads system table
index definitions except for the clustered index which is added to the
dictionary cache at booting before calling this function. */
void
dict_load_sys_table(
/*================*/
	dict_table_t*	table)	/*!< in: system table */
{
	mem_heap_t*	heap;

	ut_ad(dict_sys.locked());

	heap = mem_heap_create(1000);

	dict_load_indexes(table, false, heap, DICT_ERR_IGNORE_NONE);

	mem_heap_free(heap);
}
