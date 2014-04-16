////===================================================================
 //
 // ob_log_config.h liboblog / Oceanbase
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

#ifndef  OCEANBASE_LIBOBLOG_CONFIG_H_
#define  OCEANBASE_LIBOBLOG_CONFIG_H_

#include "tbsys.h"
#include "common/ob_define.h"
#include <map>                        // map

namespace oceanbase
{
  namespace liboblog
  {
    class ObLogConfig
    {
      typedef std::map<std::string, std::string> ConfigMap;
      typedef std::pair<std::string, std::string> ConfigMapPair;

      public:
        ObLogConfig();
        ~ObLogConfig();
      public:
        int init(const char *config_content);
        int init(const std::map<std::string, std::string>& configs);
        void destroy();
      public:
        //const char *get_ups_addr() const;
        //int get_ups_port() const;
        //const char *get_rs_addr() const;
        //int get_rs_port() const;
        const int32_t get_obsql_min_conn() const;
        const int32_t get_obsql_max_conn() const;
        const char *get_mysql_addr() const;
        int get_mysql_port() const;
        const char *get_mysql_user() const;
        const char *get_mysql_password() const;
        const char *get_tb_select() const;
        const char *get_lua_conf() const;
        const char *get_dbn_format(const char *db_name) const;
        const char *get_tbn_format(const char *tb_name) const;
        const char *get_db_partition_lua(const char *db_name) const;
        const char *get_tb_partition_lua(const char *tb_name) const;
        const char *get_config_fpath() const;
        int get_router_thread_num() const;
        int get_query_back() const;
        const char *get_query_back_read_consistency() const;
        bool get_skip_dirty_data() const;
      public:
        template <class Callback, class Arg>
        static int parse_tb_select(const char *tb_select, Callback &callback, Arg *arg);
        int operator ()(const char *tb_name, void *arg);
      private:
        const char *format_url_(const char *url);
        int load_config_();

        /// @brief Get string configuration value
        /// @param config_section configuration section
        /// @param config_name configuration name
        /// @return target configuration value
        const char *get_string_config_(const char *config_section, const char *config_name) const;

        /// @brief Get int configuration value
        /// @param config_section configuration section
        /// @param config_name configuration name
        /// @param invalid_value invalid value
        /// @return target configuration value
        int get_int_config_(const char *config_section, const char *config_name, int invalid_value) const;

        /// @brief Print map configuration content
        /// @param map configuration map
        void print_map_config_content_(const ConfigMap *map);

        /// @brief Start Logger
        void start_logger_();

        /// @brief Parse cluster URL
        /// @param cluster_url cluster URL
        /// @retval OB_SUCCESS on success
        /// @retval ! OB_SUCCESS on fail
        int parse_cluster_url_(const char *cluster_url);
      private:
        bool inited_;
        bool is_map_config_;
        ConfigMap map_config_;           ///< map container for configuration
        tbsys::CConfig tbsys_config_;    ///< tbsys container for configuration
      private:
        static const int64_t FPATH_NAME_LENGTH = 1024;
    };

    template <class Callback, class Arg>
    int ObLogConfig::parse_tb_select(const char *tb_select_cstr, Callback &callback, Arg *arg)
    {
      TBSYS_LOG(INFO, "tb_select=[%s]", tb_select_cstr);
      int ret = common::OB_SUCCESS;
      std::string tb_select = tb_select_cstr;
      tb_select.append(",");
      uint64_t pos = 0;
      while (common::OB_SUCCESS == ret)
      {
        const uint64_t next_pos_1 = tb_select.find(',', pos);
        const uint64_t next_pos_2 = tb_select.find(';', pos);

        // Choose the valid and minor position
        uint64_t next_pos = next_pos_1;
        if (std::string::npos != next_pos_2)
        {
          if (std::string::npos == next_pos || next_pos > next_pos_2)
          {
            next_pos = next_pos_2;
          }
        }

        if (std::string::npos == next_pos)
        {
          break;
        }
        if (pos == next_pos)
        {
          pos = next_pos + 1;
          continue;
        }
        std::string tb_name = tb_select.substr(pos, next_pos - pos);

        ret = callback(tb_name.c_str(), arg);

        pos = next_pos + 1;
        if (tb_select.length() <= pos)
        {
          break;
        }
      }
      return ret;
    }
  }
}

#endif //OCEANBASE_LIBOBLOG_CONFIG_H_

