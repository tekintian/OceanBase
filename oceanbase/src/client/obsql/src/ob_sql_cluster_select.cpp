#include "ob_sql_mysql_adapter.h"
#include "ob_sql_cluster_select.h"
#include <algorithm>
#include <stdlib.h>
#include "ob_sql_atomic.h"
#include "ob_sql_global.h"
#include "ob_sql_util.h"
#include "common/ob_atomic.h"
#include "common/ob_malloc.h"

using namespace oceanbase::common;

//SelectTable global_select_table;
//更新配置中的flow_weight
static int update_flow_weight(ObGroupDataSource *gds)
{
  int ret = OB_SQL_SUCCESS;
  int slot = 0;
  int used = 0;
  int index = 0;
  int16_t weight = 0;
  if (NULL == gds)
  {
    TBSYS_LOG(ERROR, "invalid argument gds is %p", gds);
    ret = OB_SQL_ERROR;
  }
  else
  {
    slot = 0;
    for (index = 0; index < gds->size_; ++index)
    {
      slot += gds->clusters_[index]->flow_weight_;
    }
    TBSYS_LOG(INFO, "slot is %d", slot);
    for (index = 0; index < gds->size_; ++index)
    {
      if (index < gds->size_ - 1)
      {
        if (0 == slot)
        {
          weight = static_cast<int16_t>(OB_SQL_SLOT_NUM/(gds->size_));
        }
        else
        {
          weight = gds->clusters_[index]->flow_weight_;
          weight = static_cast<int16_t>((weight * OB_SQL_SLOT_NUM) /slot);
        }
        used += weight;
      }
      else
      {
        weight = static_cast<int16_t>(OB_SQL_SLOT_NUM - used);
      }

      gds->clusters_[index]->flow_weight_ = weight;

      TBSYS_LOG(INFO, "cluster(id=%u,addr=%s) weight is %d",
          gds->clusters_[index]->cluster_id_,
          get_server_str(&gds->clusters_[index]->cluster_addr_), weight);
    }
  }
  return ret;
}

/* 根据全局配置来初始化集群选择对照表 加写锁 */
int update_select_table(ObGroupDataSource *gds)
{
  int ret = OB_SQL_SUCCESS;
  if (NULL == gds)
  {
    TBSYS_LOG(ERROR, "invalid argument gds is %p", gds);
    ret = OB_SQL_ERROR;
  }
  else
  {
    ret = update_flow_weight(gds);
    if (OB_SQL_SUCCESS != ret)
    {
      TBSYS_LOG(ERROR, "update_flow_weigth failed");
    }
    else
    {
      if (NULL == g_table)
      {
        g_table = (ObSQLSelectTable *)ob_malloc(sizeof(ObSQLSelectTable), ObModIds::LIB_OBSQL);
        g_table->master_count_ = 0;
        TBSYS_LOG(DEBUG, "g_table->master_count_ is %u", g_table->master_count_);
        if (NULL == g_table)
        {
          TBSYS_LOG(ERROR, "alloc mem for ObSQLSelectTable failed");
          ret = OB_SQL_ERROR;
        }
      }

      if (OB_SQL_SUCCESS == ret)
      {
        ObClusterInfo *master_cluster = get_master_cluster(gds);
        g_table->master_cluster_ = master_cluster;

        TBSYS_LOG(INFO, "update select table: get master cluster=%p, id=%d, addr=%s",
            master_cluster,
            NULL == master_cluster ? -1 : master_cluster->cluster_id_,
            NULL == master_cluster ? "NULL" : get_server_str(&master_cluster->cluster_addr_));

        for (int cindex = 0, sindex = 0; cindex < gds->size_; ++cindex)
        {
          for (int csindex = 0; csindex < gds->clusters_[cindex]->flow_weight_; ++csindex, ++sindex)
          {
            g_table->clusters_[sindex] = gds->clusters_[cindex];
          }
        }

        //shuffle global_table->data
        ObClusterInfo **start = g_table->clusters_;
        ObClusterInfo **end = g_table->clusters_ + OB_SQL_SLOT_NUM;
        std::random_shuffle(start, end);
      }
    }
  }
  return ret;
}

void destroy_select_table()
{
  TBSYS_LOG(INFO, "destroy select table %p", g_table);

  if (NULL != g_table)
  {
    ob_free(g_table);
    g_table = NULL;
  }
}

ObClusterInfo* select_cluster(ObSQLMySQL* mysql)
{
  ObClusterInfo *cluster = NULL;

  if (g_config_using->cluster_size_ > 1)
  {
    if (is_consistence(mysql))
    {
      //一致性请求
      TBSYS_LOG(DEBUG, "consistence g_table->master_count_ is %u", g_table->master_count_);
      cluster = g_table->master_cluster_;
      TBSYS_LOG(DEBUG, "consistence cluster=%p, id = %u flow weight is %d ",
          cluster,
          NULL == cluster ? -1 : cluster->cluster_id_,
          NULL == cluster ? -1 : cluster->flow_weight_);

      atomic_inc(&g_table->master_count_);
    }
    else
    {
      while (1)
      {
        cluster = g_table->clusters_[atomic_inc(&g_table->cluster_index_) % OB_SQL_SLOT_NUM];
        TBSYS_LOG(DEBUG, "select g_table->master_count_ is %u", g_table->master_count_);
        if (MASTER_CLUSTER == cluster->cluster_type_)
        {
          if (atomic_dec_positive(&g_table->master_count_) < 0)
          {
            break;
          }
        }
        else
        {
          break;
        }
      }

      if (NULL == cluster)
      {
        ObClusterInfo *master_cluster = g_table->master_cluster_;

        // NOTE: As to read_slave_only mode, master cluster may be not in select table.
        if (NULL != master_cluster)
        {
          TBSYS_LOG(WARN, "no cluster available in select table, select master cluster");
          cluster = master_cluster;
        }
        else
        {
          TBSYS_LOG(WARN, "no cluster available in select table and master cluster=NULL, cluster_size=%d",
              g_config_using->cluster_size_);
        }
      }
    }
  }
  else if (1 == g_config_using->cluster_size_)
  {
    cluster = g_table->clusters_[0];
  }

  if (NULL != cluster)
  {
    TBSYS_LOG(DEBUG, "select cluster: ID=%u FLOW=%d, CSIZE=%d, LMS=[%s], TYPE=%d, RDST=%d",
        cluster->cluster_id_, cluster->flow_weight_, cluster->size_,
        get_server_str(&cluster->cluster_addr_), cluster->cluster_type_, cluster->read_strategy_);
  }
  else
  {
    TBSYS_LOG(WARN, "select cluster fail, cluster size=%d", g_config_using->cluster_size_);
  }

  return cluster;
}
