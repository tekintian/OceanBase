#include "common/ob_define.h"   // UNUSED
#include "ob_sql_update_worker.h"
#include "tblog.h"
#include "ob_sql_util.h"
#include "ob_sql_global.h"
#include "ob_sql_ms_select.h"
#include "ob_sql_cluster_config.h"
#include "ob_sql_conn_acquire.h"    // print_connection_info
#include "ob_sql_util.h"
#include <mysql/mysql.h>
#include <string.h>
#include <tbsys.h>                  // CTimeUtil

// Mutex and condition variable used to wake up update thread
pthread_mutex_t g_update_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_update_cond = PTHREAD_COND_INITIALIZER;
bool g_update_flag = false;

pthread_t update_task;
bool update_work_started = false;

// Initialize g_rslist and g_rsnum
// FIXME: g_rslist and g_rsnum means LMS list and number. Refine THEM.
static void init_rs_list()
{
  memset(g_rslist, 0, sizeof(g_rslist));
  g_rslist[0].ip_ = trans_ip_to_int(g_sqlconfig.ip_);
  g_rslist[0].port_ = g_sqlconfig.port_;
  g_rsnum = 1;
}

void destroy_rs_list()
{
  memset(g_rslist, 0, sizeof(g_rslist));
  g_rsnum = 0;
}

void wakeup_update_worker()
{
  TBSYS_LOG(DEBUG, "wakeup update worker begin, g_update_flag=%s", g_update_flag ? "true" : "false");
  pthread_mutex_lock(&g_update_mutex);
  g_update_flag = true;
  pthread_cond_signal(&g_update_cond);
  pthread_mutex_unlock(&g_update_mutex);
  TBSYS_LOG(INFO, "wakeup update worker, g_update_flag=%s", g_update_flag ? "true" : "false");
}

void clear_update_flag()
{
  TBSYS_LOG(DEBUG, "clear update flag begin, g_update_flag=%s", g_update_flag ? "true" : "false");
  pthread_mutex_lock(&g_update_mutex);
  g_update_flag = false;
  pthread_mutex_unlock(&g_update_mutex);
  TBSYS_LOG(INFO, "clear update flag, g_update_flag=%s", g_update_flag ? "true" : "false");
}

static bool reach_time_(int64_t last_time_us, int64_t interval_sec)
{
  int64_t cur_interval_us = tbsys::CTimeUtil::getTime() - last_time_us;

  return cur_interval_us >= (interval_sec * 1000000L);
}

static void *update_global_config(void *arg)
{
  UNUSED(arg);

  int ret = OB_SQL_SUCCESS;
  struct timespec wait_time;
  int64_t last_update_time_us = 0;

  wait_time.tv_sec = OB_SQL_UPDATE_INTERVAL;
  wait_time.tv_nsec = 0;

  while (g_inited)
  {
    last_update_time_us = tbsys::CTimeUtil::getTime();

    pthread_mutex_lock(&g_update_mutex);
    while (! g_update_flag && ! reach_time_(last_update_time_us, OB_SQL_UPDATE_INTERVAL))
    {
      pthread_cond_timedwait(&g_update_cond, &g_update_mutex, &wait_time);
    }
    pthread_mutex_unlock(&g_update_mutex);

    if (! g_inited)
    {
      OB_SQL_LOG(INFO, "update worker stop on destroying");
      break;
    }

    OB_SQL_LOG(INFO, "update worker startup, g_update_flag=%s, update interval=%ld us",
        g_update_flag ? "true" : "false",
        tbsys::CTimeUtil::getTime() - last_update_time_us);

    ret = get_ob_config();
    if (OB_SQL_SUCCESS == ret)
    {
      ret = do_update();
      if (OB_SQL_SUCCESS != ret)
      {
        OB_SQL_LOG(WARN, "update global config failed");
        dump_config(g_config_update, "UPDATE");
      }
    }
    else
    {
      OB_SQL_LOG(WARN, "get mslist failed, update rslist and try again");
      init_rs_list();
    }

    // NOTE: Print connection information after updating
    print_connection_info(&g_group_ds);

    OB_SQL_LOG(INFO, "update worker end, g_update_flag=%s, ret=%d", g_update_flag ? "true" : "false", ret);

    if (OB_SQL_SUCCESS != ret)
    {
      // Force to wait when update fail
      sleep(OB_SQL_UPDATE_FAIL_WAIT_INTERVAL);
    }
  }

  if (NULL != g_func_set.real_mysql_thread_end)
  {
    OB_SQL_LOG(INFO, "update work call mysql_thread_end before exit");
    g_func_set.real_mysql_thread_end();
  }

  OB_SQL_LOG(INFO, "update work exit");
  pthread_exit(NULL);
  return NULL;
}

int start_update_worker()
{
  int ret = OB_SQL_SUCCESS;

  init_rs_list();

  ret = get_ob_config();
  if (OB_SQL_SUCCESS == ret)
  {
    ret = do_update();
    if (OB_SQL_SUCCESS == ret)
    {
      ret = pthread_create(&update_task, NULL, update_global_config, NULL);
      if (OB_SQL_SUCCESS != ret)
      {
        OB_SQL_LOG(ERROR, "start config update worker failed");
        ret = OB_SQL_ERROR;
      }
      else
      {
        OB_SQL_LOG(INFO, "start config update worker");
        update_work_started = true;
      }
    }
    else
    {
      OB_SQL_LOG(ERROR, "update global config failed");
      dump_config(g_config_update, "UPDATE");
    }
  }
  else
  {
    OB_SQL_LOG(ERROR, "get config from oceanbse failed");
  }

  return ret;
}

void stop_update_worker()
{
  if (update_work_started)
  {
    wakeup_update_worker();
    pthread_join(update_task, NULL);
    update_work_started = false;

    OB_SQL_LOG(INFO, "stop update work success");
  }
}
