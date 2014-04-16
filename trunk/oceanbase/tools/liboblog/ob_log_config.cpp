////===================================================================
 //
 // ob_log_config.cpp liboblog / Oceanbase
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

#include <curl/curl.h>
#include "tbsys.h"
#include "ob_log_config.h"
#include "ob_log_common.h"          // MAX_LOG_FORMATER_THREAD_NUM

#define LOG_CONFIG_SECTION    "liboblog"
#define LOG_CONFIG_SECTION_ENTIRE "[liboblog]\n"

#define LOG_CONFIG_OB_URL     "cluster_url"
#define LOG_CONFIG_OB_USER    "cluster_user"
#define LOG_CONFIG_OB_PASSWORD  "cluster_password"
#define LOG_CONFIG_OB_ADDR    "clusterAddress"

#define LOG_CONFIG_UPS_ADDR   "ups_addr"
#define LOG_CONFIG_UPS_PORT   "ups_port"
#define LOG_CONFIG_RS_ADDR    "rs_addr"
#define LOG_CONFIG_RS_PORT    "rs_port"
#define LOG_CONFIG_ROUTER_THREAD_NUM "router_thread_num"
#define LOG_CONFIG_LOG_FPATH  "log_fpath"
#define LOG_CONFIG_LOG_LEVEL  "log_level"
#define LOG_CONFIG_QUERY_BACK "query_back"
#define LOG_CONFIG_QUERY_BACK_READ_CONSISTENCY "query_back_read_consistency"
#define LOG_CONFIG_SKIP_DIRTY_DATA  "skip_dirty_data"

#define LOG_PARTITION_SECTION   "partition"
#define LOG_PARTITION_TB_SELECT "tb_select"
#define LOG_PARTITION_LUA_CONF  "lua_conf"
#define LOG_PARTITION_DBN_FORMAT_SUFFIX  "_dbn_format"
#define LOG_PARTITION_TBN_FORMAT_SUFFIX  "_tbn_format"
#define LOG_PARTITION_DB_PARTITION_LUA_FORMAT_SUFFIX  "_db_partition_lua"
#define LOG_PARTITION_TB_PARTITION_LUA_FORMAT_SUFFIX  "_tb_partition_lua"

#define LOG_OBSQL_SECTION  "libobsql"
#define LOG_OBSQL_MIN_CONN  "min_conn"
#define LOG_OBSQL_MAX_CONN  "max_conn"

#define DEFAULT_LOG_FPATH     "./log/liboblog.log"
#define DEFAULT_LOG_LEVEL     "INFO"

const char* svn_version();
const char* build_date();
const char* build_time();
const char* build_flags();

namespace oceanbase
{
  using namespace common;
  namespace liboblog
  {
    ObLogConfig::ObLogConfig() : inited_(false)
    {
    }

    ObLogConfig::~ObLogConfig()
    {
      destroy();
      TBSYS_LOG(INFO, "====================liboblog end====================");
    }

#define STR_CONFIG_CHECK(section, name) \
    if (OB_SUCCESS == ret)\
    { \
      const char *v = NULL; \
      if (NULL == (v = get_string_config_(section, name))) \
      { \
        LOG_AND_ERR(WARN, "string config [%s] not exist. Please check it again", name); \
        ret = OB_ENTRY_NOT_EXIST; \
      } \
      else if ('\0' == v[0]) \
      {\
        LOG_AND_ERR(WARN, "string config [%s] is empty. Please check it again", name); \
        ret = OB_ENTRY_NOT_EXIST; \
      } \
      else \
      { \
        TBSYS_LOG(INFO, "load string config succ %s=%s", name, v); \
      } \
    }

#define INT_CONFIG_CHECK(section, name, min, max) \
    if (OB_SUCCESS == ret) \
    { \
      int v = get_int_config_(section, name, (min) - 1);\
      if (v <= (min) - 1) \
      { \
        LOG_AND_ERR(WARN, "invalid int config [%s] which should be in [%d, %d]. Please check it again", \
            name, (min), (max)); \
        ret = OB_ENTRY_NOT_EXIST; \
      } \
      else \
      { \
        TBSYS_LOG(INFO, "load int config succ %s=%d", name, v); \
      } \
    }

#define BOOL_CONFIG_CHECK(section, name) \
    if (OB_SUCCESS == ret) \
    { \
      int v = get_int_config_(section, name, -1); \
      if (0 != v && 1 != v) \
      { \
        LOG_AND_ERR(WARN, "invalid bool config [%s] which should be 0 or 1. Please check it again", name); \
        ret = OB_ENTRY_NOT_EXIST; \
      } \
      else \
      { \
        TBSYS_LOG(INFO, "load bool config succ %s=%d", name, v); \
      } \
    }

    int ObLogConfig::init(const char * config_file)
    {
      int ret = OB_SUCCESS;
      if (inited_)
      {
        ret = OB_INIT_TWICE;
      }
      else if (NULL == config_file)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else if (0 != tbsys_config_.load(config_file))
      {
        TBSYS_LOG(WARN, "load config fail config_file=%s", config_file);
        ret = OB_ERR_UNEXPECTED;
      }
      else
      {
        is_map_config_ = false;

        // Start Logger
        // NOTE: is_map_config_ must be initialized first
        start_logger_();

        // Load configuration
        if (OB_SUCCESS != (ret = load_config_()))
        {
          TBSYS_LOG(WARN, "load config fail, ret=%d config_file=%s", ret, config_file);
        }
        else
        {
          TBSYS_LOG(INFO, "===================== CONFIG BEGIN =====================");
          TBSYS_LOG(INFO, "config_file='%s'", config_file);
          TBSYS_LOG(INFO, "===================== CONFIG END =====================");

          inited_ = true;
        }
      }

      if (OB_SUCCESS != ret)
      {
        destroy();
      }
      return ret;
    }

    int ObLogConfig::init(const std::map<std::string, std::string>& configs)
    {
      int ret = OB_SUCCESS;
      if (inited_)
      {
        ret = OB_INIT_TWICE;
      }
      else
      {
        map_config_ = configs;
        is_map_config_ = true;

        // Start Logger
        // NOTE: is_map_config_ must be initialized first
        start_logger_();

        // NOTE: We must start logger first, then print config immediately
        print_map_config_content_(&map_config_);

        // Load configuration
        if (OB_SUCCESS != (ret = load_config_()))
        {
          TBSYS_LOG(WARN, "load config fail, ret=%d config map=%p", ret, &configs);
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

    void ObLogConfig::destroy()
    {
      inited_ = false;
      map_config_.clear();
      is_map_config_ = true;
    }

    const char *ObLogConfig::format_url_(const char *url)
    {
      static const int64_t BUF_SIZE = 1024;
      static __thread char BUFFER[BUF_SIZE];
      const char *ret = NULL;

      if (NULL == url)
      {
        TBSYS_LOG(ERROR, "cluster_url is NULL");
      }
      else
      {
        std::string str(url);

        if (0 < str.length())
        {
          if ('\"' == str[0])
          {
            str.erase(0, 1);
          }

          if ('\"' == str[str.length() - 1])
          {
            str.erase(str.length() - 1, 1);
          }

          snprintf(BUFFER, BUF_SIZE, "%s", str.c_str());
          TBSYS_LOG(INFO, "format url from [%s] to [%s]", url, BUFFER);

          ret = BUFFER;
        }
        else
        {
          TBSYS_LOG(WARN, "cluster URL is empty");
        }
      }

      return ret;
    }

    int ObLogConfig::load_config_()
    {
      int ret = OB_SUCCESS;

      STR_CONFIG_CHECK(LOG_CONFIG_SECTION, LOG_CONFIG_OB_URL);
      if (OB_SUCCESS == ret)
      {
        const char *cluster_url = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_OB_URL);
        if (OB_SUCCESS != (ret = parse_cluster_url_(cluster_url)))
        {
          TBSYS_LOG(WARN, "parse_cluster_url_ fail, cluster_url=%s, ret=%d", cluster_url, ret);
        }
      }

      INT_CONFIG_CHECK(LOG_CONFIG_SECTION, LOG_CONFIG_ROUTER_THREAD_NUM, 1, MAX_LOG_FORMATER_THREAD_NUM);
      BOOL_CONFIG_CHECK(LOG_CONFIG_SECTION, LOG_CONFIG_SKIP_DIRTY_DATA);

      BOOL_CONFIG_CHECK(LOG_CONFIG_SECTION, LOG_CONFIG_QUERY_BACK);
      STR_CONFIG_CHECK(LOG_CONFIG_SECTION, LOG_CONFIG_OB_USER);
      if (OB_SUCCESS == ret)
      {
        // Check PASSWORD
        // NOTE: PASSWORD can be empty
        if (NULL == (get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_OB_PASSWORD)))
        {
          LOG_AND_ERR(WARN, "string config [%s] not exist. Please check it again", LOG_CONFIG_OB_PASSWORD);
          ret = OB_ENTRY_NOT_EXIST;
        }
      }

      STR_CONFIG_CHECK(LOG_CONFIG_SECTION, LOG_CONFIG_QUERY_BACK_READ_CONSISTENCY);

      if (OB_SUCCESS == ret)
      {
        // Check query back read consistency
        const char *rc = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_QUERY_BACK_READ_CONSISTENCY);
        if (0 != strcasecmp(rc, "strong") && 0 != strcasecmp(rc, "weak"))
        {
          LOG_AND_ERR(WARN, "invalid config [%s=%s] which should be \"strong\" or \"weak\" "
              "ignoring the case of characters",
              LOG_CONFIG_QUERY_BACK_READ_CONSISTENCY, rc);

          ret = OB_INVALID_ARGUMENT;
        }
      }

      STR_CONFIG_CHECK(LOG_PARTITION_SECTION, LOG_PARTITION_TB_SELECT);
      if (OB_SUCCESS == ret)
      {
        const char *tb_select = get_string_config_(LOG_PARTITION_SECTION, LOG_PARTITION_TB_SELECT);
        ret = parse_tb_select(tb_select, *this, (void *)NULL);
      }

      return ret;
    }

    const char *ObLogConfig::get_string_config_(const char *config_section, const char *config_name) const
    {
      OB_ASSERT(NULL != config_section && NULL != config_name);

      const char *ret = NULL;

      if (OB_LIKELY(is_map_config_))
      {
        ConfigMap::const_iterator iter = map_config_.find(config_name);
        if (map_config_.end() != iter)
        {
          ret = iter->second.c_str();
        }
      }
      else
      {
        ret = const_cast<ObLogConfig&>(*this).tbsys_config_.getString(config_section, config_name);
      }

      return ret;
    }

    int ObLogConfig::get_int_config_(const char *config_section, const char *config_name, int invalid_value) const
    {
      OB_ASSERT(NULL != config_section && NULL != config_name);
      int ret = invalid_value;

      if (OB_LIKELY(is_map_config_))
      {
        ConfigMap::const_iterator iter = map_config_.find(config_name);
        if (map_config_.end() != iter)
        {
          ret = tbsys::CStringUtil::strToInt(iter->second.c_str(), invalid_value);
        }
      }
      else
      {
        ret = const_cast<ObLogConfig&>(*this).tbsys_config_.getInt(config_section, config_name, invalid_value);
      }

      return ret;
    }

    void ObLogConfig::print_map_config_content_(const ConfigMap *map)
    {
      OB_ASSERT(NULL != map);

      TBSYS_LOG(INFO, "===================== LIBOBLOG CONFIG BEGIN =====================");
      ConfigMap::const_iterator iter = map->begin();
      while (iter != map->end())
      {
        TBSYS_LOG(INFO, "%s=%s", iter->first.c_str(), iter->second.c_str());
        iter++;
      }
      TBSYS_LOG(INFO, "===================== LIBOBLOG CONFIG END ======================");
    }

    void ObLogConfig::start_logger_()
    {
      const char *log_fpath = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_LOG_FPATH);
      log_fpath = (NULL != log_fpath ? log_fpath : DEFAULT_LOG_FPATH);

      char *p = strrchr(const_cast<char*>(log_fpath), '/');
      if (NULL != p)
      {
        char dir_buffer[OB_MAX_FILE_NAME_LENGTH];
        snprintf(dir_buffer, OB_MAX_FILE_NAME_LENGTH, "%.*s",
            (int)(p - log_fpath), log_fpath);
        tbsys::CFileUtil::mkdirs(dir_buffer);
      }
      TBSYS_LOGGER.setFileName(log_fpath, true);

      const char *log_level = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_LOG_LEVEL);
      log_level = (NULL != log_level) ? log_level : DEFAULT_LOG_LEVEL;
      TBSYS_LOGGER.setLogLevel(log_level);

      TBSYS_LOG(INFO, "logger=%p", &(TBSYS_LOGGER));
      TBSYS_LOG(INFO, "====================liboblog start====================");
      TBSYS_LOG(INFO, "SVN_VERSION: %s", svn_version());
      TBSYS_LOG(INFO, "BUILD_TIME: %s %s", build_date(), build_time());
      TBSYS_LOG(INFO, "BUILD_FLAGS: %s", build_flags());
    }

    int ObLogConfig::parse_cluster_url_(const char *cluster_url)
    {
      OB_ASSERT(NULL != cluster_url);

      int ret = OB_SUCCESS;
      CURL *curl = NULL;
      CURLcode curl_ret = CURLE_OK;

      const int64_t BUFFER_SIZE = 128;
      char config_file_buffer[BUFFER_SIZE];
      FILE *config_file_fd = NULL;

      if (NULL == (cluster_url = format_url_(cluster_url)))
      {
        TBSYS_LOG(WARN, "str config [%s] is invalid", LOG_CONFIG_OB_URL);
        ret = OB_ENTRY_NOT_EXIST;
      }
      else if (BUFFER_SIZE <= snprintf(config_file_buffer, BUFFER_SIZE, "/tmp/liboblog.curl.%d", getpid()))
      {
        TBSYS_LOG(WARN, "assemble config_file_buffer fail, buffer=./liboblog.curl.%d", getpid());
        ret = OB_BUF_NOT_ENOUGH;
      }
      else if (NULL == (config_file_fd = fopen(config_file_buffer, "w+")))
      {
        TBSYS_LOG(WARN, "fopen config_file fail, file=%s errno=%u", config_file_buffer, errno);
        ret = OB_ERR_UNEXPECTED;
      }
      else if (sizeof(LOG_CONFIG_SECTION_ENTIRE) != fwrite(LOG_CONFIG_SECTION_ENTIRE,
            1,
            sizeof(LOG_CONFIG_SECTION_ENTIRE),
            config_file_fd))
      {
        TBSYS_LOG(WARN, "write session string to config file fail, errno=%u", errno);
        ret = OB_ERR_UNEXPECTED;
      }
      else if (NULL == (curl = curl_easy_init()))
      {
        TBSYS_LOG(WARN, "curl_easy_init fail");
        ret = OB_ERR_UNEXPECTED;
      }
      else if (CURLE_OK != (curl_ret = curl_easy_setopt(curl, CURLOPT_URL, cluster_url)))
      {
        TBSYS_LOG(WARN, "curl_easy_setopt set CURLOPT_URL fail, error=%s", curl_easy_strerror(curl_ret));
        ret = OB_ERR_UNEXPECTED;
      }
      else if (CURLE_OK != (curl_ret = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L)))
      {
        TBSYS_LOG(WARN, "curl_easy_setopt set CURLOPT_FOLLOWLOCATION fail, error=%s", curl_easy_strerror(curl_ret));
        ret = OB_ERR_UNEXPECTED;
      }
      else if (CURLE_OK != (curl_ret = curl_easy_setopt(curl, CURLOPT_WRITEDATA, config_file_fd)))
      {
        TBSYS_LOG(WARN, "curl_easy_setopt set CURLOPT_FOLLOWLOCATION fail, error=%s", curl_easy_strerror(curl_ret));
        ret = OB_ERR_UNEXPECTED;
      }
      else if (CURLE_OK != (curl_ret = curl_easy_perform(curl)))
      {
        TBSYS_LOG(WARN, "curl_easy_perform fail, error=%s url=[%s]", curl_easy_strerror(curl_ret), cluster_url);
        ret = OB_ERR_UNEXPECTED;
      }
      else
      {
        fflush(config_file_fd);
        // NOTE: Load URL config into tbsys CConfig
        if (0 != tbsys_config_.load(config_file_buffer))
        {
          TBSYS_LOG(WARN, "load config fail fpath=%s", config_file_buffer);
          ret = OB_ERR_UNEXPECTED;
        }
        else
        {
          const char *cluster_address = tbsys_config_.getString(LOG_CONFIG_SECTION, LOG_CONFIG_OB_ADDR);
          if (NULL == cluster_address)
          {
            TBSYS_LOG(ERROR, "clusterAddress is not provided in cluster URL: %s, invalid argument", cluster_url);
            ret = OB_INVALID_ARGUMENT;
          }
          else if (is_map_config_)
          {
            // Add clusterAddress configuration into map_config_
            if (! (map_config_.insert(ConfigMapPair(LOG_CONFIG_OB_ADDR, cluster_address))).second)
            {
              TBSYS_LOG(ERROR, "\"%s\" exists both in configuration and cluster URL(%s). Please check it again",
                  LOG_CONFIG_OB_ADDR, cluster_url);
              ret = OB_INVALID_ARGUMENT;
            }
            else
            {
              TBSYS_LOG(INFO, "load string config from cluster_url succ, %s=%s",
                  LOG_CONFIG_OB_ADDR, cluster_address);
            }
          }
        }
      }

      if (NULL != curl)
      {
        curl_easy_cleanup(curl);
        curl = NULL;
      }

      if (NULL != config_file_fd)
      {
        remove(config_file_buffer);
        //fclose(config_file_fd);
        config_file_fd = NULL;
      }

      return ret;
    }

    int ObLogConfig::operator ()(const char *tb_name, void *arg)
    {
      UNUSED(arg);

      int ret = OB_SUCCESS;
      TBSYS_LOG(INFO, "parse from tb_name=[%s]", tb_name);

      std::string db_name_format = tb_name;
      db_name_format.append(LOG_PARTITION_DBN_FORMAT_SUFFIX);
      STR_CONFIG_CHECK(LOG_PARTITION_SECTION, db_name_format.c_str());
      if (OB_SUCCESS != ret)
      {
        LOG_AND_ERR(ERROR, "`%s' has no db partition format `%s' in configuration",
            tb_name, db_name_format.c_str());
      }
      else
      {
        std::string tb_name_format = tb_name;
        tb_name_format.append(LOG_PARTITION_TBN_FORMAT_SUFFIX);
        STR_CONFIG_CHECK(LOG_PARTITION_SECTION, tb_name_format.c_str());
        if (OB_SUCCESS != ret)
        {
          LOG_AND_ERR(ERROR, "`%s' has no tb partition format `%s' in configuration",
              tb_name, tb_name_format.c_str());
        }
      }

      // Check database partition LUA configuration
      if (OB_SUCCESS == ret)
      {
        std::string db_lua_format_name = tb_name;
        db_lua_format_name.append(LOG_PARTITION_DB_PARTITION_LUA_FORMAT_SUFFIX);
        const char *db_lua_format_value = get_string_config_(LOG_PARTITION_SECTION, db_lua_format_name.c_str());
        if (NULL != db_lua_format_value && '\0' == db_lua_format_value[0])
        {
          LOG_AND_ERR(ERROR, "string config [%s] is empty. Please check it again", db_lua_format_name.c_str());
          ret = OB_ENTRY_NOT_EXIST;
        }
      }

      // Check table partition LUA configuration
      if (OB_SUCCESS == ret)
      {
        std::string tb_lua_format_name = tb_name;
        tb_lua_format_name.append(LOG_PARTITION_TB_PARTITION_LUA_FORMAT_SUFFIX);
        const char *tb_lua_format_value = get_string_config_(LOG_PARTITION_SECTION, tb_lua_format_name.c_str());
        if (NULL != tb_lua_format_value && '\0' == tb_lua_format_value[0])
        {
          LOG_AND_ERR(ERROR, "string config [%s] is empty. Please check it again", tb_lua_format_name.c_str());
          ret = OB_ENTRY_NOT_EXIST;
        }
      }

      return ret;
    }

    /*
    const char *ObLogConfig::get_ups_addr() const
    {
      const char *ret = NULL;
      if (inited_)
      {
        ret = const_cast<ObLogConfig&>(*this).config_.getString(LOG_CONFIG_SECTION, LOG_CONFIG_UPS_ADDR);
      }
      return ret;
    }

    int ObLogConfig::get_ups_port() const
    {
      int ret = 0;
      if (inited_)
      {
        ret = const_cast<ObLogConfig&>(*this).config_.getInt(LOG_CONFIG_SECTION, LOG_CONFIG_UPS_PORT);
      }
      return ret;
    }

    const char *ObLogConfig::get_rs_addr() const
    {
      const char *ret = NULL;
      if (inited_)
      {
        ret = const_cast<ObLogConfig&>(*this).config_.getString(LOG_CONFIG_SECTION, LOG_CONFIG_RS_ADDR);
      }
      return ret;
    }

    int ObLogConfig::get_rs_port() const
    {
      int ret = 0;
      if (inited_)
      {
        ret = const_cast<ObLogConfig&>(*this).config_.getInt(LOG_CONFIG_SECTION, LOG_CONFIG_RS_PORT);
      }
      return ret;
    }
    */

    const char *ObLogConfig::get_mysql_addr() const
    {
      static const int64_t BUFFER_SIZE = 1024;
      static __thread char buffer[BUFFER_SIZE];
      const char *ret = NULL;
      if (inited_)
      {
        const char *cluster_address = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_OB_ADDR);
        snprintf(buffer, BUFFER_SIZE, "%s", cluster_address);
        char *delim = strchr(buffer, ':');
        if (NULL != delim)
        {
          *delim = '\0';
          if (0 < strlen(buffer))
          {
            ret = buffer;
          }
        }
      }

      return ret;
    }

    int ObLogConfig::get_mysql_port() const
    {
      static const int64_t BUFFER_SIZE = 1024;
      static __thread char buffer[BUFFER_SIZE];
      int ret = 0;
      if (inited_)
      {
        const char *cluster_address = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_OB_ADDR);
        snprintf(buffer, BUFFER_SIZE, "%s", cluster_address);
        char *delim = strchr(buffer, ':');
        if (NULL != delim)
        {
          delim++;
          ret = atoi(delim);
        }
      }
      return ret;
    }

    const int32_t ObLogConfig::get_obsql_min_conn() const
    {
      int32_t ret = 0;
      if (inited_)
      {
        ret = get_int_config_(LOG_OBSQL_SECTION, LOG_OBSQL_MIN_CONN, 0);
      }
      return ret;
    }

    const int32_t ObLogConfig::get_obsql_max_conn() const
    {
      int32_t ret = 0;
      if (inited_)
      {
        ret = get_int_config_(LOG_OBSQL_SECTION, LOG_OBSQL_MAX_CONN, 0);
      }
      return ret;
    }

    const char *ObLogConfig::get_mysql_user() const
    {
      const char *ret = NULL;
      if (inited_)
      {
        ret = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_OB_USER);
      }
      return ret;
    }

    const char *ObLogConfig::get_mysql_password() const
    {
      const char *ret = NULL;
      if (inited_)
      {
        ret = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_OB_PASSWORD);
      }
      return ret;
    }

    const char *ObLogConfig::get_tb_select() const
    {
      const char *ret = NULL;
      if (inited_)
      {
        ret = get_string_config_(LOG_PARTITION_SECTION, LOG_PARTITION_TB_SELECT);
      }
      return ret;
    }

    int ObLogConfig::get_router_thread_num() const
    {
      int ret = 0;
      if (inited_)
      {
        ret = get_int_config_(LOG_CONFIG_SECTION, LOG_CONFIG_ROUTER_THREAD_NUM, 0);
      }
      return ret;
    }

    int ObLogConfig::get_query_back() const
    {
      int ret = 1;
      if (inited_)
      {
        ret = get_int_config_(LOG_CONFIG_SECTION, LOG_CONFIG_QUERY_BACK, -1);
      }
      return ret;
    }

    bool ObLogConfig::get_skip_dirty_data() const
    {
      bool ret = false;
      if (inited_)
      {
        ret = (0 != get_int_config_(LOG_CONFIG_SECTION, LOG_CONFIG_SKIP_DIRTY_DATA, -1));
      }
      return ret;
    }

    const char *ObLogConfig::get_query_back_read_consistency() const
    {
      const char *ret = NULL;
      if (inited_)
      {
        ret = get_string_config_(LOG_CONFIG_SECTION, LOG_CONFIG_QUERY_BACK_READ_CONSISTENCY);
      }
      return ret;
    }

    const char *ObLogConfig::get_lua_conf() const
    {
      const char *ret = NULL;
      if (inited_)
      {
        ret = get_string_config_(LOG_PARTITION_SECTION, LOG_PARTITION_LUA_CONF);
      }
      return ret;
    }

    const char *ObLogConfig::get_dbn_format(const char *db_name) const
    {
      const char *ret = NULL;
      if (inited_)
      {
        std::string db_name_format = db_name;
        db_name_format.append(LOG_PARTITION_DBN_FORMAT_SUFFIX);
        ret = get_string_config_(LOG_PARTITION_SECTION, db_name_format.c_str());
      }
      return ret;
    }

    const char *ObLogConfig::get_tbn_format(const char *tb_name) const
    {
      const char *ret = NULL;
      if (inited_)
      {
        std::string tb_name_format = tb_name;
        tb_name_format.append(LOG_PARTITION_TBN_FORMAT_SUFFIX);
        ret = get_string_config_(LOG_PARTITION_SECTION, tb_name_format.c_str());
      }
      return ret;
    }

    const char *ObLogConfig::get_db_partition_lua(const char *db_name) const
    {
      const char *ret = NULL;
      if (inited_)
      {
        std::string db_partition_lua_format = db_name;
        db_partition_lua_format.append(LOG_PARTITION_DB_PARTITION_LUA_FORMAT_SUFFIX);
        ret = get_string_config_(LOG_PARTITION_SECTION, db_partition_lua_format.c_str());
      }
      return ret;
    }

    const char *ObLogConfig::get_tb_partition_lua(const char *tb_name) const
    {
      const char *ret = NULL;
      if (inited_)
      {
        std::string tb_partition_lua_format = tb_name;
        tb_partition_lua_format.append(LOG_PARTITION_TB_PARTITION_LUA_FORMAT_SUFFIX);
        ret = get_string_config_(LOG_PARTITION_SECTION, tb_partition_lua_format.c_str());
      }

      return ret;
    }

    const char *ObLogConfig::get_config_fpath() const
    {
      return "";
    }
  }
}

