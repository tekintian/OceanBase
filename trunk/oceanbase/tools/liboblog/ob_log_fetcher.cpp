////===================================================================
 //
 // ob_log_fetcher.cpp liboblog / Oceanbase
 //
 // Copyright (C) 2013 Alipay.com, Inc.
 //
 // Created on 2013-05-23 by Yubai (yubai.lk@alipay.com) 
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

#include "tbsys.h"
#include "ob_log_fetcher.h"
#include "ob_log_utils.h"
#include "ob_log_common.h"

namespace oceanbase
{
  using namespace common;
  namespace liboblog
  {
    ObLogFetcher::ObLogFetcher() : mod_(ObModIds::OB_MUTATOR),
                                   allocator_(ALLOCATOR_PAGE_SIZE, mod_),
                                   inited_(false),
                                   server_selector_(NULL),
                                   rpc_stub_(NULL),
                                   start_id_(1),
                                   p_list_(),
                                   v_list_(),
                                   p_cond_(COND_SPIN_WAIT_NUM),
                                   v_cond_(COND_SPIN_WAIT_NUM),
                                   trans_count_(0)
    {
    }

    ObLogFetcher::~ObLogFetcher()
    {
      destroy();
    }

    int ObLogFetcher::init(IObLogServerSelector *server_selector, IObLogRpcStub *rpc_stub, const int64_t start_id)
    {
      int ret = OB_SUCCESS;
      int64_t max_checkpoint = 0;
      ObServer ups_addr;

      if (inited_)
      {
        ret = OB_INIT_TWICE;
      }
      else if (NULL == (server_selector_ = server_selector)
              || NULL == (rpc_stub_ = rpc_stub))
      {
        TBSYS_LOG(WARN, "invalid param, server_selector=%p rpc_stub=%p start_id=%ld",
                  server_selector, rpc_stub, start_id);
        ret = OB_INVALID_ARGUMENT;
      }
      else if (OB_SUCCESS != (ret = server_selector_->get_ups(ups_addr, false)))
      {
        TBSYS_LOG(ERROR, "get ups addr failed, ret=%d", ret);
      }
      else if (OB_SUCCESS != (ret = rpc_stub_->get_max_log_seq(ups_addr, GET_MAX_LOG_SEQ_TIMEOUT, max_checkpoint)))
      {
        TBSYS_LOG(ERROR, "failed to get max checkpoint from UPS '%s', ret=%d",
            to_cstring(ups_addr), ret);
      }
      else if (0 > max_checkpoint)
      {
        TBSYS_LOG(ERROR, "unexcepted error: max checkpoint = %ld", max_checkpoint);
        ret = OB_ERR_UNEXPECTED;
      }
      else if (max_checkpoint < start_id || 0 > start_id)
      {
        LOG_AND_ERR(ERROR, "invalid checkpoint: %ld which should be a nonnetative number "
            "and should be less than or equal to current max checkpoint: %ld. "
            "Please check your configuration again.",
            start_id, max_checkpoint);

        ret = OB_INVALID_ARGUMENT;
      }
      else
      {
        start_id_ = start_id == 0 ? max_checkpoint : start_id;

        TBSYS_LOG(INFO, "start fetch log from checkpoint %ld", start_id_);

        if (OB_SUCCESS != (ret = init_mutator_list_(MAX_MUTATOR_NUM)))
        {
          TBSYS_LOG(WARN, "prepare mutator list fail, ret=%d", ret);
        }
        else if (1 != start())
        {
          TBSYS_LOG(WARN, "start thread to fetch log fail");
          ret = OB_ERR_UNEXPECTED;
        }
        else
        {
          inited_ = true;
        }
      }

      if (OB_SUCCESS != ret)
      {
        destroy();
      }
      return ret;
    }

    void ObLogFetcher::destroy()
    {
      inited_ = false;
      stop();
      wait();
      destroy_mutator_list_();
      start_id_ = 1;
      rpc_stub_ = NULL;
      server_selector_ = NULL;
      allocator_.reuse();
      trans_count_ = 0;
    }

    int ObLogFetcher::next_mutator(ObLogMutator **mutator, const int64_t timeout)
    {
      int ret = OB_SUCCESS;
      if (NULL == mutator)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else
      {
        ObLogMutator *log_mutator = NULL;
        int64_t start_time = tbsys::CTimeUtil::getTime();
        while (true)
        {
          ret = p_list_.pop(log_mutator);
          if (OB_SUCCESS == ret)
          {
            break;
          }

          int64_t wait_time = timeout - (tbsys::CTimeUtil::getTime() - start_time);
          if (0 >= wait_time)
          {
            break;
          }
          p_cond_.timedwait(timeout);
        }
        if (OB_SUCCESS != ret)
        {
          ret = OB_PROCESS_TIMEOUT;
        }
        else
        {
          if (NULL == log_mutator)
          {
            ret = OB_ERR_UNEXPECTED;
          }
          else
          {
            *mutator = log_mutator;
          }
        }
      }
      return ret;
    }

    void ObLogFetcher::release_mutator(ObLogMutator *mutator)
    {
      if (NULL != mutator)
      {
        mutator->reset();
        int ret = v_list_.push(mutator);
        if (OB_SUCCESS != ret)
        {
          TBSYS_LOG(WARN, "push mutator to v_list fail, ret=%d", ret);
        }
        else
        {
          v_cond_.signal();
        }
      }
    }

    int64_t ObLogFetcher::get_cur_id() const
    {
      return start_id_;
    }

    void ObLogFetcher::run(tbsys::CThread* thread, void* arg)
    {
      UNUSED(thread);
      UNUSED(arg);
      char *buffer = new char[FETCH_BUFFER_SIZE];
      if (NULL == buffer)
      {
        TBSYS_LOG(WARN, "allocate fetch buffer fail");
      }
      else
      {
        memset(buffer, 0, FETCH_BUFFER_SIZE);

        updateserver::ObFetchLogReq req;
        req.start_id_ = start_id_;
        req.max_data_len_ = FETCH_BUFFER_SIZE;

        bool change_ups_addr = false;
        int64_t change_ups_successively_num_on_fail = 0;
        int64_t last_server_refresh_time = tbsys::CTimeUtil::getTime();
        bool never_fetched_log = true;
        while (!_stop)
        {
          // Refresh server list periodically
          if (UPS_ADDR_REFRESH_INTERVAL <= (tbsys::CTimeUtil::getTime() - last_server_refresh_time))
          {
            server_selector_->refresh();
            last_server_refresh_time = tbsys::CTimeUtil::getTime();
          }

          // Print memory usage periodically
          if (REACH_TIME_INTERVAL(PRINT_MEMORY_USAGE_INTERVAL))
          {
            ob_print_mod_memory_usage();
          }

          ObServer ups_addr;
          int tmp_ret = OB_SUCCESS;
          // Get UPS
          if (OB_SUCCESS != (tmp_ret = server_selector_->get_ups(ups_addr, change_ups_addr)))
          {
            if (OB_NOT_INIT == tmp_ret)
            {
              TBSYS_LOG(ERROR, "get_ups fails: ObLogServerSelector is not initialized, ret=%d",
                  tmp_ret);
              abort();
            }
            else if (OB_ENTRY_NOT_EXIST == tmp_ret)
            {
              TBSYS_LOG(ERROR, "get_ups fails: UPS entry does not exist, ret=%d", tmp_ret);
              abort();
            }
            else
            {
              TBSYS_LOG(ERROR, "get_ups fails: error unexpected, ret=%d", tmp_ret);
              abort();
            }
          }

          // Fetch log
          bool ups_is_surely_alive = false;
          bool log_fetched = fetch_log_(ups_addr, req, change_ups_addr, ups_is_surely_alive, buffer);
          if (log_fetched)
          {
            change_ups_successively_num_on_fail = 0;
            never_fetched_log = false;
          }
          else if (change_ups_addr)
          {
            req.session_.reset();

            change_ups_successively_num_on_fail++;

            // NOTE: If UPS have changed "more than enough" times successively,
            // we consider ERRORs have occured and liboblog cannot restore automatically,
            // so abort now.
            if (change_ups_successively_num_on_fail >= server_selector_->get_ups_num())
            {
              // Cluster status is normal and checkpoint is invalid
              if (never_fetched_log && ups_is_surely_alive)
              {
                LOG_AND_ERR(ERROR, "fetch log fails: checkpoint %ld is invalid: "
                    "less than or greater than the checkpoints OceanBase has replayed.",
                    req.start_id_);
              }
              // Cluster status is abnormal or checkpoint is invalid
              else
              {
                LOG_AND_ERR(ERROR, "fetch log fails: OceanBase cluster status may be abnormal "
                    "or data on checkpoint %ld may be not served now. Please re-check your configuration",
                    req.start_id_);
              }

              abort();
            }

            // Force refresh server list
            server_selector_->refresh();
            last_server_refresh_time = tbsys::CTimeUtil::getTime();
          }

          // Print checkpoint periodically
          if (REACH_TIME_INTERVAL(PRINT_CHECKPOINT_INTERVAL))
          {
            TBSYS_LOG(INFO, "current checkpoint is %ld", req.start_id_);
          }
        }
        delete[] buffer;
      }
    }

    bool ObLogFetcher::fetch_log_(ObServer &ups_addr,
        updateserver::ObFetchLogReq &req,
        bool &change_ups_addr,
        bool &ups_is_surely_alive,
        char *buffer)
    {
      OB_ASSERT(NULL != buffer);

      bool log_fetched = false;
      static int64_t data_not_serve_retry_num = 0;

      updateserver::ObFetchedLog res;
      res.max_data_len_ = FETCH_BUFFER_SIZE;
      res.log_data_ = buffer;

      ObResultCode rc;
      int tmp_ret = OB_SUCCESS;
      change_ups_addr = false;
      ups_is_surely_alive = true;

      TBSYS_LOG(DEBUG, "fetch start cur_id=%ld", req.start_id_);
      if (OB_SUCCESS != (tmp_ret = rpc_stub_->fetch_log(ups_addr, req, res, FETCH_LOG_TIMEOUT, rc)))
      {
        if (OB_NOT_INIT == tmp_ret)
        {
          TBSYS_LOG(ERROR, "fetch_log from %s fails: ObLogRpcStub has not been initialized, ret=%d",
              to_cstring(ups_addr), tmp_ret);
          abort();
        }

        TBSYS_LOG(WARN, "fetch log from %s fail, ret=%d, change UPS next time",
            to_cstring(ups_addr), tmp_ret);

        // UPS is not surely alive
        ups_is_surely_alive = false;
        change_ups_addr = true;
      }
      // FIXME: Currently OB_NEED_RETRY means OB_DATA_NOT_SERVE
      else if (OB_NEED_RETRY == rc.result_code_)
      {
        if (DATA_NOT_SERVE_RETRY_NUM_MAX <= data_not_serve_retry_num)
        {
          TBSYS_LOG(WARN, "checkpoint = %ld DATA NOT SERVE on UPS (%s). "
              "Retry fetching log %ld times already. Change server to fetch log next time.",
              req.start_id_, to_cstring(ups_addr), data_not_serve_retry_num);

          change_ups_addr = true;
          data_not_serve_retry_num = 0;
        }
        else
        {
          data_not_serve_retry_num++;
          usleep(DATA_NOT_SERVE_RETRY_TIMEWAIT);
        }

        TBSYS_LOG(DEBUG, "fetch log from %s fails: checkpoint %ld data does not serve. "
            "retry_num = %ld, change_ups_addr = %s, result code = %d.",
            to_cstring(ups_addr), req.start_id_, data_not_serve_retry_num,
            change_ups_addr ? "true" : "false", rc.result_code_);
      }
      else if (OB_SUCCESS != rc.result_code_)
      {
        change_ups_addr = true;

        // UPS is not surely alive
        ups_is_surely_alive = false;

        TBSYS_LOG(WARN, "fetch log from %s fails: checkpoint = %ld, result code = %d, change UPS anyway next time",
            to_cstring(ups_addr), req.start_id_, rc.result_code_);
      }
      else if (OB_SUCCESS == rc.result_code_ && 0 == res.data_len_)
      {
        TBSYS_LOG(WARN, "Fetch data length is zero, return %d, result code = %d sleep %ld us and retry",
            tmp_ret, rc.result_code_, FETCH_LOG_RETRY_TIMEWAIT);

        usleep(FETCH_LOG_RETRY_TIMEWAIT);
      }
      else
      {
        // ERROR situations must be handled in other branches
        OB_ASSERT(OB_SUCCESS == rc.result_code_);

        TBSYS_LOG(DEBUG, "fetch done %s from %s", to_cstring(res), to_cstring(ups_addr));

        // Restore state variable
        data_not_serve_retry_num = 0;
        log_fetched = true;

        change_ups_addr = false;
        req.session_ = res.next_req_.session_;
        req.max_data_len_ = FETCH_BUFFER_SIZE;

        TIMED_FUNC(handle_fetched_log_, 10L * 1000000L, res, (uint64_t&)req.start_id_);
        start_id_ = req.start_id_;

        req.start_id_ += 1;
      }

      return log_fetched;
    }

    //////////////////////////////////////////////////

    int ObLogFetcher::init_mutator_list_(const int64_t max_mutator_num)
    {
      int ret = OB_SUCCESS;
      ret = p_list_.init(max_mutator_num);
      if (OB_SUCCESS != ret)
      {
        TBSYS_LOG(WARN, "init mutator p_list fail, max_mutator_num=%ld", max_mutator_num);
      }
      if (OB_SUCCESS == ret)
      {
        ret = v_list_.init(max_mutator_num);
        if (OB_SUCCESS != ret)
        {
          TBSYS_LOG(WARN, "init mutator v_list fail, max_mutator_num=%ld", max_mutator_num);
        }
      }
      for (int64_t i = 0; OB_SUCCESS == ret && i < max_mutator_num; i++)
      {
        void *buffer = allocator_.alloc(sizeof(ObLogMutator));
        if (NULL == buffer)
        {
          TBSYS_LOG(WARN, "allocator memory to build log_mutator fail, i=%ld", i);
          ret = OB_ALLOCATE_MEMORY_FAILED;
          break;
        }

        ObLogMutator *log_mutator = new(buffer) ObLogMutator();
        if (NULL == log_mutator)
        {
          TBSYS_LOG(WARN, "build log_mutator fail, i=%ld", i);
          ret = OB_ERR_UNEXPECTED;
          break;
        }

        ret = v_list_.push(log_mutator);
        if (OB_SUCCESS != ret)
        {
          TBSYS_LOG(WARN, "push mutator to list fail, i=%ld", i);
          break;
        }
      }
      return ret;
    }

    void ObLogFetcher::destroy_mutator_list_()
    {
      int64_t counter = v_list_.get_total() + v_list_.get_free();
      ObLogMutator *log_mutator = NULL;
      while (OB_SUCCESS == p_list_.pop(log_mutator))
      {
        if (NULL != log_mutator)
        {
          log_mutator->~ObLogMutator();
          log_mutator = NULL;
          counter--;
        }
      }
      p_list_.destroy();

      while (OB_SUCCESS == v_list_.pop(log_mutator))
      {
        if (NULL != log_mutator)
        {
          log_mutator->~ObLogMutator();
          log_mutator = NULL;
          counter--;
        }
      }
      v_list_.destroy();

      if (0 != counter)
      {
        TBSYS_LOG(WARN, "still have mutator not release, counter=%ld", counter);
      }
    }

    void ObLogFetcher::handle_fetched_log_(updateserver::ObFetchedLog &log, uint64_t &seq)
    {
      int64_t pos = 0;
      while (!_stop
            && pos < log.data_len_)
      {
        ObLogEntry log_entry;
        int tmp_ret = OB_SUCCESS;
        if (OB_SUCCESS != (tmp_ret = log_entry.deserialize(log.log_data_, log.data_len_, pos)))
        {
          TBSYS_LOG(ERROR, "deserialize log_entry fail, ret=%d", tmp_ret);
        }
        else if (OB_SUCCESS != (tmp_ret = log_entry.check_header_integrity(false)))
        {
          TBSYS_LOG(ERROR, "check_header_integrity fail, ret=%d", tmp_ret);
        }
        else if (OB_SUCCESS != (tmp_ret = log_entry.check_data_integrity(log.log_data_ + pos, false)))
        {
          TBSYS_LOG(ERROR, "check_data_integrity fail, ret=%d", tmp_ret);
        }
        // NOTE: we only construct mutator for UPS mutator
        else if (OB_LOG_UPS_MUTATOR == log_entry.cmd_)
        {
          if (OB_SUCCESS != (tmp_ret = construct_mutator_(log_entry, log, pos)))
          {
            TBSYS_LOG(ERROR, "construct_mutator_ fails. ret=%d", tmp_ret);
          }
        }

        // Update pos and seq on success
        if (OB_SUCCESS == tmp_ret)
        {
          pos += log_entry.get_log_data_len();
          seq = log_entry.seq_;
        }

        // Abort while falling into ERROR state
        if (OB_SUCCESS != tmp_ret)
        {
          if (OB_IN_STOP_STATE != tmp_ret)
          {
            abort();
          }

          break;
        }
      } // end while

      return;
    } // end handle_fetched_log_

    int ObLogFetcher::construct_mutator_(ObLogEntry &log_entry, updateserver::ObFetchedLog &log, const int64_t pos)
    {
      OB_ASSERT(OB_LOG_UPS_MUTATOR == log_entry.cmd_);

      int ret = OB_SUCCESS;
      ObLogMutator *log_mutator = NULL;

      // Get mutator from free list
      while (!_stop)
      {
        ret = v_list_.pop(log_mutator);
        if (OB_SUCCESS != ret)
        {
          v_cond_.timedwait(WAIT_VLIST_TIMEOUT);
        }
        else
        {
          break;
        }
      }

      if (_stop && NULL != log_mutator)
      {
        release_mutator(log_mutator);
        log_mutator = NULL;
      }

      if (NULL == log_mutator)
      {
        if (!_stop)
        {
          ret = OB_ERR_UNEXPECTED;
        }
        else
        {
          ret = OB_IN_STOP_STATE;
        }
      }
      else
      {
        // Reset content before use
        log_mutator->reset();

        // Deserialize MUTATOR
        int64_t tmp_pos = pos;
        if (OB_SUCCESS != (ret = log_mutator->deserialize(log.log_data_, log.data_len_, tmp_pos)))
        {
          TBSYS_LOG(WARN, "deserialize log_mutator fail, ret=%d", ret);
        }
        else
        {
          // Set cmd type and log ID
          log_mutator->set_cmd_type(static_cast<LogCommand>(log_entry.cmd_));
          log_mutator->set_log_id(log_entry.seq_);

          // Push valid mutator into p_list_
          if (OB_SUCCESS != (ret = p_list_.push(log_mutator)))
          {
            TBSYS_LOG(WARN, "push to p_list fail, ret=%d", ret);
          }
          else
          {
            p_cond_.signal();

            // Update statistics information
            if (log_mutator->is_dml_mutator())
            {
              trans_count_++;
            }

            log_mutator = NULL;
          }
        }
      }

      // Recycle mutator
      if (OB_SUCCESS != ret && NULL != log_mutator)
      {
        release_mutator(log_mutator);
        log_mutator = NULL;
      }

      return ret;
    } // end construct_mutator_

  }
}

