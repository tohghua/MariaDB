/*
   Copyright (c) 2013, 2023, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


#ifndef TABLE_REPLICATION_APPLIER_STATUS_BY_COORDINATOR_H
#define TABLE_REPLICATION_APPLIER_STATUS_BY_COORDINATOR_H

/**
  @file storage/perfschema/table_replication_applier_applier_by_coordinator.h
  Table replication_applier_status_by_coordinator(declarations).
*/

#include "pfs_column_types.h"
#include "pfs_engine_table.h"
#include "rpl_mi.h"
#include "mysql_com.h"
//#include "rpl_msr.h"
//#include "rpl_info.h" /*CHANNEL_NAME_LENGTH*/
#include "my_thread.h"

class Master_info;

/**
  @addtogroup Performance_schema_tables
  @{
*/

#ifndef ENUM_RPL_YES_NO
#define ENUM_RPL_YES_NO
/** enum values for Service_State of coordinator thread */
enum enum_rpl_yes_no {
  PS_RPL_YES= 1, /* Service_State= on */
  PS_RPL_NO /* Service_State= off */
};
#endif

/*
  A row in coordinator's table. The fields with string values have an
  additional length field denoted by <field_name>_length.
*/
struct st_row_coordinator {
  char channel_name[CHANNEL_NAME_LENGTH];
  uint channel_name_length;
  ulonglong thread_id;
  bool thread_id_is_null;
  enum_rpl_yes_no service_state;
  uint last_error_number;
  char last_error_message[MAX_SLAVE_ERRMSG];
  uint last_error_message_length;
  ulonglong last_error_timestamp;
};

/** Table PERFORMANCE_SCHEMA.replication_applier_status_by_coordinator */
class table_replication_applier_status_by_coordinator: public PFS_engine_table
{
  typedef PFS_simple_index pos_t;

private:
  void make_row(Master_info *mi);

  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Fields definition. */
  static TABLE_FIELD_DEF m_field_def;
  /** Current row */
  st_row_coordinator m_row;
  /** True is the current row exists. */
  bool m_row_exists;
  /** Current position. */
  pos_t m_pos;
  /** Next position. */
  pos_t m_next_pos;

protected:
  /**
    Read the current row values.
    @param table            Table handle
    @param buf              row buffer
    @param fields           Table fields
    @param read_all         true if all columns are read.
  */

  virtual int read_row_values(TABLE *table,
                              unsigned char *buf,
                              Field **fields,
                              bool read_all);

  table_replication_applier_status_by_coordinator();

public:
  ~table_replication_applier_status_by_coordinator();

  static PFS_engine_table_share_state m_share_state;
  /** Table share. */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();
  static ha_rows get_row_count();
  virtual int rnd_next();
  virtual int rnd_pos(const void *pos);
  virtual void reset_position(void);

};

/** @} */
#endif
