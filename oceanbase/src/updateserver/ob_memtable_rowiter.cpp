////===================================================================
 //
 // ob_memtable_rowiter.cpp updateserver / Oceanbase
 //
 // Copyright (C) 2010 Taobao.com, Inc.
 //
 // Created on 2011-03-24 by Yubai (yubai.lk@taobao.com) 
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

#include "common/ob_define.h"
#include "common/ob_atomic.h"
#include "common/ob_tsi_factory.h"
#include "sstable/ob_sstable_trailer.h"
#include "sstable/ob_sstable_block_builder.h"
#include "ob_memtable_rowiter.h"
#include "ob_update_server_main.h"

namespace oceanbase
{
  namespace updateserver
  {
    using namespace common;
    using namespace hash;

    MemTableRowIterator::MemTableRowIterator() : memtable_(NULL),
                                                 memtable_iter_(),
                                                 get_iter_(),
                                                 rc_iter_()
    {
      reset_();
    }

    MemTableRowIterator::~MemTableRowIterator()
    {
      destroy();
    }

    int MemTableRowIterator::init(MemTable *memtable, const int store_type)
    {
      int ret = OB_SUCCESS;
      if (NULL != memtable_)
      {
        TBSYS_LOG(WARN, "have already inited");
        ret = OB_INIT_TWICE;
      }
      else if (NULL == memtable)
      {
        TBSYS_LOG(WARN, "invalid param memtable=%p", memtable_);
        ret = OB_INVALID_ARGUMENT;
      }
      else if (!get_schema_handle_())
      {
        TBSYS_LOG(WARN, "get schema handle fail");
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != (ret = memtable->scan_all(memtable_iter_)))
      {
        TBSYS_LOG(WARN, "scan all start fail ret=%d memtable=%p", ret, memtable);
        revert_schema_handle_();
      }
      else
      {
        store_type_ = store_type;
        memtable_ = memtable;
      }
      return ret;
    }

    void MemTableRowIterator::destroy()
    {
      if (NULL != memtable_)
      {
        memtable_iter_.reset();
        revert_schema_handle_();
        memtable_ = NULL;
      }
      reset_();
    }

    void MemTableRowIterator::reset_()
    {
      schema_handle_ = UpsSchemaMgr::INVALID_SCHEMA_HANDLE;
      store_type_ = 0;
    }

    void MemTableRowIterator::revert_schema_handle_()
    {
      ObUpdateServerMain *ups_main = ObUpdateServerMain::get_instance();
      if (NULL != ups_main)
      {
        UpsSchemaMgr &schema_mgr = ups_main->get_update_server().get_table_mgr().get_schema_mgr();
        schema_mgr.revert_schema_handle(schema_handle_);
      }
    }

    bool MemTableRowIterator::get_schema_handle_()
    {
      bool bret = false;
      ObUpdateServerMain *ups_main = ObUpdateServerMain::get_instance();
      if (NULL != ups_main)
      {
        UpsSchemaMgr &schema_mgr = ups_main->get_update_server().get_table_mgr().get_schema_mgr();
        bret = (OB_SUCCESS == schema_mgr.get_schema_handle(schema_handle_));
      }
      return bret;
    }

    int MemTableRowIterator::reset_iter()
    {
      int ret = OB_SUCCESS;
      if (NULL == memtable_)
      {
        TBSYS_LOG(WARN, "have not inited this=%p", this);
        ret = OB_NOT_INIT;
      }
      else
      {
        TBSYS_LOG(INFO, "reset row_iter this=%p", this);
        memtable_iter_.reset();
        ret = memtable_->scan_all(memtable_iter_);
      }
      return ret;
    }

    int MemTableRowIterator::next_row()
    {
      int ret = OB_SUCCESS;
      if (NULL == memtable_)
      {
        TBSYS_LOG(WARN, "have not inited this=%p", this);
        ret = OB_NOT_INIT;
      }
      else
      {
        ret = memtable_iter_.next();
      }
      return ret;
    }

    int MemTableRowIterator::get_row(sstable::ObSSTableRow &sstable_row)
    {
      int ret = OB_SUCCESS;
      const TEKey key = memtable_iter_.get_key();
      const TEValue *pvalue = memtable_iter_.get_value();
      if (NULL == memtable_)
      {
        TBSYS_LOG(WARN, "have not inited this=%p", this);
        ret = OB_NOT_INIT;
      }
      else if (NULL == pvalue)
      {
        TBSYS_LOG(WARN, "value null pointer");
        ret = OB_ERROR;
      }
      else
      {
        sstable_row.clear();
        if (OB_SUCCESS != (ret = sstable_row.set_internal_rowkey(key.table_id, key.row_key)))
        {
          TBSYS_LOG(WARN, "set internal rowkey to sstable_row fail key=[%s]", key.log_str());
        }
        // TODO sstable是否支持读取rowkey列
        get_iter_.set_(key, pvalue, NULL, true, NULL);
        rc_iter_.set_iterator(&get_iter_);
        while (OB_SUCCESS == (ret = rc_iter_.next_cell()))
        {
          ObCellInfo *ci = NULL;
          if (OB_SUCCESS != (ret = rc_iter_.get_cell(&ci))
              || NULL == ci)
          {
            TBSYS_LOG(WARN, "get cell from get_iter/rc_iter fail ret=%d", ret);
            ret = (OB_SUCCESS == ret) ? OB_ERROR : ret;
            break;
          }
          // sstable不接受column_id为OB_INVALID_ID所以转成了0
          // 迭代出来后需要再修改成OB_INVALID_ID
          uint64_t column_id = ci->column_id_;
          if (OB_INVALID_ID == column_id)
          {
            column_id = OB_FULL_ROW_COLUMN_ID;
          }
          if (OB_SUCCESS != (ret = sstable_row.shallow_add_obj(ci->value_, column_id)))
          {
            TBSYS_LOG(WARN, "add obj to sstable_row fail ret=%d [%s]", ret, print_cellinfo(ci));
            break;
          }
        }
        ret = (OB_ITER_END == ret) ? OB_SUCCESS : ret;
      }
      return ret;
    }

    bool MemTableRowIterator::get_compressor_name(ObString &compressor_str)
    {
      bool bret = false;
      ObUpdateServerMain *ups_main = ObUpdateServerMain::get_instance();
      if (NULL != ups_main)
      {
        const char *compressor_name = ups_main->get_update_server().get_param().sstable_compressor_name;
        if (NULL == compressor_name
            || 0 == strlen(compressor_name))
        {
          compressor_name = DEFAULT_COMPRESSOR_NAME;
        }
        compressor_str.assign_ptr(const_cast<char*>(compressor_name), static_cast<int32_t>(strlen(compressor_name)));
        TBSYS_LOG(INFO, "get compressor_name=%s from config", compressor_name);
        bret = true;
      }
      return bret;
    }

    bool MemTableRowIterator::get_sstable_schema(sstable::ObSSTableSchema &sstable_schema)
    {
      int bret = false;
      if (NULL != memtable_)
      {
        ObUpdateServerMain *ups_main = ObUpdateServerMain::get_instance();
        if (NULL != ups_main)
        {
          UpsSchemaMgr &schema_mgr = ups_main->get_update_server().get_table_mgr().get_schema_mgr();
          if (OB_SUCCESS == schema_mgr.build_sstable_schema(schema_handle_, sstable_schema))
          {
            bret = true;
          }
        }
      }
      return bret;
    }

    const ObRowkeyInfo *MemTableRowIterator::get_rowkey_info(const uint64_t table_id) const
    {
      return RowkeyInfoCache::get_rowkey_info(table_id);
    }

    bool MemTableRowIterator::get_store_type(int &store_type)
    {
      bool bret = false;
      if (NULL != memtable_)
      {
        store_type = store_type_;
        bret = true;
      }
      return bret;
    }

    bool MemTableRowIterator::get_block_size(int64_t &block_size)
    {
      bool bret = false;
      ObUpdateServerMain *ups_main = ObUpdateServerMain::get_instance();
      if (NULL != ups_main)
      {
        block_size = ups_main->get_update_server().get_param().sstable_block_size;
        TBSYS_LOG(INFO, "get block_size=%ld from config", block_size);
        bret = true;
      }
      return bret;
    }
  }
}

