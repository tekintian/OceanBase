#include "ob_sql_cluster_config.h"
#include "ob_sql_cluster_select.h"
#include "ob_sql_global.h"
#include "ob_sql_util.h"
#include "ob_sql_ms_select.h"         // destroy_ms_select_table
#include "tblog.h"
#include "common/ob_server.h"
#include "ob_sql_cluster_info.h"  // ObSQLClusterType
#include "ob_sql_update_worker.h"   // clear_update_flag
#include <mysql/mysql.h>
#include <string.h>

/// Delete cluster from specific configuration
void delete_cluster_from_config(ObSQLGlobalConfig &config, const int index)
{
  if (0 <= index && index < config.cluster_size_)
  {
    TBSYS_LOG(INFO, "delete cluster={id=%d,addr=%s} from config",
        config.clusters_[index].cluster_id_, get_server_str(&config.clusters_[index].server_));

    for (int idx = index + 1; idx < config.cluster_size_; idx++)
    {
      config.clusters_[idx - 1] = config.clusters_[idx];
    }

    config.cluster_size_--;
  }
}

/**
 * 读取OceanBase集群信息
 * 
 * @param server   listen ms information
 *
 * @return int     return OB_SQL_SUCCESS if get cluster information from listen ms success
 *                                       else return OB_SQL_ERROR
 *
 */
static int get_cluster_config(ObServerInfo *server)
{
  int ret = OB_SQL_ERROR;
  int sindex = 0;
  MYSQL_RES *results = NULL;
  MYSQL_ROW record;

  TBSYS_LOG(INFO, "get_cluster_config: real_mysql_init=%p, MYSQL_FUNC(mysql_init)=%p",
      g_func_set.real_mysql_init, MYSQL_FUNC(mysql_init));

  MYSQL * mysql = (*g_func_set.real_mysql_init)(NULL);
  if (NULL != mysql)
  {
    mysql = (*g_func_set.real_mysql_real_connect)(mysql, get_ip(server), g_sqlconfig.username_, g_sqlconfig.passwd_,
                                                  OB_SQL_DB, server->port_, NULL, 0);
    if (NULL != mysql)
    {
      //get cluster config
      ret = (*g_func_set.real_mysql_query)(mysql, OB_SQL_QUERY_CLUSTER);
      if (0 == ret)
      {
        results = (*g_func_set.real_mysql_store_result)(mysql);
        if (NULL == results)
        {
          TBSYS_LOG(WARN, "store result failed, query is %s errno is %u", OB_SQL_QUERY_CLUSTER,
                    (*g_func_set.real_mysql_errno)(mysql));
          ret = OB_SQL_ERROR;
        }
        else
        {
          int cluster_index = 0;
          int avail_cluster_num = 0;
          uint64_t cluster_num = (*g_func_set.real_mysql_num_rows)(results);
          TBSYS_LOG(DEBUG, "cluster num is %lu", cluster_num);
          if (cluster_num > OB_SQL_MAX_CLUSTER_NUM)
          {
            TBSYS_LOG(ERROR, "there are %lu cluster info, max cluster supported is %d",
                      cluster_num, OB_SQL_MAX_CLUSTER_NUM);
            ret = OB_SQL_ERROR;
          }
          else
          {
            while (OB_SQL_SUCCESS == ret
                   &&(record = (*g_func_set.real_mysql_fetch_row)(results)))
            {
              if (NULL == record[0]    /* cluster_id */
                  || NULL == record[1] /* cluster_role */
                  || NULL == record[3] /* cluster_vip */
                  || NULL == record[4] /* cluster_port */
                 )
              {
                TBSYS_LOG(WARN, "cluster info are not complete: cluster_id=%s, cluster_role=%s, cluster_vip=%s, "
                    "cluster_port=%s",
                    NULL == record[0] ? "NULL" : record[0], NULL == record[1] ? "NULL" : record[1],
                    NULL == record[3] ? "NULL" : record[3], NULL == record[4] ? "NULL" : record[4]);
                ret = OB_SQL_ERROR;
              }
              else
              {
                avail_cluster_num++;
                ObSQLClusterConfig *cconfig = g_config_update->clusters_ + cluster_index++;

                cconfig->cluster_id_ = static_cast<uint32_t>(atoll(record[0]));
                cconfig->cluster_type_ = (1 == atoll(record[1])) ? MASTER_CLUSTER : SLAVE_CLUSTER;
                cconfig->flow_weight_ = NULL == record[2] ? static_cast<int16_t>(0) : static_cast<int16_t>(atoll(record[2]));
                cconfig->server_.ip_ = oceanbase::common::ObServer::convert_ipv4_addr(record[3]);
                cconfig->server_.port_ = static_cast<uint32_t>(atoll(record[4]));
                cconfig->read_strategy_ = NULL == record[5] ? 0 : static_cast<uint32_t>(atoll(record[5]));

                if (g_sqlconfig.read_slave_only_)
                {
                  int16_t old_flow_weight = cconfig->flow_weight_;

                  if (MASTER_CLUSTER == cconfig->cluster_type_)
                  {
                    cconfig->flow_weight_ = 0;
                  }
                  else if (0 == old_flow_weight)
                  {
                    cconfig->flow_weight_ = 1;
                  }

                  TBSYS_LOG(INFO, "re-config cluster flow weight: cluster={id=%d,addr=%s}, flow={old=%d,new=%d}, type=%d",
                      cconfig->cluster_id_, get_server_str(&cconfig->server_),
                      old_flow_weight, cconfig->flow_weight_, cconfig->cluster_type_);
                }

                // insert cluster ip/port to rslist
                insert_rs_list(cconfig->server_.ip_, cconfig->server_.port_);

                TBSYS_LOG(DEBUG, "update config: add cluster id=%d, type=%d, flow=%d, addr=%s, read_strategy=%u",
                    cconfig->cluster_id_, cconfig->cluster_type_, cconfig->flow_weight_,
                    get_server_str(&cconfig->server_), cconfig->read_strategy_);
              }
            }

            TBSYS_LOG(DEBUG, "update config is %p cluster size is %d", g_config_update, avail_cluster_num);
            g_config_update->cluster_size_ = static_cast<int16_t>(avail_cluster_num);
          }

          (*g_func_set.real_mysql_free_result)(results);
          results = NULL;

          //get mslist per cluster
          for (int index = 0; index < g_config_update->cluster_size_
                 && OB_SQL_SUCCESS == ret; ++index)
          {
            sindex = 0;
            char querystr[OB_SQL_QUERY_STR_LENGTH];
            memset(querystr, 0, OB_SQL_QUERY_STR_LENGTH);
            snprintf(querystr, OB_SQL_QUERY_STR_LENGTH, "%s%u", OB_SQL_QUERY_SERVER, g_config_update->clusters_[index].cluster_id_);
            ret = (*g_func_set.real_mysql_query)(mysql, querystr);
            if (0 == ret)
            {
              results = (*g_func_set.real_mysql_store_result)(mysql);
              if (NULL == results)
              {
                TBSYS_LOG(WARN, "store result failed, query is %s errno is %u", querystr,
                          (*g_func_set.real_mysql_errno)(mysql));
                ret = OB_SQL_ERROR;
              }
              else
              {
                uint64_t ms_num = (*g_func_set.real_mysql_num_rows)(results);
                if (ms_num > OB_SQL_MAX_MS_NUM)
                {
                  TBSYS_LOG(ERROR, "cluster has %lu ms more than %d(OB_SQL_MAX_MS_NUM)",
                            ms_num, OB_SQL_MAX_MS_NUM);
                  ret = OB_SQL_ERROR;
                }
                else
                {
                  while ((record = (*g_func_set.real_mysql_fetch_row)(results)))
                  {
                    if (NULL == record[0] || NULL == record[1])
                    {
                      TBSYS_LOG(WARN, "ip or port info is null");
                    }
                    else
                    {
                      g_config_update->clusters_[index].merge_server_[sindex].ip_ = oceanbase::common::ObServer::convert_ipv4_addr(record[0]);
                      g_config_update->clusters_[index].merge_server_[sindex].port_ = static_cast<uint32_t>(atoll(record[1]));
                      sindex++;
                    }
                  }

                  g_config_update->clusters_[index].server_num_ = static_cast<int16_t>(sindex);

                  // NOTE: Delete cluster which has no MS
                  if (0 >= g_config_update->clusters_[index].server_num_)
                  {
                    TBSYS_LOG(WARN, "No avaliable MeregeServer in cluster %d, addr=%s. Delete it from configuration",
                        g_config_update->clusters_[index].cluster_id_,
                        get_server_str(&g_config_update->clusters_[index].server_));

                    delete_cluster_from_config(*g_config_update, index);
                    index--;
                  }
                }
              }
              (*g_func_set.real_mysql_free_result)(results);
              results = NULL;
            }
            else
            {
              ret = OB_SQL_ERROR;
              TBSYS_LOG(WARN, "do query (%s) from %s failed", querystr, get_server_str(server));
              break;
            }
          }
        }
      }
      else
      {
        TBSYS_LOG(WARN, "do query (%s) from %s failed ret is %d", OB_SQL_QUERY_CLUSTER, get_server_str(server), ret);
        ret = OB_SQL_ERROR;
      }
      (*g_func_set.real_mysql_close)(mysql);
    }
    else
    {
      TBSYS_LOG(ERROR, "failed to connect to server %s, Error: %s", get_server_str(server), (*g_func_set.real_mysql_error)(mysql));
      ret = OB_SQL_ERROR;
    }
  }
  else
  {
    TBSYS_LOG(ERROR, "mysql init failed");
  }
  return ret;
}

/* swap g_config_using && g_config_update */
static void swap_config()
{
  ObSQLGlobalConfig * tmp = g_config_using;
  g_config_using = g_config_update;
  g_config_update = tmp;
}

/**
 * 集群个数，流量配置，集群类型是否变化
 */
static bool is_cluster_changed()
{
  bool ret = false;
  if (g_config_update->cluster_size_ != g_config_using->cluster_size_)
  {
    ret = true;
  }
  else
  {
    for (int cindex = 0; cindex < g_config_using->cluster_size_; cindex++)
    {
      if (g_config_update->clusters_[cindex].cluster_id_ != g_config_using->clusters_[cindex].cluster_id_
          ||g_config_update->clusters_[cindex].flow_weight_ != g_config_using->clusters_[cindex].flow_weight_
          ||g_config_update->clusters_[cindex].cluster_type_ != g_config_using->clusters_[cindex].cluster_type_)
      {
        ret = true;
        break;
      }
    }
  }
  return ret;
}

//cluster 没变化
//判断cluster 里面的mslist是否变化
static bool is_mslist_changed(bool cluster_changed)
{
  bool ret = false;

  if (cluster_changed)
  {
    ret = true;
  }
  else
  {
    for (int cindex = 0; false == ret && cindex < g_config_using->cluster_size_; cindex++)
    {
      if (g_config_update->clusters_[cindex].server_num_ != g_config_using->clusters_[cindex].server_num_)
      {
        ret = true;
        break;
      }
      else
      {
        for (int sindex = 0; sindex < g_config_using->clusters_[cindex].server_num_; ++sindex)
        {
          if ((g_config_update->clusters_[cindex].merge_server_[sindex].ip_
                != g_config_using->clusters_[cindex].merge_server_[sindex].ip_)
              || (g_config_update->clusters_[cindex].merge_server_[sindex].port_
                != g_config_using->clusters_[cindex].merge_server_[sindex].port_))
          {
            ret = true;
            break;
          }
        } // end for

        if (true == ret)
        {
          break;
        }
      }
    } // end for
  }

  return ret;
}

static int update_table(ObGroupDataSource *gds)
{
  int ret = OB_SQL_SUCCESS;
  ret = update_select_table(gds);
  if (OB_SQL_SUCCESS != ret)
  {
    TBSYS_LOG(ERROR, "update select table failed");
    dump_config(g_config_using, "USING");
  }
  dump_table();
  return ret;
}

static int rebuild_tables()
{
  int ret = OB_SQL_SUCCESS;
  bool cluster_changed = is_cluster_changed();
  bool mslist_changed = is_mslist_changed(cluster_changed);

  // First update ObGroupDataSource if needed
  if (mslist_changed || cluster_changed)
  {
    TBSYS_LOG(INFO, "update group data source. mslist_changed=%s, cluster_changed=%s",
        mslist_changed ? "true" : "false",
        cluster_changed ? "true" : "false");

    if (OB_SQL_SUCCESS != (ret = update_group_ds(g_config_using, &g_group_ds)))
    {
      TBSYS_LOG(ERROR, "update group data source fail, ret=%d", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "update group data source success");
    }

    dump_config(g_config_using, "USING");
    dump_config(g_config_update, "UPDATE");
  }

// NOTE: Disable updating MS select table
// As this module has BUG which will cause "Core Dump" in consishash_mergeserver_select.
// wanhong.wwh 2014-04-09
#if 0
  if (OB_SQL_SUCCESS == ret && mslist_changed)
  {
    ret = update_ms_select_table();
    if (OB_SQL_SUCCESS != ret)
    {
      TBSYS_LOG(ERROR, "update ms select table failed");
    }
    else
    {
      TBSYS_LOG(DEBUG, "update ms select table success");
    }
  }
#endif

  //集群信息流量变化更新集群选择表
  if (OB_SQL_SUCCESS == ret && cluster_changed)
  {
    TBSYS_LOG(DEBUG, "update select table");
    ret = update_table(&g_group_ds);
    if (OB_SQL_SUCCESS != ret)
    {
      TBSYS_LOG(ERROR, "update select table failed");
    }
    else
    {
      TBSYS_LOG(DEBUG, "update select table success");
    }
  }
  return ret;
}

// 根据rslist 从Oceanbase 获取ms列表 流量信息
// 更新g_config_update的配置
int get_ob_config()
{
  int ret = OB_SQL_SUCCESS;
  TBSYS_LOG(DEBUG, "get_ob_config: rsnum is %d", g_rsnum);
  for (int index = 0; index < g_rsnum; ++index)
  {
    TBSYS_LOG(DEBUG, "listener mergeserver ip is %u, port is %u", g_rslist[index].ip_, g_rslist[index].port_);
    ret = get_cluster_config(g_rslist + index);
    if (OB_SQL_SUCCESS == ret)
    {
      TBSYS_LOG(DEBUG, "Get config information from listen ms(%s) success", get_server_str(g_rslist + index));
      dump_config(g_config_update, "FETCHED");
      break;
    }
    else
    {
      TBSYS_LOG(WARN, "get cluster info from %s failed", get_server_str(g_rslist + index));
    }
  }
  return ret;
}

void destroy_global_config()
{
  TBSYS_LOG(INFO, "destroy g_config_update %p", g_config_update);
  destroy_ms_select_table(g_config_update);    /* g_config_update->clusters_[].table_ */
  (void)memset(g_config_update, 0, sizeof(ObSQLGlobalConfig));

  TBSYS_LOG(INFO, "destroy g_config_using %p", g_config_using);
  destroy_ms_select_table(g_config_using);    /* g_config_using->clusters_[].table_ */
  (void)memset(g_config_using, 0, sizeof(ObSQLGlobalConfig));
}

int do_update()
{
  int ret = OB_SQL_SUCCESS;
  pthread_rwlock_wrlock(&g_config_rwlock); // wlock for update global config
  swap_config();
  ret = rebuild_tables();
  if (OB_SQL_SUCCESS != ret)
  {
    TBSYS_LOG(ERROR, "do_update fail: fail to rebuild table");
  }
  else
  {
    TBSYS_LOG(INFO, "update global config success");
    clear_update_flag();
  }

  pthread_rwlock_unlock(&g_config_rwlock); // unlock
  return ret;
}
