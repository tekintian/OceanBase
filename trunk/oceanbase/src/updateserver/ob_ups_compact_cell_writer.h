////===================================================================
 //
 // ob_ups_compact_cell_writer.h updateserver / Oceanbase
 //
 // Copyright (C) 2010 Taobao.com, Inc.
 //
 // Created on 2010-09-09 by Jianming (jianming.cjq@taobao.com) 
 //
 // -------------------------------------------------------------------
 //
 // Description
 //
 //
 // -------------------------------------------------------------------
 // 
 // Change Log
 //
////====================================================================

#ifndef OCEANBASE_UPDATESERVER_COMPACT_CELL_WRITER_H_
#define OCEANBASE_UPDATESERVER_COMPACT_CELL_WRITER_H_

#include "common/ob_compact_cell_writer.h"
#include "ob_memtank.h"

namespace oceanbase
{
  using namespace common;

  namespace updateserver
  {
    struct ObCTimeRecorder
    {
      int64_t ctime_;
      ObCTimeRecorder(): ctime_(INT64_MAX) {}
      ~ObCTimeRecorder() {}
      void reset() { ctime_ = INT64_MAX; }
      int64_t get_ctime() { return ctime_; }
      void add(bool is_row_deleted, const ObObj& value) {
        int64_t cur_time = INT64_MAX;
        if (is_row_deleted)
        {
          reset();
        }
        switch(value.get_type())
        {
          case ObCreateTimeType:
            value.get_createtime(cur_time);
            break;
          case ObModifyTimeType:
            value.get_modifytime(cur_time);
            break;
          default:
            break;
        }
        if (cur_time < ctime_)
        {
          ctime_ = cur_time;
        }
      }
    };
    class ObUpsCompactCellWriter : public ObCompactCellWriter
    {
      public:
        ObUpsCompactCellWriter();
        virtual ~ObUpsCompactCellWriter();

        void init(char *buf, int64_t size, MemTank *mem_tank = NULL);
        virtual int write_varchar(const ObObj &value, ObObj *clone_value);
      public:
        void set_row_deleted(const bool deleted) {is_row_deleted_ = deleted;};
        bool is_row_deleted() {return is_row_deleted_;};
        void reset() {ObCompactCellWriter::reset(); is_row_deleted_ = false; ctime_recorder_.reset(); };
        ObCTimeRecorder& get_ctime_recorder() { return ctime_recorder_; }

      private:
        MemTank *mem_tank_;
        bool is_row_deleted_;
        ObCTimeRecorder ctime_recorder_;
    };
  }
}

#endif /* OCEANBASE_UPDATESERVER_COMPACT_CELL_WRITER_H_ */

