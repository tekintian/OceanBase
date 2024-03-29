/**
 * (C) 2007-2010 Taobao Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Version: $Id$
 *
 * Authors:
 *   yanran <yanran.hfs@taobao.com>
 *     - some work details if you want
 */

#ifndef OCEANBASE_UPDATESERVER_OB_UPS_LOG_MGR_H_
#define OCEANBASE_UPDATESERVER_OB_UPS_LOG_MGR_H_

#include "common/ob_define.h"
#include "common/ob_log_writer.h"
#include "common/ob_mutex_task.h"
#include "common/ob_server_getter.h"
#include "ob_ups_role_mgr.h"
#include "ob_ups_table_mgr.h"
#include "ob_ups_slave_mgr.h"

#include "ob_ups_log_utils.h"
#include "ob_log_buffer.h"
#include "ob_pos_log_reader.h"
#include "ob_cached_pos_log_reader.h"
#include "ob_replay_log_src.h"
#include "ob_log_sync_delay_stat.h"
#include "ob_clog_stat.h"
#include "ob_log_replay_worker.h"
#include "ob_ups_stat.h"

namespace oceanbase
{
  namespace tests
  {
    namespace updateserver
    {
      // forward decleration
      class TestObUpsLogMgr_test_init_Test;
    }
  }
  namespace updateserver
  {
    class ObUpsLogMgr : public common::ObLogWriter
    {
      public:
      enum WAIT_SYNC_TYPE
      {
        WAIT_NONE = 0,
        WAIT_COMMIT = 1,
        WAIT_FLUSH = 2,
      };
      friend class tests::updateserver::TestObUpsLogMgr_test_init_Test;
        class FileIdBeforeLastMajorFrozen: public MinAvailFileIdGetter
        {
          public:
            FileIdBeforeLastMajorFrozen(){}
            ~FileIdBeforeLastMajorFrozen(){}
            virtual int64_t get();
        };

      struct ReplayLocalLogTask
      {
        ReplayLocalLogTask(): lock_(0), log_mgr_(NULL) {}
        virtual ~ReplayLocalLogTask() {}
        int init(ObUpsLogMgr* log_mgr)
        {
          int err = OB_SUCCESS;
          log_mgr_ = log_mgr;
          return err;
        }
        bool is_started() const
        {
          return lock_ >= 1;
        }
        bool is_done() const
        {
          return lock_ >= 2;
        }
        virtual int do_work()
        {
          OnceGuard guard(lock_);
          int err = OB_SUCCESS;
          if (!guard.try_lock())
          {}
          else if (NULL == log_mgr_)
          {
            err = OB_NOT_INIT;
          }
          else if (OB_SUCCESS != (err = log_mgr_->replay_local_log()))
          {
            if (OB_CANCELED == err)
            {
              TBSYS_LOG(WARN, "cancel replay local log");
            }
            else
            {
              TBSYS_LOG(ERROR, "replay_local_log()=>%d, will kill self", err);
              kill(getpid(), SIGTERM);
            }
          }
          else
          {
            TBSYS_LOG(INFO, "replay local log succ.");
          }
          return err;
        }
        volatile uint64_t lock_;
        ObUpsLogMgr* log_mgr_;
      };
      public:
      ObUpsLogMgr();
      virtual ~ObUpsLogMgr();
      int init(const char* log_dir, const int64_t log_file_max_size,
               ObLogReplayWorker* replay_worker_, ObReplayLogSrc* replay_log_src, ObUpsTableMgr* table_mgr,
               ObUpsSlaveMgr *slave_mgr, ObiRole* obi_role, ObUpsRoleMgr *role_mgr, int64_t log_sync_type);

      /// @brief set new replay point
      /// this method will write replay point to replay_point_file
      int write_replay_point(uint64_t replay_point);

      public:
        // 继承log_writer留下的回调接口

        // 主备UPS每次写盘之后调用
        virtual int write_log_hook(const bool is_master,
                                   const ObLogCursor start_cursor, const ObLogCursor end_cursor,
                                   const char* log_data, const int64_t data_len);
      public:
      // 统计回放日志的时间和生成mutator的时间延迟
      ObLogSyncDelayStat& get_delay_stat()
      {
        return delay_stat_;
      }
        int add_log_replay_event(const int64_t seq, const int64_t mutation_time, ReplayType replay_type);
        // 取得recent_log_cache的引用
      ObLogBuffer& get_log_buffer();
      public: // 主要接口
        // UPS刚启动，重放本地日志任务的函数
      int replay_local_log();
      int start_log(const ObLogCursor& start_cursor);
        // 重放完本地日志之后，主UPS调用start_log_for_master_write()，
        //主要是初始化log_writer写日志的起始点
      int start_log_for_master_write();
        // 主UPS给备UPS推送日志后，备UPS调用slave_receive_log()收日志
        // 收到日志后放到recent_log_cache
        int slave_receive_log(const char* buf, int64_t len, const int64_t wait_sync_time_us,
                              const WAIT_SYNC_TYPE wait_event_type);
        // 备UPS向主UPS请求日志时，主UPS调用get_log_for_slave_fetch()读缓冲区或文件得到日志
      int get_log_for_slave_fetch(ObFetchLogReq& req, ObFetchedLog& result);
        // 备UPS向主UPS注册成功后，主UPS返回自己的最大日志号，备UPS调用set_master_log_id()记录下这个值
      int set_master_log_id(const int64_t master_log_id);
        // RS选主时，需要问UPS已收到的连续的最大日志号
      int get_max_log_seq_replayable(int64_t& max_log_seq) const;
        // 如果备UPS本地没有日志，需要问主UPS自己应该从那个点开始写，
        // 主UPS调用fill_log_cursor()填充日志点
      int fill_log_cursor(ObLogCursor& log_cursor);
        // 备UPS的replay线程，调用replay_log()方法不停追赶主机的日志
        // replay_log()接着本地日志开始，不仅负责回放，也负责写盘
      int replay_log();
      int get_replayed_cursor(ObLogCursor& cursor) const;
      ObLogCursor& get_replayed_cursor_(ObLogCursor& cursor) const;
      bool is_sync_with_master() const;
      bool has_nothing_in_buf_to_replay() const;
      bool has_log_to_replay() const;
      int64_t wait_new_log_to_replay(const int64_t timeout_us);
      int write_log_as_slave(const char* buf, const int64_t len);
      int64_t to_string(char* buf, const int64_t len) const;
        
      protected:
        // 下面几个方法都是replay_log()需要的辅助方法
      int replay_and_write_log(const int64_t start_id, const int64_t end_id, const char* buf, int64_t len);
      int retrieve_log_for_replay(const int64_t start_id, int64_t& end_id, char* buf, const int64_t len,
          int64_t& read_count);
        // 确保得到一个确定的日志点才能开始回放后续日志
        // 可能需要用rpc询问主UPS
      int start_log_for_replay();

      protected:
      bool is_master_master() const;
      bool is_slave_master() const;
      int set_state_as_active();
      int get_max_log_seq_in_file(int64_t& log_seq) const;
      int get_max_log_seq_in_buffer(int64_t& log_seq) const;
      public:
      int do_replay_local_log_task()
      {
        return replay_local_log_task_.do_work();
      }
      bool is_log_replay_started() const
      {
        return replay_local_log_task_.is_started();
      }
      bool is_log_replay_finished() const
      {
        return replay_local_log_task_.is_done();
      }
      uint64_t get_master_log_seq() const
      {
        return master_log_id_;
      }
      void signal_stop()
      {
        if (replay_worker_)
        {
          replay_worker_->stop();
        }
        stop_ = true;
      }

      void wait()
      {
        if (replay_worker_)
        {
          replay_worker_->wait();
        }
      }
      inline int64_t get_last_receive_log_time() {return last_receive_log_time_;}

        inline ObClogStat& get_clog_stat() {return clog_stat_; }
      int add_slave(const common::ObServer& server, uint64_t &new_log_file_id, const bool switch_log);

      private:
      bool is_inited() const;
      private:
      int load_replay_point_();

      inline int check_inner_stat() const
      {
        int ret = common::OB_SUCCESS;
        if (!is_initialized_)
        {
          TBSYS_LOG(ERROR, "ObUpsLogMgr has not been initialized");
          ret = common::OB_NOT_INIT;
        }
        return ret;
      }

      private:
        FileIdBeforeLastMajorFrozen last_fid_before_frozen_;
        ObUpsTableMgr *table_mgr_;
        ObiRole *obi_role_;
        ObUpsRoleMgr *role_mgr_;
        ObLogBuffer recent_log_cache_; // 主机写日志或备机收日志时保存日志用的缓冲区
        ObPosLogReader pos_log_reader_; //从磁盘上根据日志ID读日志， 用来初始化cached_pos_log_reader_
        ObCachedPosLogReader cached_pos_log_reader_; // 根据日志ID读日志，先查看缓冲区，在查看日志文件
        ObReplayLogSrc* replay_log_src_; // 备机replay_log()时从replay_log_src_中取日志

        IObServerGetter* master_getter_; // 用来指示远程日志源的地址和类型(是lsync还是ups)
        ReplayLocalLogTask replay_local_log_task_;
        //common::ObLogCursor replayed_cursor_; // 已经回放到的点，不管发生什么情况都有保持连续递增
        common::ObLogCursor start_cursor_; // start_log()用到的参数，start_log()之后就没用了。
        int64_t local_max_log_id_when_start_;
        ObLogSyncDelayStat delay_stat_;
        ObClogStat clog_stat_;
        volatile bool stop_; // 主要用来通知回放本地日志的任务结束
        ThreadSpecificBuffer log_buffer_for_fetch_;
        ThreadSpecificBuffer log_buffer_for_replay_;
        ObLogReplayWorker* replay_worker_;
        volatile int64_t master_log_id_; // 备机知道的主机最大日志号
        tbsys::CThreadCond master_log_id_cond_;
        int64_t last_receive_log_time_;
        ObLogReplayPoint replay_point_;
      uint64_t max_log_id_;
      bool is_initialized_;
      char log_dir_[common::OB_MAX_FILE_NAME_LENGTH];
      bool is_started_;
    };
  } // end namespace updateserver
} // end namespace oceanbase

#endif // OCEANBASE_UPDATESERVER_OB_UPS_LOG_MGR_H_
