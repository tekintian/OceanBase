////===================================================================
 //
 // ob_log_mysql_adaptor.cpp liboblog / Oceanbase
 //
 // Copyright (C) 2013 Alipay.com, Inc.
 //
 // Created on 2013-07-23 by Yubai (yubai.lk@alipay.com) 
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

#include "ob_log_utils.h"
#include "ob_log_mysql_adaptor.h"
#include "obmysql/ob_mysql_util.h"      // get_mysql_type

namespace oceanbase
{
  using namespace common;
  namespace liboblog
  {
    int ObLogMysqlAdaptor::TableSchemaContainer::init(const ObLogSchema *schema, uint64_t table_id)
    {
      int ret = OB_SUCCESS;

      if (NULL == (total_schema = schema) || OB_INVALID_ID == table_id)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else if (NULL == (table_schema = total_schema->get_table_schema(table_id)))
      {
        TBSYS_LOG(ERROR, "get_table_schema fail, table_id=%lu", table_id);
        ret = OB_ERR_UNEXPECTED;
      }
      else if (NULL == (column_schema_array = total_schema->get_table_schema(table_id, column_number)))
      {
        TBSYS_LOG(ERROR, "get_table_schema fail, table_id=%lu", table_id);
        ret = OB_ERR_UNEXPECTED;
      }
      else
      {
        inited = true;
      }

      return ret;
    }

    ObLogMysqlAdaptor::ObLogMysqlAdaptor() : inited_(false),
                                             mysql_port_(0),
                                             mysql_addr_(NULL),
                                             mysql_user_(NULL),
                                             mysql_password_(NULL),
                                             read_consistency_(NULL),
                                             schema_version_(OB_INVALID_VERSION),
                                             tc_info_map_(),
                                             allocator_()
    {
      int tmp_ret = OB_SUCCESS;

      allocator_.set_mod_id(ObModIds::OB_LOG_MYSQL_ADAPTOR);

      // Initialize FIFOAllocator in constructor.
      // NOTE: [0.4.2] FIFOAllocator has BUG: It cannot be re-initialized after being destroyed.
      // So we have to initialize FIFOAllocator in constructor and destroy it in destructor.
      // TODO: Move this to init() function in [0.5] version
      if (OB_SUCCESS != (tmp_ret = allocator_.init(MYSQL_ADAPTOR_SIZE_TOTAL_LIMIT,
            MYSQL_ADAPTOR_SIZE_HOLD_LIMIT, ALLOCATOR_PAGE_SIZE)))
      {
        TBSYS_LOG(ERROR, "initialize FIFOAllocator fail, ret=%d, total_limit=%ld, "
            "hold_limit=%ld, page_size=%ld",
            tmp_ret, MYSQL_ADAPTOR_SIZE_TOTAL_LIMIT,
            MYSQL_ADAPTOR_SIZE_HOLD_LIMIT, ALLOCATOR_PAGE_SIZE);

        abort();
      }
    }

    ObLogMysqlAdaptor::~ObLogMysqlAdaptor()
    {
      destroy();

      // NOTE: Destroy FIFOAllocator to avoid re-initialization
      // TODO: Move this to destroy() function in [0.5] version
      allocator_.destroy();
    }

    int ObLogMysqlAdaptor::get_mysql_adaptor(ObLogMysqlAdaptor *&mysql_adaptor,
        const ObLogConfig &config,
        const ObLogSchema *total_schema)
    {
      int ret = OB_SUCCESS;

      ObLogMysqlAdaptor *tmp_mysql_adaptor = GET_TSI_MULT(ObLogMysqlAdaptor, TSI_LIBOBLOG_MYSQL_ADAPTOR);
      if (NULL == tmp_mysql_adaptor)
      {
        TBSYS_LOG(ERROR, "GET_TSI_MULT get ObLogMysqlAdaptor fail, return NULL");
        ret = OB_ERR_UNEXPECTED;
      }
      else if (! tmp_mysql_adaptor->is_inited())
      {
        ret = tmp_mysql_adaptor->init(config, total_schema);
        if (OB_SUCCESS != ret)
        {
          if (OB_SCHEMA_ERROR == ret)
          {
            TBSYS_LOG(WARN, "initialize MySQL Adaptor with schema (%ld,%ld) fail, ret=%d",
                total_schema->get_version(), total_schema->get_timestamp(), ret);
          }
          else
          {
            TBSYS_LOG(ERROR, "failed to initialize ObLogMysqlAdaptor, ret=%d", ret);
          }

          tmp_mysql_adaptor = NULL;
        }
      }

      if (OB_SUCCESS == ret)
      {
        mysql_adaptor = tmp_mysql_adaptor;
      }

      return ret;
    }

    int ObLogMysqlAdaptor::init(const ObLogConfig &config, const ObLogSchema *total_schema)
    {
      int ret = OB_SUCCESS;

      if (inited_)
      {
        ret = OB_INIT_TWICE;
      }
      else if (NULL == (mysql_addr_ = config.get_mysql_addr())
              || 0 >= (mysql_port_ = config.get_mysql_port())
              || NULL == (mysql_user_ = config.get_mysql_user())
              || NULL == (mysql_password_ = config.get_mysql_password())
              || NULL == total_schema)
      {
        TBSYS_LOG(ERROR, "invalid configuration or arguments: mysql_addr=%s, mysql_port=%d "
            "mysql_user=%s, mysql_password=%s, total_schema=%p",
            NULL == mysql_addr_ ? "" : mysql_addr_,
            mysql_port_,
            NULL == mysql_user_ ? "" : mysql_user_,
            NULL == mysql_password_ ? "" : mysql_password_,
            total_schema);
        ret = OB_INVALID_ARGUMENT;
      }
      else
      {
        // If read_consistency_ is NULL, we will not provide read consistency
        read_consistency_ = config.get_query_back_read_consistency();
      }

      if (OB_SUCCESS == ret)
      {
        TBSYS_LOG(INFO, "Initialize MySQL Adaptor based on schema version=%ld, timestamp=%ld",
            total_schema->get_version(), total_schema->get_timestamp());

        // Create tc_info_map_
        if (0 != tc_info_map_.create(TC_INFO_MAP_SIZE))
        {
          TBSYS_LOG(WARN, "tc_info_map_ create fail, size=%ld", TC_INFO_MAP_SIZE);
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
        else if (OB_SUCCESS != (ret = ObLogConfig::parse_tb_select(config.get_tb_select(), *this, total_schema)))
        {
          TBSYS_LOG(WARN, "parse table selected fail with schema (%ld,%ld), ret=%d",
              total_schema->get_version(), total_schema->get_timestamp(), ret);
        }
        else
        {
          schema_version_ = total_schema->get_version();
          inited_ = true;
        }
      }

      if (OB_SUCCESS != ret)
      {
        destroy();
      }
      return ret;
    }

    void ObLogMysqlAdaptor::destroy()
    {
      inited_ = false;

      destroy_tc_info_map_();

      schema_version_ = OB_INVALID_VERSION;

      read_consistency_ = NULL;
      mysql_password_ = NULL;
      mysql_user_ = NULL;
      mysql_addr_ = NULL;
      mysql_port_ = 0;

      // Call mysql_thread_end for every formator thread
      ob_mysql_thread_end();
      TBSYS_LOG(INFO, "Thread %lu: mysql adaptor calls mysql_thread_end", pthread_self());
    }

    bool ObLogMysqlAdaptor::is_time_type(const ObObjType type)
    {
      bool bret = false;
      if (ObDateTimeType == type
          || ObPreciseDateTimeType == type
          || ObCreateTimeType == type
          || ObModifyTimeType == type)
      {
        bret = true;
      }
      return bret;
    }

    int ObLogMysqlAdaptor::operator ()(const char *tb_name, const ObLogSchema *total_schema)
    {
      int ret = OB_SUCCESS;
      const ObTableSchema *table_schema = NULL;

      if (NULL == tb_name || NULL == total_schema)
      {
        TBSYS_LOG(ERROR, "invalid argument: tb_name=%s, total_schema=%p",
            NULL == tb_name ? "NULL" : tb_name, total_schema);
        ret = OB_INVALID_ARGUMENT;
      }
      else if (NULL == (table_schema = total_schema->get_table_schema(tb_name)))
      {
        TBSYS_LOG(WARN, "get_table_schema fail, tb_name=%s", tb_name);
        ret = OB_ERR_UNEXPECTED;
      }
      else if (OB_SUCCESS != (ret = update_tc_info_(table_schema->get_table_id(), total_schema)))
      {
        if (OB_SCHEMA_ERROR != ret)
        {
          TBSYS_LOG(ERROR, "update_tc_info_ fail, tb_name=%s, table_id=%lu, total_schema=%p, ret=%d",
              tb_name, table_schema->get_table_id(), total_schema, ret);
        }
      }

      return ret;
    }

    int ObLogMysqlAdaptor::query_whole_row(const uint64_t table_id,
        const common::ObRowkey &rowkey,
        const ObLogSchema *asked_schema,
        const IObLogColValue *&list)
    {
      int ret = OB_SUCCESS;
      if (!inited_)
      {
        ret = OB_NOT_INIT;
      }
      else if (OB_INVALID_ID == table_id || NULL == asked_schema)
      {
        ret = OB_INVALID_ARGUMENT;
        TBSYS_LOG(ERROR, "invalid arguments, table_id=%lu, asked_schema=%p", table_id, asked_schema);
      }
      else if (OB_INVALID_VERSION == schema_version_ || asked_schema->get_version() > schema_version_)
      {
        // Update all TCInfo based on new schema
        if (OB_SUCCESS != (ret = update_all_tc_info_(asked_schema)))
        {
          if (OB_SCHEMA_ERROR != ret)
          {
            TBSYS_LOG(ERROR, "update_all_tc_info_ fail, ret=%d", ret);
          }

          // NOTE: Schema version should be set as invalid value because TCInfo data has been
          // in uncertain state.
          schema_version_ = OB_INVALID_VERSION;
        }
        else
        {
          schema_version_ = asked_schema->get_version();
        }
      }
      else if (asked_schema->get_version() < schema_version_)
      {
        TBSYS_LOG(ERROR, "invalid arguments: asked schema version %ld is less than local schema version %ld",
            asked_schema->get_version(), schema_version_);
        ret = OB_INVALID_ARGUMENT;
      }

      if (OB_SUCCESS == ret)
      {
        ret = query_data_(table_id, rowkey, asked_schema, list);
        if (OB_SUCCESS != ret && OB_ENTRY_NOT_EXIST != ret && OB_SCHEMA_ERROR != ret)
        {
          TBSYS_LOG(ERROR, "query_data_ fail, table_id=%lu, ret=%d", table_id, ret);
        }
      }

      if (OB_SCHEMA_ERROR == ret)
      {
        TBSYS_LOG(DEBUG, "query data fail with schema version=%ld, timestamp=%ld. table_id=%lu, "
            "schema should be updated.",
            asked_schema->get_version(), asked_schema->get_timestamp(), table_id);
      }

      return ret;
    }

    int ObLogMysqlAdaptor::query_data_(const uint64_t table_id,
        const common::ObRowkey &rowkey,
        const ObLogSchema *asked_schema,
        const IObLogColValue *&list)
    {
      OB_ASSERT(inited_ && OB_INVALID_ID != table_id && NULL != asked_schema
          && OB_INVALID_VERSION != schema_version_
          && asked_schema->get_version() == schema_version_);

      int ret = OB_SUCCESS;
      int32_t retry_times = 0;
      TCInfo *tc_info = NULL;

      while (MAX_MYSQL_FAIL_RETRY_TIMES >= ++retry_times)
      {
        tc_info = NULL;
        if (OB_SUCCESS != (ret = get_tc_info_(table_id, tc_info)))
        {
          TBSYS_LOG(ERROR, "get_tc_info fail, table_id=%lu", table_id);
          break;
        }
        else if (OB_NEED_RETRY != (ret = execute_query_(tc_info, rowkey)))
        {
          break;
        }

        if (MAX_MYSQL_FAIL_RETRY_TIMES <= retry_times)
        {
          TBSYS_LOG(ERROR, "execute_query_ fail, table_id=%lu, re-prepare retry times=%d/%d",
              table_id, retry_times, MAX_MYSQL_FAIL_RETRY_TIMES);
          ret = OB_ERR_UNEXPECTED;
          break;
        }

        // Re-Prepare when execute fails
        if (OB_SUCCESS != (ret = update_all_tc_info_(asked_schema)))
        {
          if (OB_SCHEMA_ERROR != ret)
          {
            TBSYS_LOG(ERROR, "fail to re-prepare, update_all_tc_info_ fail, ret=%d", ret);
          }

          // NOTE: Schema version should be set as invalid value because TCInfo data has been
          // in uncertain state.
          schema_version_ = OB_INVALID_VERSION;
          break;
        }
      }

      if (OB_SUCCESS == ret)
      {
        if (1 < retry_times)
        {
          TBSYS_LOG(WARN, "execute_query_ success after retry times=%d/%d", retry_times, MAX_MYSQL_FAIL_RETRY_TIMES);
        }

        list = tc_info->res_list;
      }
      else if (OB_ENTRY_NOT_EXIST != ret && OB_SCHEMA_ERROR != ret)
      {
        TBSYS_LOG(WARN, "execute_query_ fail, retry times=%d/%d, table_id=%lu, ret=%d",
            retry_times, MAX_MYSQL_FAIL_RETRY_TIMES, table_id, ret);
      }

      return ret;
    }

    int ObLogMysqlAdaptor::update_tc_info_(uint64_t table_id, const ObLogSchema *total_schema)
    {
      OB_ASSERT(OB_INVALID_ID != table_id && NULL != total_schema);

      int ret = OB_SUCCESS;
      int hash_ret = 0;
      TCInfo *tc_info = NULL;

      // Get TCInfo
      if (-1 == (hash_ret = tc_info_map_.get(table_id, tc_info)))
      {
        TBSYS_LOG(ERROR, "get tc_info from tc_info_map_ fail, table_id=%lu, hash_ret=%d",
            table_id, hash_ret);

        ret = OB_ERR_UNEXPECTED;
      }
      else
      {
        // Free TCInfo if exists
        if (NULL != tc_info)
        {
          destruct_tc_info_(tc_info);
          tc_info = NULL;
        }

        // Construct a new TCInfo
        if (OB_SUCCESS != (ret = construct_tc_info_(tc_info, table_id, total_schema)))
        {
          if (OB_SCHEMA_ERROR != ret)
          {
            TBSYS_LOG(ERROR, "construct_tc_info_ fail, table_id=%lu, tc_info=%p, total_schema=%p, ret=%d",
                table_id, tc_info, total_schema, ret);
          }

          // NOTE: tc_info must be set to NULL on fail as we will fill tc_info subsequently anyway.
          tc_info = NULL;
        }

        // NOTE: Fill tc_info anyway regard less the value of "ret".
        //       In this way, this function will be reenterable.
        //
        // Fill TCInfoMap and overwrite if exists
        if (-1 == (hash_ret = tc_info_map_.set(table_id, tc_info, 1)))
        {
          TBSYS_LOG(ERROR, "set tc_info_map_ fail, table_id=%lu, tc_info=%p, hash_ret=%d",
              table_id, tc_info, hash_ret);

          ret = OB_ERR_UNEXPECTED;
        }
      }

      return ret;
    }

    int ObLogMysqlAdaptor::update_all_tc_info_(const ObLogSchema *total_schema)
    {
      OB_ASSERT(inited_ && NULL != total_schema);

      int ret = OB_SUCCESS;

      TCInfoMap::iterator iter;
      for (iter = tc_info_map_.begin(); OB_SUCCESS == ret && iter != tc_info_map_.end(); iter++)
      {
        uint64_t table_id = iter->first;
        if (OB_SUCCESS != (ret = update_tc_info_(table_id, total_schema)))
        {
          if (OB_SCHEMA_ERROR != ret)
          {
            TBSYS_LOG(WARN, "update table %lu TCInfo fail, ret=%d", table_id, ret);
          }

          break;
        }
      }

      return ret;
    }

    int ObLogMysqlAdaptor::get_tc_info_(const uint64_t table_id, TCInfo *&tc_info)
    {
      OB_ASSERT(OB_INVALID_ID != table_id && NULL == tc_info);

      int ret = OB_SUCCESS;
      TCInfo *tmp_tc_info = NULL;
      int hash_ret = tc_info_map_.get(table_id, tmp_tc_info);
      if (hash::HASH_NOT_EXIST == hash_ret)
      {
        // TODO: Support drop/create the same table
        TBSYS_LOG(ERROR, "table ID=%lu does not exist", table_id);
        ret = OB_NOT_SUPPORTED;
      }
      else if (-1 == hash_ret || NULL == tmp_tc_info)
      {
        TBSYS_LOG(ERROR, "get TCInfo from tc_info_map_ fail, table_id=%lu, tc_info=%p, hash_ret=%d",
            table_id, tmp_tc_info, hash_ret);
        ret = OB_ERR_UNEXPECTED;
      }
      else
      {
        tc_info = tmp_tc_info;
      }

      return ret;
    }

    int ObLogMysqlAdaptor::execute_query_(TCInfo *tc_info, const common::ObRowkey &rowkey)
    {
      int ret = OB_SUCCESS;
      if (rowkey.get_obj_cnt() != tc_info->rk_column_num)
      {
        TBSYS_LOG(WARN, "rowkey column num not match, rowkey num in mutator=%ld, rowkey num in schema=%ld",
                  rowkey.get_obj_cnt(), tc_info->rk_column_num);
        ret = OB_INVALID_DATA;
      }
      else
      {
        for (int64_t i = 0; i < rowkey.get_obj_cnt(); i++)
        {
          if (is_time_type(rowkey.get_obj_ptr()[i].get_type()))
          {
            int64_t tm_raw = *(int64_t*)rowkey.get_obj_ptr()[i].get_data_ptr();
            time_t tm_s = (time_t)tm_raw;
            int64_t usec = 0;
            if (ObDateTimeType != rowkey.get_obj_ptr()[i].get_type())
            {
              tm_s = (time_t)(tm_raw / 1000000);
              usec = tm_raw % 1000000;
            }
            struct tm tm;
            localtime_r(&tm_s, &tm);
            tc_info->tm_data[i].year = tm.tm_year + 1900;
            tc_info->tm_data[i].month = tm.tm_mon + 1;
            tc_info->tm_data[i].day = tm.tm_mday;
            tc_info->tm_data[i].hour = tm.tm_hour;
            tc_info->tm_data[i].minute = tm.tm_min;
            tc_info->tm_data[i].second = tm.tm_sec;
            tc_info->tm_data[i].second_part = usec;
            tc_info->params[i].buffer = &tc_info->tm_data[i];
            tc_info->params[i].buffer_length = sizeof(tc_info->tm_data[i]);
          }
          else
          {
            tc_info->params[i].buffer = const_cast<void*>(rowkey.get_obj_ptr()[i].get_data_ptr());
            tc_info->params[i].buffer_length = rowkey.get_obj_ptr()[i].get_data_length();
          }
        }
        int tmp_ret = 0;
        if (0 != ob_mysql_stmt_bind_param(tc_info->stmt, tc_info->params))
        {
          TBSYS_LOG(WARN, "mysql_stmt_bind_param fail, errno=%u, %s",
              ob_mysql_stmt_errno(tc_info->stmt), ob_mysql_stmt_error(tc_info->stmt));

          ret = OB_ERR_UNEXPECTED;
        }
        //TODO oceanbase crash retry
        else if (0 != TIMED_FUNC(ob_mysql_stmt_execute, 1000000, tc_info->stmt))
        {
          TBSYS_LOG(WARN, "mysql_stmt_execute fail, errno=%u, %s",
              ob_mysql_stmt_errno(tc_info->stmt), ob_mysql_stmt_error(tc_info->stmt));

          if (is_schema_error_(ob_mysql_stmt_errno(tc_info->stmt)))
          {
            // TCInfo should be updated with newer schema
            ret = OB_SCHEMA_ERROR;
          }
          else
          {
            // NOTE: Retry when falling into any other error state.
            ret = OB_NEED_RETRY;
          }
        }
        else if (0 != ob_mysql_stmt_bind_result(tc_info->stmt, tc_info->res_idx))
        {
          TBSYS_LOG(WARN, "mysql_stmt_bind_result fail, errno=%u, %s",
              ob_mysql_stmt_errno(tc_info->stmt), ob_mysql_stmt_error(tc_info->stmt));

          ret = OB_ERR_UNEXPECTED;
        }
        else if (0 != (tmp_ret = ob_mysql_stmt_fetch(tc_info->stmt)))
        {
          if (MYSQL_NO_DATA == tmp_ret)
          {
            ret = OB_ENTRY_NOT_EXIST;
          }
          else
          {
            TBSYS_LOG(WARN, "mysql_stmt_fetch fail, errno=%u, %s",
                ob_mysql_stmt_errno(tc_info->stmt), ob_mysql_stmt_error(tc_info->stmt));

            ret = OB_ERR_UNEXPECTED;
          }
        }
        else
        {
          ob_mysql_stmt_free_result(tc_info->stmt);
        }
      }
      return ret;
    }

    int ObLogMysqlAdaptor::construct_tc_info_(TCInfo *&tc_info, uint64_t table_id, const ObLogSchema *total_schema)
    {
      OB_ASSERT(NULL == tc_info && OB_INVALID_ID != table_id && NULL != total_schema);

      int ret = OB_SUCCESS;
      TCInfo *tmp_tc_info = NULL;
      TableSchemaContainer tsc;
      tsc.reset();

      if (NULL == (tmp_tc_info = (TCInfo *)allocator_.alloc(sizeof(TCInfo))))
      {
        TBSYS_LOG(ERROR, "alloc TCInfo fail");
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      else if (OB_SUCCESS != (ret = tsc.init(total_schema, table_id)))
      {
        TBSYS_LOG(ERROR, "set TableSchemaContainer fail, table_id=%lu, ret=%d", table_id, ret);
      }
      else
      {
        (void)memset(tmp_tc_info, 0, sizeof(TCInfo));

        // Initialize PS SQL
        if (OB_SUCCESS != (ret = init_ps_sql_(tmp_tc_info->sql,
                TCInfo::PS_SQL_BUFFER_SIZE,
                tsc)))
        {
          TBSYS_LOG(ERROR, "init_ps_sql_ fail, table_id=%lu, ret=%d", table_id, ret);
        }
        else if (OB_SUCCESS != (ret = init_mysql_stmt_(tmp_tc_info->mysql,
                tmp_tc_info->stmt,
                tmp_tc_info->sql,
                static_cast<int64_t>(strlen(tmp_tc_info->sql)))))
        {
          if (OB_SCHEMA_ERROR != ret)
          {
            TBSYS_LOG(ERROR, "init_mysql_stmt_ fail, sql=%s, ret=%d", tmp_tc_info->sql, ret);
          }
          tmp_tc_info->stmt = NULL;
          tmp_tc_info->mysql = NULL;
        }
        else if (OB_SUCCESS != (ret = init_mysql_params_(tmp_tc_info->params,
                tmp_tc_info->tm_data,
                tsc)))
        {
          TBSYS_LOG(ERROR, "init_mysql_params_ fail, ret=%d", ret);
          tmp_tc_info->params = NULL;
          tmp_tc_info->tm_data = NULL;
        }
        else if (OB_SUCCESS != (ret = init_mysql_result_(tmp_tc_info->res_idx,
                tmp_tc_info->res_list,
                tsc)))
        {
          TBSYS_LOG(ERROR, "init_mysql_result_ fail, ret=%d", ret);
          tmp_tc_info->res_idx = NULL;
          tmp_tc_info->res_list = NULL;
        }
        else
        {
          tmp_tc_info->rk_column_num = tsc.table_schema->get_rowkey_info().get_size();
        }
      }

      if (NULL != tmp_tc_info)
      {
        if (OB_SUCCESS != ret)
        {
          destruct_tc_info_(tmp_tc_info);
          tmp_tc_info = NULL;
        }
        else
        {
          tc_info = tmp_tc_info;
        }
      }

      return ret;
    }

    void ObLogMysqlAdaptor::destruct_tc_info_(TCInfo *tc_info)
    {
      if (NULL != tc_info)
      {
        // Destroy mysql result
        destroy_mysql_result_(tc_info->res_idx, tc_info->res_list);
        tc_info->res_idx = NULL;
        tc_info->res_list = NULL;

        // Destroy mysql params
        destroy_mysql_params_(tc_info->params, tc_info->tm_data);
        tc_info->params = NULL;
        tc_info->tm_data = NULL;

        // Destroy mysql stmt
        destroy_mysql_stmt_(tc_info->mysql, tc_info->stmt);
        tc_info->mysql = NULL;
        tc_info->stmt = NULL;

        // Free TCInfo
        tc_info->clear();
        allocator_.free(reinterpret_cast<char *>(tc_info));
        tc_info = NULL;
      }
    }

    int ObLogMysqlAdaptor::init_ps_sql_(char *sql, int64_t size, const TableSchemaContainer &tsc)
    {
      OB_ASSERT(0 < size && tsc.inited);

      int ret = OB_SUCCESS;
      int column_num = tsc.column_number;
      const ObLogSchema *total_schema = tsc.total_schema;
      const ObColumnSchemaV2 *column_schema = tsc.column_schema_array;
      const ObTableSchema *table_schema = tsc.table_schema;
      uint64_t table_id = tsc.table_schema->get_table_id();

      int64_t pos = 0;
      if (NULL != read_consistency_)
      {
        // Add read consistency
        databuff_printf(sql, size, pos, "select /*+read_consistency(%s)*/ ", read_consistency_);
      }
      else
      {
        databuff_printf(sql, size, pos, "select ");
      }

      for (int64_t i = 0; i < column_num; i++)
      {
        databuff_printf(sql, size, pos, "%s", column_schema[i].get_name());
        if (i < (column_num - 1))
        {
          databuff_printf(sql, size, pos, ",");
        }
        else
        {
          databuff_printf(sql, size, pos, " ");
        }
      }

      databuff_printf(sql, size, pos, "from %s where ", table_schema->get_table_name());

      for (int64_t i = 0; i < table_schema->get_rowkey_info().get_size(); i++)
      {
        uint64_t column_id = table_schema->get_rowkey_info().get_column(i)->column_id_;
        const ObColumnSchemaV2 *rk_column_schema = total_schema->get_column_schema(table_id, column_id);
        databuff_printf(sql, size, pos, "%s=?", rk_column_schema->get_name());
        if (i < (table_schema->get_rowkey_info().get_size() - 1))
        {
          databuff_printf(sql, size, pos, " and ");
        }
        else
        {
          databuff_printf(sql, size, pos, ";");
        }
      }

      TBSYS_LOG(INFO, "build sql succ, table_id=%lu [%s]", table_id, sql);

      return ret;
    }

    int ObLogMysqlAdaptor::init_mysql_stmt_(ObSQLMySQL *&mysql, MYSQL_STMT *&stmt, const char *sql, int64_t sql_len)
    {
      OB_ASSERT(NULL != sql && 0 < sql_len
          && NULL != mysql_addr_ && NULL != mysql_user_ && NULL != mysql_password_ && 0 < mysql_port_);

      int ret = OB_SUCCESS;
      int retry_times = 0;

      while (MAX_MYSQL_FAIL_RETRY_TIMES >= ++retry_times)
      {
        if (OB_NEED_RETRY != (ret = init_mysql_stmt_internal_(mysql, stmt, sql, sql_len)))
        {
          break;
        }

        if (MAX_MYSQL_FAIL_RETRY_TIMES <= retry_times)
        {
          TBSYS_LOG(ERROR, "fail to init mysql statement, retry_times=%d/%d", retry_times, MAX_MYSQL_FAIL_RETRY_TIMES);
          ret = OB_ERR_UNEXPECTED;
          break;
        }
      }

      if (OB_SUCCESS == ret && 1 < retry_times)
      {
        TBSYS_LOG(WARN, "init_mysql_stmt_ success after retry %d/%d times", retry_times, MAX_MYSQL_FAIL_RETRY_TIMES);
      }

      return ret;
    }

    int ObLogMysqlAdaptor::init_mysql_stmt_internal_(ObSQLMySQL *&mysql, MYSQL_STMT *&stmt, const char *sql, int64_t sql_len)
    {
      OB_ASSERT(NULL != sql && 0 < sql_len
          && NULL != mysql_addr_ && NULL != mysql_user_ && NULL != mysql_password_ && 0 < mysql_port_);

      int ret = OB_SUCCESS;
      ObSQLMySQL *tmp_mysql = NULL;
      MYSQL_STMT *tmp_stmt = NULL;

      if (NULL == (tmp_mysql = ob_mysql_init(NULL)))
      {
        TBSYS_LOG(WARN, "ob_mysql_init fail");
        ret = OB_INIT_FAIL;
      }
      else if (tmp_mysql != ob_mysql_real_connect(tmp_mysql,
            mysql_addr_,
            mysql_user_,
            mysql_password_,
            OB_MYSQL_DATABASE,
            mysql_port_,
            NULL, 0))
      {
        TBSYS_LOG(WARN, "ob_mysql_real_connect fail, mysql_addr=%s, mysql_port=%d, errno=%u, %s",
            mysql_addr_, mysql_port_, ob_mysql_errno(tmp_mysql), ob_mysql_error(tmp_mysql));
        ret = OB_INIT_FAIL;
      }
      else if (NULL == (tmp_stmt = ob_mysql_stmt_init(tmp_mysql)))
      {
        TBSYS_LOG(WARN, "ob_mysql_stmt_init_ fail, errno=%u, %s",
            ob_mysql_errno(tmp_mysql), ob_mysql_error(tmp_mysql));
        ret = OB_NEED_RETRY;
      }
      else if (0 != ob_mysql_stmt_prepare(tmp_stmt, sql, sql_len))
      {
        TBSYS_LOG(WARN, "ob_mysql_stmt_prepare fail, errno=%u, %s",
            ob_mysql_stmt_errno(tmp_stmt), ob_mysql_stmt_error(tmp_stmt));

        if (is_schema_error_(ob_mysql_stmt_errno(tmp_stmt)))
        {
          ret = OB_SCHEMA_ERROR;
        }
        else
        {
          ret = OB_NEED_RETRY;
        }
      }

      if (OB_SUCCESS != ret || (NULL == tmp_mysql || NULL == tmp_stmt))
      {
        destroy_mysql_stmt_(tmp_mysql, tmp_stmt);
        tmp_mysql = NULL;
        tmp_stmt = NULL;

        ret = OB_SUCCESS == ret ? OB_ERR_UNEXPECTED : ret;
      }
      else
      {
        mysql = tmp_mysql;
        stmt = tmp_stmt;
      }

      return ret;
    }

    void ObLogMysqlAdaptor::destroy_mysql_stmt_(ObSQLMySQL *mysql, MYSQL_STMT *stmt)
    {
      if (NULL != stmt)
      {
        ob_mysql_stmt_close(stmt);
        stmt = NULL;
      }

      if (NULL != mysql)
      {
        ob_mysql_close(mysql);
        mysql = NULL;
      }
    }

    int ObLogMysqlAdaptor::init_mysql_params_(MYSQL_BIND *&params,
        MYSQL_TIME *&tm_data,
        const TableSchemaContainer &tsc)
    {
      OB_ASSERT(tsc.inited);

      MYSQL_BIND *tmp_params = NULL;
      MYSQL_TIME *tmp_tm_data = NULL;

      int ret = OB_SUCCESS;
      const ObTableSchema *table_schema = tsc.table_schema;
      int64_t rk_num = table_schema->get_rowkey_info().get_size();

      if (NULL == (tmp_params = (MYSQL_BIND*)allocator_.alloc(sizeof(MYSQL_BIND) * rk_num)))
      {
        TBSYS_LOG(WARN, "alloc params fail, size=%ld", rk_num);
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      else if (NULL == (tmp_tm_data = (MYSQL_TIME*)allocator_.alloc(sizeof(MYSQL_TIME) * rk_num)))
      {
        TBSYS_LOG(WARN, "alloc tm_data fail, size=%ld", rk_num);
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      else
      {
        (void)memset(tmp_params, 0, sizeof(MYSQL_BIND) * rk_num);
        (void)memset(tmp_tm_data, 0, sizeof(MYSQL_TIME) * rk_num);

        for (int64_t i = 0; OB_SUCCESS == ret && i < rk_num; i++)
        {
          int32_t mysql_type = INT32_MAX;
          uint8_t num_decimals = 0;
          uint32_t length = 0;
          if (OB_SUCCESS == (ret = obmysql::ObMySQLUtil::get_mysql_type(
                  table_schema->get_rowkey_info().get_column(i)->type_,
                  (obmysql::EMySQLFieldType&)mysql_type,
                  num_decimals,
                  length)))
          {
            tmp_params[i].buffer_type = (enum_field_types)mysql_type;
          }
          else
          {
            TBSYS_LOG(WARN, "obmysql::ObMySQLUtil::get_mysql_type():  "
                "trans ob_type=%d to mysql_type=%d fail, ret=%d",
                table_schema->get_rowkey_info().get_column(i)->type_, mysql_type, ret);
            break;
          }
        }
      }

      if (OB_SUCCESS != ret || (NULL == tmp_params || NULL == tmp_tm_data))
      {
        destroy_mysql_params_(tmp_params, tmp_tm_data);
        tmp_params = NULL;
        tmp_tm_data = NULL;

        ret = OB_SUCCESS == ret ? OB_ERR_UNEXPECTED : ret;
      }
      else
      {
        params = tmp_params;
        tm_data = tmp_tm_data;
      }

      return ret;
    }

    void ObLogMysqlAdaptor::destroy_mysql_params_(MYSQL_BIND *params, MYSQL_TIME *tm_data)
    {
      if (NULL != tm_data)
      {
        allocator_.free(reinterpret_cast<char *>(tm_data));
        tm_data = NULL;
      }

      if (NULL != params)
      {
        allocator_.free(reinterpret_cast<char *>(params));
        params = NULL;
      }
    }

    int ObLogMysqlAdaptor::init_mysql_result_(MYSQL_BIND *&res_idx,
        IObLogColValue *&res_list,
        const TableSchemaContainer &tsc)
    {
      OB_ASSERT(NULL == res_idx && NULL == res_list && tsc.inited);

      int ret = OB_SUCCESS;
      MYSQL_BIND *tmp_res_idx = NULL;
      IObLogColValue *tmp_res_list = NULL;
      int32_t column_num = tsc.column_number;
      const ObColumnSchemaV2 *column_schema = tsc.column_schema_array;

      if (NULL == (tmp_res_idx = (MYSQL_BIND*)allocator_.alloc(sizeof(MYSQL_BIND) * column_num)))
      {
        TBSYS_LOG(WARN, "alloc res_idx fail, size=%d", column_num);
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      else
      {
        (void)memset(tmp_res_idx, 0, sizeof(MYSQL_BIND) * column_num);
        tmp_res_list = NULL;

        for (int32_t i = column_num - 1; i >= 0; i--)
        {
          IObLogColValue *cv = NULL;
          if (ObIntType == column_schema[i].get_type())
          {
            cv = (IObLogColValue*)allocator_.alloc(sizeof(IntColValue));
            new(cv) IntColValue();
          }
          else if (is_time_type(column_schema[i].get_type()))
          {
            cv = (IObLogColValue*)allocator_.alloc(sizeof(TimeColValue));
            new(cv) TimeColValue();
          }
          else if (ObVarcharType == column_schema[i].get_type())
          {
            int64_t column_size = column_schema[i].get_size();
            if (0 > column_size)
            {
              column_size = OB_MAX_VARCHAR_LENGTH;
            }
            cv = (IObLogColValue*)allocator_.alloc(sizeof(VarcharColValue) + column_size);
            new(cv) VarcharColValue(column_size);
          }
          else
          {
            TBSYS_LOG(ERROR, "unknown column type: %s,%d: not supported",
                ob_obj_type_str(column_schema[i].get_type()), column_schema[i].get_type());
            ret = OB_NOT_SUPPORTED;
            break;
          }

          int32_t mysql_type = INT32_MAX;
          uint8_t num_decimals = 0;
          uint32_t length = 0;
          if (NULL != cv
              && OB_SUCCESS == (ret = obmysql::ObMySQLUtil::get_mysql_type(column_schema[i].get_type(),
                  (obmysql::EMySQLFieldType&)mysql_type,
                  num_decimals,
                  length)))
          {
            tmp_res_idx[i].buffer_type = (enum_field_types)mysql_type;
            tmp_res_idx[i].buffer_length = cv->get_length();
            tmp_res_idx[i].buffer = cv->get_data_ptr();
            tmp_res_idx[i].length = cv->get_length_ptr();
            tmp_res_idx[i].is_null = cv->get_isnull_ptr();
            TBSYS_LOG(DEBUG, "init cv list, buffer_type=%d buffer_length=%ld buffer=%p length=%p is_null=%p",
                tmp_res_idx[i].buffer_type,
                tmp_res_idx[i].buffer_length,
                tmp_res_idx[i].buffer,
                tmp_res_idx[i].length,
                tmp_res_idx[i].is_null);

            cv->set_mysql_type(mysql_type);
            cv->set_next(tmp_res_list);
            tmp_res_list = cv;
          }
          else
          {
            TBSYS_LOG(WARN, "obmysql::ObMySQLUtil::get_mysql_type():  "
                "trans ob_type=%d to mysql_type=%d fail, ret=%d",
                column_schema[i].get_type(), mysql_type, ret);
            break;
          }
        }
      }

      if (OB_SUCCESS != ret || NULL == tmp_res_idx || NULL == tmp_res_list)
      {
        destroy_mysql_result_(tmp_res_idx, tmp_res_list);
        tmp_res_idx = NULL;
        tmp_res_list = NULL;

        ret = OB_SUCCESS == ret ? OB_ERR_UNEXPECTED : ret;
      }
      else
      {
        res_idx = tmp_res_idx;
        res_list = tmp_res_list;
      }

      return ret;
    }

    void ObLogMysqlAdaptor::destroy_mysql_result_(MYSQL_BIND *res_idx, IObLogColValue *res_list)
    {
      IObLogColValue *cvs = res_list;

      while (NULL != cvs)
      {
        IObLogColValue *cv = cvs;
        cvs = cvs->get_next();

        cv->~IObLogColValue();
        allocator_.free(reinterpret_cast<char *>(cv));
        cv = NULL;
      }

      if (NULL != res_idx)
      {
        allocator_.free(reinterpret_cast<char *>(res_idx));
        res_idx = NULL;
      }
    }

    void ObLogMysqlAdaptor::destroy_tc_info_map_()
    {
      TCInfoMap::iterator tc_map_iter;
      for (tc_map_iter = tc_info_map_.begin(); tc_map_iter != tc_info_map_.end(); tc_map_iter++)
      {
        TCInfo *info_iter = tc_map_iter->second;
        if (NULL != info_iter)
        {
          destruct_tc_info_(info_iter);
          info_iter = NULL;
        }

        tc_map_iter->second = NULL;
      }

      tc_info_map_.destroy();
    }
  }
}

