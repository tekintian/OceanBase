/*
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Authors:
 *   Author XiuMing     2014-03-09
 *   Email: wanhong.wwh@alibaba-inc.com
 *     -some work detail if you want
 *
 */

#include "libobsql.h"
#include "ob_sql_global.h"
#include "common/ob_define.h"
#include "ob_sql_define.h"
#include "ob_sql_update_worker.h"
#include "ob_sql_util.h"
#include "ob_sql_cluster_select.h"    // destroy_select_table
#include "ob_sql_cluster_config.h"    // destroy_global_config
#include "common/ob_malloc.h"
#include <string.h>
#include <malloc.h>

using namespace std;
using namespace oceanbase::common;

// Static functions
static void pre_init_();
static int init_sql_config_(const ObSQLConfig &config);
static void destroy_sql_config_();
static void print_config_(const ObSQLConfig &config);

int ob_sql_init(const ObSQLConfig &config)
{
  int ret = OB_SQL_SUCCESS;

  OB_SQL_LOG(INFO, "*********************** LIBOBSQL START ***********************");

  pre_init_();

  if (OB_SQL_SUCCESS != (ret = init_sql_config_(config)))
  {
    OB_SQL_LOG(ERROR, "init_sql_config_ fail, ret=%d", ret);
  }
  //加载libmysqlclient中的函数
  else if (OB_SQL_SUCCESS != (ret = init_func_set(&g_func_set)))
  {
    OB_SQL_LOG(ERROR, "load real mysql function symbol from libmysqlclient failed, ret=%d", ret);
  }
  // Call mysql_library_init before create connection pool
  else if (0 != g_func_set.real_mysql_server_init(0, NULL, NULL))
  {
    OB_SQL_LOG(ERROR, "mysql_server_init fail, real_mysql_server_init=%p", g_func_set.real_mysql_server_init);
  }
  else
  {
    g_inited = 1;

    //从配置服务器获取配置 初始化连接池 集群选择表
    if (OB_SQL_SUCCESS != (ret = start_update_worker()))
    {
      OB_SQL_LOG(ERROR, "start_update_worker fail, ret=%d", ret);
    }
  }

  return ret;
}

void ob_sql_destroy()
{
  OB_SQL_LOG(INFO, "libobsql begins to destroy");

  g_inited = 0;

  stop_update_worker();
  destroy_select_table();         /* g_table */
  destroy_group_ds(&g_group_ds);  /* g_group_ds */
  destroy_global_config();        /* g_config_update and g_config_using */
  destroy_rs_list();              /* g_rsnum and g_rslist */

  // NOTE: Call mysql_thread_end before destroy_func_set
  if (NULL != g_func_set.real_mysql_thread_end)
  {
    g_func_set.real_mysql_thread_end();
    OB_SQL_LOG(INFO, "%s(): Thread %lu call mysql_thread_end", __FUNCTION__, pthread_self());
  }

  // NOTE: Call mysql_server_end before exit
  if (NULL != g_func_set.real_mysql_server_end)
  {
    g_func_set.real_mysql_server_end();
    OB_SQL_LOG(INFO, "%s() call mysql_server_end", __FUNCTION__);
  }

  destroy_func_set(&g_func_set);  /* g_func_set */
  destroy_sql_config_();          /* g_sqlconfig */

  OB_SQL_LOG(INFO, "*********************** LIBOBSQL END ***********************");
}

static void pre_init_()
{
  ::mallopt(M_MMAP_THRESHOLD, OB_SQL_DEFAULT_MMAP_THRESHOLD);
  ob_init_memory_pool();
}

static int init_sql_config_(const ObSQLConfig &config)
{
  int ret = OB_SQL_INVALID_ARGUMENT;

  // NOTE: password can be empty
  if (0 == strlen(config.username_))
  {
    OB_SQL_LOG(ERROR, "invalid argument, user name is empty");
  }
  else if (0 == strlen(config.ip_))
  {
    OB_SQL_LOG(ERROR, "invalid argument, ip is empty");
  }
  else if (0 == config.port_)
  {
    OB_SQL_LOG(ERROR, "invalid argument, port=%u", config.port_);
  }
  else
  {
    g_sqlconfig = config;

    g_sqlconfig.min_conn_ = (0 >= config.min_conn_ ? OB_SQL_DEFAULT_MIN_CONN_SIZE : config.min_conn_);
    g_sqlconfig.max_conn_ = (0 >= config.max_conn_ ? OB_SQL_DEFAULT_MAX_CONN_SIZE : config.max_conn_);

    print_config_(g_sqlconfig);

    //set min max conn
    g_config_using->min_conn_size_ = static_cast<int16_t>(g_sqlconfig.min_conn_);
    g_config_using->max_conn_size_ = static_cast<int16_t>(g_sqlconfig.max_conn_);
    g_config_update->min_conn_size_ = static_cast<int16_t>(g_sqlconfig.min_conn_);
    g_config_update->max_conn_size_ = static_cast<int16_t>(g_sqlconfig.max_conn_);

    ret = OB_SQL_SUCCESS;
  }

  return ret;
}

static void destroy_sql_config_()
{
  (void) memset(&g_sqlconfig, 0, sizeof(g_sqlconfig));
}

static void print_config_(const ObSQLConfig &config)
{
  OB_SQL_LOG(INFO, "=================== LIBOBSQL CONFIG BEGIN ===================");
  OB_SQL_LOG(INFO, "username=%s", config.username_);
  OB_SQL_LOG(INFO, "password=%s", config.passwd_);
  OB_SQL_LOG(INFO, "ip=%s", config.ip_);
  OB_SQL_LOG(INFO, "port=%u", config.port_);
  OB_SQL_LOG(INFO, "min_conn=%d", config.min_conn_);
  OB_SQL_LOG(INFO, "max_conn=%d", config.max_conn_);
  OB_SQL_LOG(INFO, "read_slave_only=%s", config.read_slave_only_ ? "true" : "false");
  OB_SQL_LOG(INFO, "=================== LIBOBSQL CONFIG END ====================");
}

#if 0
static int parseValue(char *str, char *key, char *val)
{
  char           *p, *p1, *name, *value;

  if (str == NULL)
    return -1;

  p = str;
  while ((*p) == ' ' || (*p) == '\t' || (*p) == '\r' || (*p) == '\n') p++;
  p1 = p + strlen(p);
  while(p1 > p) {
    p1 --;
    if (*p1 == ' ' || *p1 == '\t' || *p1 == '\r' || *p1 == '\n') continue;
    p1 ++;
    break;
  }
  (*p1) = '\0';
  if (*p == '#' || *p == '\0') return -1;
  p1 = strchr(str, '=');
  if (p1 == NULL) return -2;
  name = p;
  value = p1 + 1;
  while ((*(p1 - 1)) == ' ') p1--;
  (*p1) = '\0';

  while ((*value) == ' ') value++;
  p = strchr(value, '#');
  if (p == NULL) p = value + strlen(value);
  while ((*(p - 1)) <= ' ') p--;
  (*p) = '\0';
  if (name[0] == '\0')
    return -2;

  strcpy(key, name);
  strcpy(val, value);
  return 0;
}

static int get_config(const char *filename, ObSQLConfig *config)
{
  int ret = OB_SQL_SUCCESS;
  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  ssize_t read = 0;
  char key[1024];
  char value[1024];
  if (NULL == (fp = fopen(filename, "r")))
  {
    //fprintf(stderr, "can not open file %s\n", filename);
    ret = OB_SQL_ERROR;
  }
  else
  {
    //set default config
    memcpy(config->username_, OB_SQL_USER, strlen(OB_SQL_USER));
    memcpy(config->passwd_, OB_SQL_PASS, strlen(OB_SQL_PASS));
    while((read = getline(&line, &len, fp)) != -1)
    {
      parseValue(line, key, value);
      if (0 == memcmp(key, OB_SQL_CONFIG_LOG, strlen(OB_SQL_CONFIG_LOG)))
      {
        memcpy(config->logfile_, value, strlen(value));
      }
// Disable URL
#if 0
      else if (0 == memcmp(key, OB_SQL_CONFIG_URL, strlen(OB_SQL_CONFIG_URL)))
      {
        memcpy(config->url_, value, strlen(value));
      }
#endif
      else if (0 == memcmp(key, OB_SQL_CONFIG_LOG_LEVEL, strlen(OB_SQL_CONFIG_LOG_LEVEL)))
      {
        memcpy(config->loglevel_, value, strlen(value));
      }
      else if (0 == memcmp(key, OB_SQL_CONFIG_MIN_CONN, strlen(OB_SQL_CONFIG_MIN_CONN)))
      {
        config->min_conn_ = atoi(value);
      }
      else if (0 == memcmp(key, OB_SQL_CONFIG_MAX_CONN, strlen(OB_SQL_CONFIG_MAX_CONN)))
      {
        config->max_conn_ = atoi(value);
      }
      else if (0 == memcmp(key, OB_SQL_CONFIG_IP, strlen(OB_SQL_CONFIG_IP)))
      {
        memcpy(config->ip_, value, strlen(value));
        g_rslist[g_rsnum].ip_ = trans_ip_to_int(config->ip_);
      }
      else if (0 == memcmp(key, OB_SQL_CONFIG_PORT, strlen(OB_SQL_CONFIG_PORT)))
      {
        config->port_ = atoi(value);
        g_rslist[g_rsnum].port_ = config->port_;
      }
      else if (0 == memcmp(key, OB_SQL_CONFIG_USERNAME, strlen(OB_SQL_CONFIG_USERNAME)))
      {
        memset(config->username_, 0, OB_SQL_MAX_USER_NAME_LEN);
        memcpy(config->username_, value, strlen(value));
      }
      else if (0 == memcmp(key, OB_SQL_CONFIG_PASSWD, strlen(OB_SQL_CONFIG_PASSWD)))
      {
        memset(config->passwd_, 0, OB_SQL_MAX_PASSWD_LEN);
        memcpy(config->passwd_, value, strlen(value));
      }
    }
    if (0 != config->port_ && 0 != strlen(config->ip_))
    {
      g_rsnum++;
    }
    fclose(fp);
  }
  return ret;
}

static const char* read_env()
{
  const char* config_file = getenv(OB_SQL_CONFIG_ENV);
  if (NULL == config_file)
  {
    config_file = OB_SQL_CONFIG_DEFAULT_NAME;
  }
  return config_file;
}

static int load_config()
{
  int ret = OB_SQL_SUCCESS;
  const char *config_file = read_env();
  ret = get_config(config_file, &g_sqlconfig);
  if (OB_SQL_SUCCESS != ret)
  {
    //OB_SQL_LOG(WARN, "failed to get config %s", config_file);
  }
  return ret;
}
#endif
