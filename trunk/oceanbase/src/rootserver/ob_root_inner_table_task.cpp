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
  *   zhidong <xielun.szd@taobao.com>
  *     - some work details if you want
  */

#include "ob_root_inner_table_task.h"
#include "ob_root_async_task_queue.h"
#include "ob_root_sql_proxy.h"

using namespace oceanbase::common;
using namespace oceanbase::rootserver;

ObRootInnerTableTask::ObRootInnerTableTask():cluster_id_(-1), timer_(NULL), queue_(NULL), proxy_(NULL)
{
}

ObRootInnerTableTask::~ObRootInnerTableTask()
{
}

int ObRootInnerTableTask::init(const int cluster_id, ObRootSQLProxy & proxy, ObTimer & timer,
    ObRootAsyncTaskQueue & queue)
{
  int ret = OB_SUCCESS;
  if (cluster_id < 0)
  {
    TBSYS_LOG(WARN, "check init param failed:cluster_id[%d]", cluster_id);
    ret = OB_INVALID_ARGUMENT;
  }
  else
  {
    cluster_id_ = cluster_id;
    timer_ = &timer;
    queue_ = &queue;
    proxy_ = &proxy;
  }
  return ret;
}

int ObRootInnerTableTask::modify_all_server_table(const ObRootAsyncTaskQueue::ObSeqTask & task)
{
  int ret = OB_SUCCESS;
  // write server info to internal table
  char buf[OB_MAX_SQL_LENGTH] = "";
  int32_t server_port = task.server_.get_port();
  char ip_buf[OB_MAX_SERVER_ADDR_SIZE] = "";
  if (false == task.server_.ip_to_string(ip_buf, sizeof(ip_buf)))
  {
    ret = OB_INVALID_ARGUMENT;
    TBSYS_LOG(WARN, "convert server ip to string failed:ret[%d]", ret);
  }
  else
  {
    switch (task.type_)
    {
      case SERVER_ONLINE:
      {
        const char * sql_temp = "REPLACE INTO __all_server(cluster_id, svr_type,"
          " svr_ip, svr_port, inner_port, svr_role, svr_version)"
          " VALUES(%d,\'%s\',\'%s\',%u,%u,%d,\'%s\');";
        snprintf(buf, sizeof (buf), sql_temp, cluster_id_, print_role(task.role_),
            ip_buf, server_port, task.inner_port_, task.server_status_, task.server_version_);
        break;
      }
      case SERVER_OFFLINE:
      {
        const char * sql_temp = "DELETE FROM __all_server WHERE svr_type=\'%s\' AND"
          " svr_ip=\'%s\' AND svr_port=%d AND cluster_id=%d;";
        snprintf(buf, sizeof (buf), sql_temp, print_role(task.role_),
            ip_buf, server_port, cluster_id_);
        break;
      }
      case ROLE_CHANGE:
      {
        const char * sql_temp = "REPLACE INTO __all_server(cluster_id, svr_type,"
          " svr_ip, svr_port, inner_port, svr_role) VALUES(%d,\'%s\',\'%s\',%u,%u,%d);";
        snprintf(buf, sizeof (buf), sql_temp, cluster_id_, print_role(task.role_),
            ip_buf, server_port, task.inner_port_, task.server_status_);
        break;
      }
      default:
      {
        ret = OB_INVALID_ARGUMENT;
        TBSYS_LOG(WARN, "check input param failed:task_type[%d]", task.type_);
      }
    }
  }
  if (OB_SUCCESS == ret)
  {
    ObString sql;
    sql.assign_ptr(buf, static_cast<ObString::obstr_size_t>(strlen(buf)));
    ret = proxy_->query(true, RETRY_TIMES, TIMEOUT, sql);
    if (OB_SUCCESS == ret)
    {
      TBSYS_LOG(INFO, "process inner task succ:task_id[%lu], timestamp[%ld], sql[%s]",
          task.get_task_id(), task.get_task_timestamp(), buf);
    }
  }
  return ret;
}

int ObRootInnerTableTask::modify_all_cluster_table(const ObRootAsyncTaskQueue::ObSeqTask & task)
{
  int ret = OB_SUCCESS;
  // write cluster info to internal table
  char buf[OB_MAX_SQL_LENGTH] = "";

  if (task.type_ == LMS_ONLINE)
  {
    char ip_buf[OB_MAX_SERVER_ADDR_SIZE] = "";
    if (false == task.server_.ip_to_string(ip_buf, sizeof(ip_buf)))
    {
      ret = OB_INVALID_ARGUMENT;
      TBSYS_LOG(WARN, "convert server ip to string failed:ret[%d]", ret);
    }
    else
    {
      const char * sql_temp = "REPLACE INTO %s"
        "(cluster_id, cluster_vip, cluster_port)"
        "VALUES(%d, \'%s\',%u);";
      snprintf(buf, sizeof (buf), sql_temp, OB_ALL_CLUSTER, cluster_id_, ip_buf,
               task.server_.get_port());
    }
  }
  else if (task.type_ == OBI_ROLE_CHANGE)
  {
    if (1 == task.cluster_role_)
    {
      ret = clean_other_masters();
    }
    if (OB_SUCCESS == ret)
    {
      const char * sql_temp = "REPLACE INTO %s"
        "(cluster_id, cluster_role)"
        "VALUES(%d, %d);";
      snprintf(buf, sizeof (buf), sql_temp, OB_ALL_CLUSTER, cluster_id_, task.cluster_role_);
    }
  }
  else
  {
    ret = OB_INVALID_ARGUMENT;
    TBSYS_LOG(WARN, "check input param failed:task_type[%d]", task.type_);
  }
  if (OB_SUCCESS == ret)
  {
    ObString sql;
    sql.assign_ptr(buf, static_cast<ObString::obstr_size_t>(strlen(buf)));
    ret = proxy_->query(true, RETRY_TIMES, TIMEOUT, sql);
    if (OB_SUCCESS == ret)
    {
      TBSYS_LOG(INFO, "process inner task succ:task_id[%lu], timestamp[%ld], sql[%s]",
          task.get_task_id(), task.get_task_timestamp(), buf);
    }
  }
  return ret;
}

int ObRootInnerTableTask::clean_other_masters(void)
{
  int ret = OB_SUCCESS;
  ObSEArray<int64_t, OB_MAX_CLUSTER_COUNT> cluster_ids;
  ObServer ms;
  bool query_master = true;
  if (OB_SUCCESS != (ret = proxy_->ms_provider_.get_ms(ms, query_master)))
  {
    TBSYS_LOG(WARN, "get mergeserver address failed, ret %d", ret);
  }
  else if (OB_SUCCESS != (ret = proxy_->rpc_stub_.fetch_master_cluster_id_list(
          ms, cluster_ids, TIMEOUT)))
  {
    TBSYS_LOG(WARN, "fetch master cluster id list failed, ret %d, ms %s", ret, to_cstring(ms));
  }
  else
  {
    char sql[OB_MAX_SQL_LENGTH];
    for (int64_t i = 0; OB_SUCCESS == ret && i < cluster_ids.count(); i++)
    {
      if (cluster_ids.at(i) == cluster_id_)
      {
        continue;
      }
      int n = snprintf(sql, sizeof(sql),
          "REPLACE INTO %s (cluster_id, cluster_role) VALUES (%ld, %d);",
          OB_ALL_CLUSTER, cluster_ids.at(i), 2);
      if (n < 0 || n >= static_cast<int>(sizeof(sql)))
      {
        TBSYS_LOG(WARN, "sql buffer not enough, n %d, errno %d", n, n < 0 ? errno : 0);
        ret = OB_BUF_NOT_ENOUGH;
      }
      else if (OB_SUCCESS != (ret = proxy_->query(
              query_master, RETRY_TIMES, TIMEOUT, ObString::make_string(sql))))
      {
        TBSYS_LOG(WARN, "execute sql [%s] failed, ret %d", sql, ret);
      }
    }
  }
  return ret;
}

void ObRootInnerTableTask::runTimerTask(void)
{
  if (check_inner_stat() != true)
  {
    TBSYS_LOG(WARN, "check inner stat failed");
  }
  else
  {
    int ret = OB_SUCCESS;
    int64_t cur_time = tbsys::CTimeUtil::getTime();
    int64_t end_time = cur_time + MAX_TIMEOUT;
    while ((OB_SUCCESS == ret) && (end_time > cur_time))
    {
      ret = process_head_task();
      if ((ret != OB_SUCCESS) && (ret != OB_ENTRY_NOT_EXIST))
      {
        TBSYS_LOG(WARN, "process head inner task failed:ret[%d]", ret);
      }
      else
      {
        cur_time = tbsys::CTimeUtil::getTime();
      }
    }
  }
}

int ObRootInnerTableTask::process_head_task(void)
{
  // process the head task
  ObRootAsyncTaskQueue::ObSeqTask task;
  int ret = queue_->head(task);
  if (OB_SUCCESS == ret)
  {
    switch (task.type_)
    {
    case ROLE_CHANGE:
    case SERVER_ONLINE:
    case SERVER_OFFLINE:
      {
        ret = modify_all_server_table(task);
        break;
      }
    case OBI_ROLE_CHANGE:
    case LMS_ONLINE:
      {
        ret = modify_all_cluster_table(task);
        break;
      }
    default:
      {
        ret = OB_ERROR;
        TBSYS_LOG(WARN, "not supported task right now");
        break;
      }
    }
  }
  // pop the succ task
  if (OB_SUCCESS == ret)
  {
    ObRootAsyncTaskQueue::ObSeqTask pop_task;
    ret = queue_->pop(pop_task);
    if ((ret != OB_SUCCESS) || (pop_task != task))
    {
      ret = OB_ERROR;
      TBSYS_LOG(ERROR, "pop the succ task failed:ret[%d]", ret);
      pop_task.print_info();
      task.print_info();
    }
  }
  return ret;
}
