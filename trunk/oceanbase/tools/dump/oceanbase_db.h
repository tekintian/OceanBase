/*
 * =====================================================================================
 *
 *       Filename:  OceanbaseDb.h
 *
 *        Version:  1.0
 *        Created:  04/12/2011 04:48:39 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  yushun.swh (DBA Group), yushun.swh@taobao.com
 *        Company:  taobao
 *
 * =====================================================================================
 */
#ifndef OB_API_OCEANBASEDB_H
#define  OB_API_OCEANBASEDB_H

#include "common/ob_packet_factory.h"
#include "common/ob_client_manager.h"
#include "common/ob_server.h"
#include "common/ob_string.h"
#include "common/ob_scanner.h"
#include "common/ob_result.h"
#include "common/utility.h"
#include "common/ob_schema.h"
#include "common/ob_mutator.h"
#include "common/ob_object.h"
#include "common/ob_base_client.h"
#include "db_table_info.h"
#include <string>
#include <map>
#include <vector>

#define RPC_WITH_RETIRES(RPC, retries, RET) {                                                       \
  int __RPC_LOCAL_INDEX__ = 0;                                                                      \
  while (__RPC_LOCAL_INDEX__++ < retries) {                                                         \
    (RET) = (RPC);                                                                                  \
    if ((RET) != 0) {                                                                               \
      TBSYS_LOG(WARN, "call {%s} failed, ret = %d, retry = %d", #RPC, (RET), __RPC_LOCAL_INDEX__);  \
    } else {                                                                                        \
      break;                                                                                        \
    }                                                                                               \
  }                                                                                                 \
}

namespace oceanbase {
  namespace api {
    const int TSI_GET_ID = 1001;
    const int TSI_SCAN_ID = 1002;
    const int TSI_MBUF_ID = 1003;

    class DbRecordSet;
    using namespace common;

    const int64_t kDefaultTimeout = 1000000;

    class DbTranscation;
    class OceanbaseDb;

    class RowMutator {
      public:
        friend class DbTranscation;
      public:

        int delete_row();
        int add(const char *column_name, const ObObj &val);
        int add(const std::string &column_name, const ObObj &val);

        int32_t op() const { return op_; }

        RowMutator();
        RowMutator(const std::string &table_name, const ObRowkey &rowkey, 
            DbTranscation *tnx);

      private:
        
        void set_op(int32_t op) { op_ = op; }
        void set(const std::string &table_name, const ObRowkey &rowkey, 
            DbTranscation *tnx);

        std::string table_name_;
        ObRowkey rowkey_;
        DbTranscation *tnx_;
        int32_t op_;
    };

    class DbTranscation {
      public:
        friend class RowMutator;
        friend class OceanbaseDb;
      public:
        DbTranscation();
        DbTranscation(OceanbaseDb *db);

        int insert_mutator(const char* table_name, const ObRowkey &rowkey, RowMutator *mutator);
        int update_mutator(const char* table_name, const ObRowkey &rowkey, RowMutator *mutator);

        int free_row_mutator(RowMutator *&mutator);

        int commit();
        int abort();
        int reset();
        inline void set_db(OceanbaseDb *db)
        {
          db_ = db;
          assert(db_ != NULL);
        }

      private:
        int add_cell(ObMutatorCellInfo &cell);

        OceanbaseDb *db_;
        ObMutator mutator_;
    };

    struct DbMutiGetRow {
      DbMutiGetRow() :columns(NULL) { }

      ObRowkey rowkey;
      const std::vector<std::string> *columns;
      std::string table;
    };

    class OceanbaseDb {
      private:
        typedef std::map<ObRowkey, TabletInfo> CacheRow;
        typedef std::map<std::string, CacheRow > CacheSet;
      public:
        struct DbStats {
          int64_t total_succ_gets;
          int64_t total_fail_gets;
          int64_t total_send_bytes;
          int64_t total_recv_bytes;
          int64_t total_succ_apply;
          int64_t total_fail_apply;

          DbStats() {
            memset(this, 0, sizeof(DbStats));
          }
        };

      public:
        static const uint64_t kTabletTimeout = 10;
        friend class DbTranscation;
      public:
        OceanbaseDb(const char *ip, unsigned short port, int64_t timeout = kDefaultTimeout, 
            uint64_t tablet_timeout = kTabletTimeout);
        ~OceanbaseDb();

        int get(const std::string &table, const std::vector<std::string> &columns,
            const ObRowkey &rowkey, DbRecordSet &rs);

        int get(const std::string &table, const std::vector<std::string> &columns,
            const std::vector<ObRowkey> &rowkeys, DbRecordSet &rs);

        int get(const std::vector<DbMutiGetRow> &rows, DbRecordSet &rs);

        int init();
        static int global_init(const char*log_dir, const char *level);
        int get_ms_location(const ObRowkey &row_key, const std::string &table_name);

        int get_tablet_location(const std::string &table, const ObRowkey &rowkey, 
            common::ObServer &server);

        int fetch_schema(common::ObSchemaManagerV2& schema_manager);

        int search_tablet_cache(const std::string &table, const ObRowkey &rowkey, TabletInfo &loc);
        void insert_tablet_cache(const std::string &table, const ObRowkey &rowkey, TabletInfo &tablet);

        DbStats db_stats() const { return db_stats_; }

        int start_tnx(DbTranscation *&tnx);
        void end_tnx(DbTranscation *&tnx);

        int get_update_server(ObServer &server);

        void set_consistency(bool consistency) { consistency_ = consistency; }

        bool get_consistency() const { return consistency_; }

        int scan(const TabletInfo &tablets, const std::string &table, const std::vector<std::string> &columns, 
            const ObRowkey &start_key, const ObRowkey &end_key, DbRecordSet &rs, int64_t version = 0, 
            bool inclusive_start = false, bool inclusive_end = true);

        int scan(const std::string &table, const std::vector<std::string> &columns, 
            const ObRowkey &start_key, const ObRowkey &end_key, DbRecordSet &rs, int64_t version = 0, 
            bool inclusive_start = false, bool inclusive_end = true);

        int get_memtable_version(int64_t &version);

        int set_limit_info(const int64_t offset, const int64_t count);

      private:
        int init_get_param(ObGetParam *&param, const std::vector<DbMutiGetRow> &rows);

        int do_server_get(common::ObServer &server, 
            const ObRowkey& row_key, common::ObScanner &scanner, 
            common::ObDataBuffer& data_buff, const std::string &table_name, 
            const std::vector<std::string> &columns);

        int do_muti_get(common::ObServer &server, const std::vector<DbMutiGetRow>& rows, 
            ObScanner &scanner, ObDataBuffer& data_buff);

        common::ObScanParam *get_scan_param(const std::string &table, const std::vector<std::string>& columns,
            const ObRowkey &start_key, const ObRowkey &end_key,
            bool inclusive_start = false, bool inclusive_end = true,
            int64_t data_version = 0);

        void free_tablet_cache();
        void mark_ms_failure(ObServer &server, const std::string &table, const ObRowkey &rowkey);
        void try_mark_server_fail(TabletInfo &tablet_info, ObServer &server, bool &do_erase_tablet);

        int do_server_cmd(const ObServer &server, const int32_t opcode, 
            ObDataBuffer &inout_buffer, int64_t &pos);

        //reference count of db handle
        void unref() { __sync_fetch_and_sub(&db_ref_, 1); }
        void ref() { __sync_fetch_and_add (&db_ref_, 1); }

        common::ObServer root_server_;
        common::ObServer update_server_;
        common::ObBaseClient client_;

        CacheSet cache_set_;
        tbsys::CThreadMutex cache_lock_;

        uint64_t tablet_timeout_;
        int64_t timeout_;
        bool inited_;

        DbStats db_stats_;

        int64_t db_ref_;
        bool consistency_;

        int64_t limit_offset_;
        /// 0 means not limit
        int64_t limit_count_;
    };
  }
}

#endif
