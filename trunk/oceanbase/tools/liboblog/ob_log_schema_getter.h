////===================================================================
 //
 // ob_log_meta_manager.h liboblog / Oceanbase
 //
 // Copyright (C) 2013 Alipay.com, Inc.
 //
 // Created on 2014-01-17 by Xiuming (wanhong.wwh@alibaba-inc.com)
 //
 // -------------------------------------------------------------------
 //
 // Description
 //   Schema Getter Header File
 //
 // -------------------------------------------------------------------
 //
 // Change Log
 //
////====================================================================

#ifndef  OCEANBASE_LIBOBLOG_SCHEMA_GETTER_H_
#define  OCEANBASE_LIBOBLOG_SCHEMA_GETTER_H_

#include "common/ob_define.h"
#include "common/ob_schema.h"       // ObSchemaManagerV2
#include "common/hash/ob_hashmap.h" // ObHashMap
#include "common/ob_bit_set.h"      // ObBitSet
#include "ob_log_common.h"          // MAX_TB_PARTITION_NUM
#include "common/ob_spin_lock.h"    // ObSpinLock
#include "common/ob_fifo_allocator.h" // FIFOAllocator

namespace oceanbase
{
  namespace liboblog
  {
    class ObLogSchema : public common::ObSchemaManagerV2
    {
      public:
        ObLogSchema() : ref_(0),
                        timestamp_(OB_LOG_INVALID_TIMESTAMP)
        {
          timestamp_ = tbsys::CTimeUtil::getTime();
        }

      public:
        void ref() {ATOMIC_ADD(&ref_, 1);};
        int64_t deref() {return ATOMIC_ADD(&ref_, -1);};

        int64_t get_timestamp() const { return timestamp_; }
        void set_timestamp(int64_t timestamp) { timestamp_ = timestamp; }

        bool is_newer_than(const ObLogSchema &schema) const
        {
          int bret = false;
          int64_t cur_version = get_version();
          int64_t target_version = schema.get_version();
          int64_t target_timestamp = schema.get_timestamp();

          if (cur_version == target_version)
          {
            bret = timestamp_ > target_timestamp;
          }
          else
          {
            bret = cur_version > target_version;
          }

          return bret;
        }

      private:
        volatile int64_t ref_;
        volatile int64_t timestamp_;
    };

    ////////////////////////////////////////////////////////////////////////////////////////////////////

    class IObLogServerSelector;
    class IObLogRpcStub;
    class ObLogConfig;
    class IObLogSchemaGetter
    {
      public:
        virtual ~IObLogSchemaGetter() {};
      public:
        virtual int init(IObLogServerSelector *server_selector, IObLogRpcStub *rpc_stub, ObLogConfig &config) = 0;

        virtual void destroy() = 0;

        virtual int operator() (const char *tb_name, const ObLogConfig *config) = 0;

        virtual const ObLogSchema *get_schema() = 0;

        virtual void revert_schema(const ObLogSchema *schema) = 0;

        virtual const ObLogSchema *get_schema_newer_than(const ObLogSchema *old_schema) = 0;

        virtual int force_refresh() = 0;

        virtual int refresh_schema_greater_than(int64_t base_version) = 0;
    };

    ////////////////////////////////////////////////////////////////////////////////////////////////////

    class ObLogSchemaGetter : public IObLogSchemaGetter
    {
      static const int64_t FETCH_SCHEMA_TIMEOUT = 10L * 1000000L;
      static const int64_t RS_ADDR_REFRESH_INTERVAL = 60L * 1000000L;
      static const int64_t ALLOCATOR_PAGE_SIZE = 16L * 1024L * 1024L;
      static const int64_t OB_LOG_SCHEMA_SIZE_TOTAL_LIMIT = 4L * 1024L * 1024L * 1024L;
      static const int64_t OB_LOG_SCHEMA_SIZE_HOLD_LIMIT = ALLOCATOR_PAGE_SIZE * 2;
      static const int64_t SCHEMA_REFRESH_FAIL_RETRY_TIMEOUT = 10L * 1000000L;

      public:
        ObLogSchemaGetter();
        ~ObLogSchemaGetter();
      public:
        int init(IObLogServerSelector *server_selector, IObLogRpcStub *rpc_stub, ObLogConfig &config);
        void destroy();
        int operator() (const char *tb_name, const ObLogConfig *config);

        const ObLogSchema *get_schema();
        void revert_schema(const ObLogSchema *schema);

        /// @brief Get a newer schema than specific schema
        /// @param old_schema specific schema
        /// @return valid schema on success or NULL on fail
        const ObLogSchema *get_schema_newer_than(const ObLogSchema *old_schema);

        /// @brief Force refresh schema to get a newer one
        /// @retval OB_SUCCESS on success
        /// @retval ! OB_SUCCESS on fail
        int force_refresh();

        /// @brief Refresh schema to get a new one whose version is greater than specific version
        /// @param base_version target schema version should be greater than base_version.
        /// @retval OB_SUCCESS on success
        /// @retval ! OB_SUCCESS on fail
        int refresh_schema_greater_than(int64_t base_version);

      // Private member functions
      private:
        /// @brief Get current schema version
        /// @retval current schema version
        const int64_t get_current_schema_version_();

        /// @brief Refresh schema
        /// @param base_version target schema version should be greater than base_version.
        /// @retval OB_SUCCESS on success
        /// @retval ! OB_SUCCESS on fail
        int refresh_(int64_t base_version = common::OB_INVALID_VERSION);

        /// @brief Create a new ObLogSchema and fetch schema data from OceanBase
        /// @param[out] returned schema
        /// @retval OB_SUCCESS on success
        /// @retval ! OB_SUCCESS on fail
        int fetch_schema_(ObLogSchema *&schema);

        /// @brief Destroy ObLogSchema
        /// @param schema target schema
        void destroy_schema_(ObLogSchema *schema);

        /// @brief destroy table_name_array_
        void destroy_table_name_array_();

      private:
        bool inited_;
        IObLogServerSelector *server_selector_;
        IObLogRpcStub *rpc_stub_;
        common::SpinRWLock schema_get_lock_;
        common::ObSpinLock schema_refresh_lock_;
        tbsys::CThreadMutex multi_refresh_mutex_;

        ObLogSchema *cur_schema_;

        // Allocator
        common::ModulePageAllocator mod_;
        common::ModuleArena string_allocator_;

        common::FIFOAllocator schema_allocator_;  ///< Allocator for schema object

        common::ObArray<char *> table_name_array_;
    };
  }
}
#endif //OCEANBASE_LIBOBLOG_SCHEMA_GETTER_H_
