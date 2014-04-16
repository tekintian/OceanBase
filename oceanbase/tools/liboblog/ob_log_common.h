////===================================================================
 //
 // ob_log_commonc.h liboblog / Oceanbase
 //
 // Copyright (C) 2013 Alipay.com, Inc.
 //
 // Created on 2013-06-07 by Fusheng Han (yanran.hfs@alipay.com) 
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

#ifndef  OCEANBASE_LIBOBLOG_COMMON_H_
#define  OCEANBASE_LIBOBLOG_COMMON_H_

#include "common/hash/ob_hashmap.h"   // ObHashMap

#define LOG_AND_ERR(level, format_str, ...) \
  { \
    TBSYS_LOG(level, format_str, ##__VA_ARGS__); \
    bool is_err = false; \
    const char * prefix_str = NULL; \
    if (TBSYS_LOG_LEVEL_##level == TBSYS_LOG_LEVEL_ERROR) { \
      prefix_str = "\033[1;31mERROR\033[0m : "; is_err = true; } \
    else if (TBSYS_LOG_LEVEL_##level == TBSYS_LOG_LEVEL_WARN) { \
      prefix_str = "\033[1;31mWARN\033[0m  : "; is_err = true; } \
    else prefix_str = "INFO  : "; \
    if (is_err) fprintf(stderr, "%s" format_str "\n", prefix_str, ##__VA_ARGS__); \
    else fprintf(stdout, "%s" format_str "\n", prefix_str, ##__VA_ARGS__); \
  }

// DRC data format protocol
#define COLUMN_VALUE_CHANGED_MARK    ""
#define COLUMN_VALUE_UNCHANGED_MARK  NULL

namespace oceanbase
{
  namespace liboblog
  {
    static const int64_t OB_LOG_INVALID_TIMESTAMP    = -1;
    static const uint64_t OB_LOG_INVALID_CHECKPOINT  = 0;

    static const int64_t PERF_PRINT_INTERVAL         = 10L * 1000000L;  // interval time to print perf information
    static const int64_t HEARTBEAT_INTERVAL          = 5L * 1000000L;

    static const int32_t MAX_TABLE_NUM                = 128;    // max table number which can be selected
    static const uint64_t MAX_OBSERVER_NUM            = 1024LU; // max number of IObLog instances
    static const uint64_t MAX_DB_PARTITION_NUM        = 1024LU; // max number of DataBase Partition number
    static const uint64_t MAX_TB_PARTITION_NUM        = 1024LU; // max number of Table Partition number
    static const int32_t MAX_LOG_FORMATER_THREAD_NUM  = 100;

    static const int32_t MAX_SCHEMA_ERROR_RETRY_TIMES = 100;     // max retry times when encountering into schema error

    struct string_equal_to
    {
      bool operator()(const char *a, const char *b) const
      {
        return 0 == strcmp(a, b);
      }
    };

    template <class value_type>
    class ObLogStringMap : public common::hash::ObHashMap<const char *, value_type,
                                                          common::hash::ReadWriteDefendMode,
                                                          common::hash::hash_func<const char *>,
                                                          string_equal_to>
    {
    };
  }
}

#endif //OCEANBASE_LIBOBLOG_COMMON_H_
