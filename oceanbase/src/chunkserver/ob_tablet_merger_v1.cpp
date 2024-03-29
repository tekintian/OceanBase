/*
 * (C) 2007-2010 Alibaba Group Holding Limited.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License 
 * version 2 as published by the Free Software Foundation. 
 *
 *         ob_tablet_merger.cpp is for what ...
 *
 *  Version: $Id: ob_tablet_merger.cpp 12/25/2012 03:42:46 PM qushan Exp $
 *
 *  Authors:
 *     qushan < qushan@taobao.com >
 *        - some work details if you want
 */


#include "ob_tablet_merger_v1.h"
#include "common/ob_schema.h"
#include "sstable/ob_sstable_schema.h"
#include "ob_chunk_server_main.h"
#include "common/file_directory_utils.h"
#include "ob_chunk_merge.h"
#include "ob_tablet_manager.h"

namespace oceanbase
{
  namespace chunkserver
  {
    using namespace tbutil;
    using namespace common;
    using namespace sstable;

    /*-----------------------------------------------------------------------------
     *  ObTabletMerger
     *-----------------------------------------------------------------------------*/

    ObTabletMerger::ObTabletMerger(ObChunkMerge& chunk_merge,ObTabletManager& manager) 
      : chunk_merge_(chunk_merge), manager_(manager), old_tablet_(NULL), frozen_version_(0)
    {
      sstable_id_.sstable_file_id_  = 0;
      sstable_id_.sstable_file_offset_ = 0;
      path_[0] = 0;
      tablet_array_.clear();
    }

    ObTabletMerger::~ObTabletMerger()
    {
    }

    int64_t ObTabletMerger::calc_tablet_checksum(const int64_t sstable_checksum)
    {
      int64_t tablet_checksum = 0;
      int64_t checksum_len = sizeof(uint64_t);
      char checksum_buf[checksum_len];
      int64_t pos = 0;
      if (OB_SUCCESS == serialization::encode_i64(checksum_buf, 
          checksum_len, pos, sstable_checksum))
      {
        tablet_checksum = ob_crc64(
            tablet_checksum, checksum_buf, checksum_len);
      }
      return tablet_checksum;
    }

    int ObTabletMerger::check_tablet(const ObTablet* tablet)
    {
      int ret = OB_SUCCESS;

      if (NULL == tablet)
      {
        TBSYS_LOG(WARN, "tablet is NULL");
        ret = OB_INVALID_ARGUMENT;
      }
      else if (!manager_.get_disk_manager().is_disk_avail(tablet->get_disk_no()))
      {
        TBSYS_LOG(WARN, "tablet:%s locate on bad disk:%d, sstable_id:%ld",
            to_cstring(tablet->get_range()), tablet->get_disk_no(),
            (tablet->get_sstable_id_list()).count() > 0 ?
            (tablet->get_sstable_id_list()).at(0).sstable_file_id_ : 0);
        ret = OB_ERROR;
      }
      //check sstable path where exist
      else if ((tablet->get_sstable_id_list()).count() > 0)
      {
        int64_t sstable_id = 0;
        char path[OB_MAX_FILE_NAME_LENGTH];
        path[0] = '\0';
        sstable_id = (tablet->get_sstable_id_list()).at(0).sstable_file_id_;

        if ( OB_SUCCESS != (ret = get_sstable_path(sstable_id, path, sizeof(path))) )
        {
          TBSYS_LOG(WARN, "can't get the path of sstable, tablet:%s sstable_id:%ld",
              to_cstring(tablet->get_range()), sstable_id);
        }
        else if (false == FileDirectoryUtils::exists(path))
        {
          TBSYS_LOG(WARN, "tablet:%s sstable file is not exist, path:%s, sstable_id:%ld",
              to_cstring(tablet->get_range()), path, sstable_id);
          ret = OB_FILE_NOT_EXIST;
        }
      }

      return ret;
    }

    int ObTabletMerger::update_meta(ObTablet* old_tablet, 
        const common::ObVector<ObTablet*> & tablet_array, const bool sync_meta)
    {
      int ret = OB_SUCCESS;
      ObMultiVersionTabletImage& tablet_image = manager_.get_serving_tablet_image();
      ObTablet *new_tablet_list[ tablet_array.size() ];
      int32_t idx = 0;
      if (OB_SUCCESS == ret)
      {
        ret = check_tablet(old_tablet);
      }

      for(ObVector<ObTablet *>::iterator it = tablet_array.begin();
          OB_SUCCESS == ret && it != tablet_array.end(); ++it)
      {
        if (NULL == (*it)) //in case
        {
          ret = OB_ERROR;
          break;
        }

        if(OB_SUCCESS != (ret = check_tablet((*it))))
        {
          break;
        }

        new_tablet_list[idx++] = (*it);
      }


      if (OB_SUCCESS == ret)
      {
        // in case we have migrated tablets, discard current merge tablet
        if (!old_tablet->is_merged())
        {
          if (OB_SUCCESS != (ret = tablet_image.upgrade_tablet(
                  old_tablet, new_tablet_list, idx, false)))
          {
            TBSYS_LOG(WARN,"upgrade new merged tablets error [%d]",ret);
          }
          else
          {
            if (sync_meta)
            {
              // sync new tablet meta files;
              for(int32_t i = 0; i < idx; ++i)
              {
                if (OB_SUCCESS != (ret = tablet_image.write(
                        new_tablet_list[i]->get_data_version(),
                        new_tablet_list[i]->get_disk_no())))
                {
                  TBSYS_LOG(WARN,"write new meta failed i=%d, version=%ld, disk_no=%d", i ,
                      new_tablet_list[i]->get_data_version(), new_tablet_list[i]->get_disk_no());
                }
              }

              // sync old tablet meta files;
              if (OB_SUCCESS == ret
                  && OB_SUCCESS != (ret = tablet_image.write(
                      old_tablet->get_data_version(), old_tablet->get_disk_no())))
              {
                TBSYS_LOG(WARN,"write old meta failed version=%ld, disk_no=%d",
                    old_tablet->get_data_version(), old_tablet->get_disk_no());
              }
            }

            if (OB_SUCCESS == ret)
            {
              int64_t recycle_version = old_tablet->get_data_version()
                - (ObMultiVersionTabletImage::MAX_RESERVE_VERSION_COUNT - 1);
              if (recycle_version > 0)
              {
                manager_.get_regular_recycler().recycle_tablet(
                    old_tablet->get_range(), recycle_version);
              }
            }

          }
        }
        else
        {
          TBSYS_LOG(INFO, "current tablet covered by migrated tablets, discard.");
        }
      }

      return ret;
    }

    int ObTabletMerger::gen_sstable_file_location(const int32_t disk_no, 
        sstable::ObSSTableId& sstable_id, char* path, const int64_t path_size)
    {
      int ret = OB_SUCCESS;
      bool is_sstable_exist = false;
      sstable_id.sstable_file_id_ = manager_.allocate_sstable_file_seq();
      sstable_id.sstable_file_offset_ = 0;
      do
      {
        sstable_id.sstable_file_id_ = (sstable_id_.sstable_file_id_ << 8) | (disk_no & DISK_NO_MASK);

        if ( OB_SUCCESS != (ret = get_sstable_path(sstable_id, path, path_size)) )
        {
          TBSYS_LOG(WARN, "Merge : can't get the path of new sstable, ");
        }
        else if (true == (is_sstable_exist = FileDirectoryUtils::exists(path_)))
        {
          // reallocate new file seq until get file name not exist.
          sstable_id_.sstable_file_id_ = manager_.allocate_sstable_file_seq();
        }
      } while (OB_SUCCESS == ret && is_sstable_exist);

      return ret;
    }

    int ObTabletMerger::init()
    {
      return OB_SUCCESS;
    }

    /*-----------------------------------------------------------------------------
     *  ObTabletMergerV1
     *-----------------------------------------------------------------------------*/

    ObTabletMergerV1::ObTabletMergerV1(ObChunkMerge& chunk_merge, ObTabletManager& manager) 
      : ObTabletMerger(chunk_merge, manager),
      cell_(NULL),
      new_table_schema_(NULL),
      current_sstable_size_(0),
      row_num_(0),
      pre_column_group_row_num_(0),
      cs_proxy_(manager),
      ms_wrapper_(*(ObChunkServerMain::get_instance()->get_chunk_server().get_rpc_proxy()),
                  ObChunkServerMain::get_instance()->get_chunk_server().get_config().merge_timeout),
      merge_join_agent_(cs_proxy_)
    {}

    int ObTabletMergerV1::init()
    {
      int ret = OB_SUCCESS;
      ObChunkServer&  chunk_server = ObChunkServerMain::get_instance()->get_chunk_server();

      if (OB_SUCCESS == ret && chunk_server.get_config().join_cache_size >= 1)
      {
        ms_wrapper_.get_ups_get_cell_stream()->set_cache(manager_.get_join_cache());
      }

      if (OB_SUCCESS == ret)
      {
        bool dio = chunk_server.get_config().write_sstable_use_dio;
        writer_.set_dio(dio);
      }

      return ret;
    }

    int ObTabletMergerV1::check_row_count_in_column_group()
    {
      int ret = OB_SUCCESS;
      if (pre_column_group_row_num_ != 0)
      {
        if (row_num_ != pre_column_group_row_num_)
        {
          TBSYS_LOG(ERROR,"the row num between two column groups is difference,[%ld,%ld]",
              row_num_,pre_column_group_row_num_);
          ret = OB_ERROR;
        }
      }
      else
      {
        pre_column_group_row_num_ = row_num_;
      }
      return ret;
    }

    void ObTabletMergerV1::reset_for_next_column_group()
    {
      row_.clear();
      reset_local_proxy();
      merge_join_agent_.clear();
      scan_param_.reset();
      row_num_ = 0;
    }

    int ObTabletMergerV1::save_current_row(const bool current_row_expired)
    {
      int ret = OB_SUCCESS;
      if (current_row_expired)
      {
        //TBSYS_LOG(DEBUG, "current row expired.");
        //hex_dump(row_.get_row_key().ptr(), row_.get_row_key().length(), false, TBSYS_LOG_LEVEL_DEBUG);
      }
      else if ((ret = writer_.append_row(row_,current_sstable_size_)) != OB_SUCCESS )
      {
        TBSYS_LOG(ERROR, "Merge : append row failed [%d], this row_, obj count:%ld, "
                         "table:%lu, group:%lu",
            ret, row_.get_obj_count(), row_.get_table_id(), row_.get_column_group_id());
        for(int32_t i=0; i<row_.get_obj_count(); ++i)
        {
          row_.get_obj(i)->dump(TBSYS_LOG_LEVEL_ERROR);
        }
      }
      return ret;
    }

    int ObTabletMergerV1::wait_aio_buffer() const
    {
      int ret = OB_SUCCESS;
      int status = 0;

      ObThreadAIOBufferMgrArray* aio_buf_mgr_array = GET_TSI_MULT(ObThreadAIOBufferMgrArray, TSI_SSTABLE_THREAD_AIO_BUFFER_MGR_ARRAY_1);
      if (NULL == aio_buf_mgr_array)
      {
        ret = OB_ERROR;
      }
      else if (OB_AIO_TIMEOUT == (status =
            aio_buf_mgr_array->wait_all_aio_buf_mgr_free(10 * 1000000)))
      {
        TBSYS_LOG(WARN, "failed to wait all aio buffer manager free, stop current thread");
        ret = OB_ERROR;
      }

      return ret;

    }

    int ObTabletMergerV1::reset_local_proxy() const
    {
      int ret = OB_SUCCESS;

      /**
       * if this function fails, it means that some critical problems
       * happen, don't kill chunk server here, just output some error
       * info, then we can restart this chunk server manualy.
       */
      ret = manager_.end_scan();
      if (OB_SUCCESS != ret)
      {
        TBSYS_LOG(WARN, "failed to end scan to release resources");
      }

      ret = reset_query_thread_local_buffer();
      if (OB_SUCCESS != ret)
      {
        TBSYS_LOG(WARN, "failed to reset query thread local buffer");
      }

      return ret;
    }

    int ObTabletMergerV1::reset() 
    {
      row_num_ = 0;
      frozen_version_ = 0;

      sstable_id_.sstable_file_id_ = 0;
      path_[0] = 0;

      sstable_schema_.reset();
      tablet_array_.clear();
      return reset_local_proxy();
    }

    int ObTabletMergerV1::merge_column_group(
        const int64_t column_group_idx,
        const uint64_t column_group_id,
        int64_t& split_row_pos,
        const int64_t max_sstable_size,
        const bool is_need_join,
        bool& is_tablet_splited,
        bool& is_tablet_unchanged)
    {
      int ret = OB_SUCCESS;
      RowStatus row_status = ROW_START;
      ObOperatorMemLimit mem_limit;

      bool is_row_changed = false;
      bool current_row_expired = false;
      bool need_filter = tablet_merge_filter_.need_filter();
      /**
       * there are 4 cases that we cann't do "unmerge_if_unchanged"
       * optimization
       * 1. the config of chunkserver doesn't enable this function
       * 2. need expire some data in this tablet
       * 3. the table need join another tables
       * 4. the sub range of this tablet is splited
       */
      bool unmerge_if_unchanged =
        (THE_CHUNK_SERVER.get_config().unmerge_if_unchanged
         && !need_filter && !is_need_join && !is_tablet_splited);
      int64_t expire_row_num = 0;
      mem_limit.merge_mem_size_ = THE_CHUNK_SERVER.get_config().merge_mem_limit;
      // in order to reuse the internal cell array of merge join agent
      mem_limit.max_merge_mem_size_ = mem_limit.merge_mem_size_ + 1024 * 1024;
      ms_wrapper_.get_ups_get_cell_stream()->set_timeout(THE_CHUNK_SERVER.get_config().merge_timeout);
      ms_wrapper_.get_ups_scan_cell_stream()->set_timeout(THE_CHUNK_SERVER.get_config().merge_timeout);

      is_tablet_splited = false;
      is_tablet_unchanged = false;

      if (OB_SUCCESS != (ret = wait_aio_buffer()))
      {
        TBSYS_LOG(ERROR, "wait aio buffer error, column_group_id = %ld", column_group_id);
      }
      else if (OB_SUCCESS != (ret = fill_scan_param( column_group_id )))
      {
        TBSYS_LOG(ERROR,"prepare scan param failed : [%d]",ret);
      }
      else if (OB_SUCCESS != (ret = tablet_merge_filter_.adjust_scan_param(
        column_group_idx, column_group_id, scan_param_)))
      {
        TBSYS_LOG(ERROR, "tablet merge filter adjust scan param failed : [%d]", ret);
      }
      else if (OB_SUCCESS != (ret = merge_join_agent_.start_agent(scan_param_,
              *ms_wrapper_.get_ups_scan_cell_stream(),
              *ms_wrapper_.get_ups_get_cell_stream(),
              chunk_merge_.current_schema_,
              mem_limit,0, unmerge_if_unchanged)))
      {
        TBSYS_LOG(ERROR,"set request param for merge_join_agent failed [%d]",ret);
        merge_join_agent_.clear();
        scan_param_.reset();
      }
      else if (merge_join_agent_.is_unchanged()
          && old_tablet_->get_sstable_id_list().count() > 0)
      {
        is_tablet_unchanged = true;
      }

      if (OB_SUCCESS == ret && 0 == column_group_idx && !is_tablet_unchanged
          && (ret = create_new_sstable()) != OB_SUCCESS)
      {
        TBSYS_LOG(ERROR,"create sstable failed.");
      }

      while(OB_SUCCESS == ret && !is_tablet_unchanged)
      {
        cell_ = NULL; //in case
        if ( manager_.is_stoped() )
        {
          TBSYS_LOG(WARN,"stop in merging column_group_id=%ld", column_group_id);
          ret = OB_CS_MERGE_CANCELED;
        }
        else if ((ret = merge_join_agent_.next_cell()) != OB_SUCCESS ||
            (ret = merge_join_agent_.get_cell(&cell_, &is_row_changed)) != OB_SUCCESS)
        {
          //end or error
          if (OB_ITER_END == ret)
          {
            TBSYS_LOG(DEBUG,"Merge : end of file");
            ret = OB_SUCCESS;
            if (need_filter && ROW_GROWING == row_status)
            {
              ret = tablet_merge_filter_.check_and_trim_row(column_group_idx, row_num_,
                row_, current_row_expired);
              if (OB_SUCCESS != ret)
              {
                TBSYS_LOG(ERROR, "tablet merge filter do check_and_trim_row failed, "
                                 "row_num_=%ld", row_num_);
              }
              else if (OB_SUCCESS == ret && current_row_expired)
              {
                ++expire_row_num;
              }
            }
            if (OB_SUCCESS == ret && ROW_GROWING == row_status
                && OB_SUCCESS == (ret = save_current_row(current_row_expired)))
            {
              ++row_num_;
              row_.clear();
              current_row_expired = false;
              row_status = ROW_END;
            }
          }
          else
          {
            TBSYS_LOG(ERROR,"Merge : get data error,ret : [%d]", ret);
          }

          // end of file
          break;
        }
        else if ( cell_ != NULL )
        {
          /**
           * there are some data in scanner
           */
          if ((ROW_GROWING == row_status && is_row_changed))
          {
            if (0 == (row_num_ % chunk_merge_.merge_pause_row_count_) )
            {
              if (chunk_merge_.merge_pause_sleep_time_ > 0)
              {
                if (0 == (row_num_ % (chunk_merge_.merge_pause_row_count_ * 20)))
                {
                  // print log every sleep 20 times;
                  TBSYS_LOG(INFO,"pause in merging,sleep %ld",
                      chunk_merge_.merge_pause_sleep_time_);
                }
                usleep(static_cast<useconds_t>(chunk_merge_.merge_pause_sleep_time_));
              }

            }

            if (need_filter)
            {
              ret = tablet_merge_filter_.check_and_trim_row(column_group_idx, row_num_,
                row_, current_row_expired);
              if (OB_SUCCESS != ret)
              {
                TBSYS_LOG(ERROR, "tablet merge filter do check_and_trim_row failed, "
                                 "row_num_=%ld", row_num_);
              }
              else if (OB_SUCCESS == ret && current_row_expired)
              {
                ++expire_row_num;
              }
            }

            // we got new row, write last row we build.
            if (OB_SUCCESS == ret
                && OB_SUCCESS == (ret = save_current_row(current_row_expired)) )
            {
              ++row_num_;
              current_row_expired = false;
              // start new row.
              row_status = ROW_START;
            }

            // this split will use current %row_, so we clear row below.
            if (OB_SUCCESS == ret
                && ((split_row_pos > 0 && row_num_ > split_row_pos)
                    || (0 == column_group_idx && 0 == split_row_pos
                        && current_sstable_size_ > max_sstable_size))
                && maybe_change_sstable())
            {
              /**
               * if the merging tablet is no data, we can't set a proper
               * split_row_pos, so we try to merge the first column group, and
               * the first column group split by sstable size, if the first
               * column group size is larger than max_sstable_size, we set the
               * current row count as the split_row_pos, the next clolumn
               * groups of the first splited tablet use this split_row_pos,
               * after merge the first splited tablet, we use the first
               * splited tablet as sampler, re-caculate the average row size
               * of tablet and split_row_pos, the next splited tablet of the
               * tablet will use the new split_row_pos.
               */
              if (0 == column_group_idx && 0 == split_row_pos
                  && current_sstable_size_ > max_sstable_size)
              {
                split_row_pos = row_num_;
                TBSYS_LOG(INFO, "column group %ld splited by sstable size, "
                                "current_sstable_size_=%ld, max_sstable_size=%ld, "
                                "split_row_pos=%ld",
                  column_group_idx, current_sstable_size_,
                  max_sstable_size, split_row_pos);
              }
              /**
               * record the end key
               */
              if ( (ret = update_range_end_key()) != OB_SUCCESS)
              {
                TBSYS_LOG(ERROR,"update end range error [%d]",ret);
              }
              else
              {
                is_tablet_splited = true;
              }
              // entercount split point, skip to merge next column group
              // we reset all status whenever start new column group, so
              // just break it, leave it to reset_for_next_column_group();
              break;
            }

            // before we build new row, must clear last row in use.
            row_.clear();
          }

          if ( OB_SUCCESS == ret && ROW_START == row_status)
          {
            // first cell in current row, set rowkey and table info.
            row_.set_rowkey(cell_->row_key_);
            row_.set_table_id(cell_->table_id_);
            row_.set_column_group_id(column_group_id);
            row_status = ROW_GROWING;
          }

          if (OB_SUCCESS == ret && ROW_GROWING == row_status)
          {
            // no need to add current cell to %row_ when current row expired.
            if ((ret = row_.add_obj(cell_->value_)) != OB_SUCCESS)
            {
              TBSYS_LOG(ERROR,"Merge : add_cell_to_row failed [%d]",ret);
            }
          }
        }
        else
        {
          TBSYS_LOG(ERROR,"get cell return success but cell is null");
          ret = OB_ERROR;
        }
      }

      TBSYS_LOG(INFO, "column group %ld merge completed, ret = %d, version_range=%s, "
          "total row count =%ld, expire row count = %ld, split_row_pos=%ld, "
          "max_sstable_size=%ld, is_tablet_splited=%d, is_tablet_unchanged=%d, range=%s",
          column_group_id, ret, range2str(scan_param_.get_version_range()),
          row_num_, expire_row_num, split_row_pos,
          max_sstable_size, is_tablet_splited, is_tablet_unchanged, to_cstring(new_range_));

      return ret;
    }

    bool ObTabletMergerV1::is_table_need_join(const uint64_t table_id)
    {
      bool ret = false;
      const ObColumnSchemaV2* column = NULL;
      int32_t column_size = 0;

      column = chunk_merge_.current_schema_.get_table_schema(table_id, column_size);
      if (NULL != column && column_size > 0)
      {
        for (int32_t i = 0; i < column_size; ++i)
        {
          if (NULL != column[i].get_join_info())
          {
            ret = true;
            break;
          }
        }
      }

      return ret;
    }

    int ObTabletMergerV1::merge(ObTablet *tablet,int64_t frozen_version)
    {
      int   ret = OB_SUCCESS;
      int64_t split_row_pos = 0;
      int64_t init_split_row_pos = 0;
      bool    is_tablet_splited = false;
      bool    is_first_column_group_splited = false;
      bool    is_tablet_unchanged = false;
      int64_t max_sstable_size = OB_DEFAULT_MAX_TABLET_SIZE;
      bool    sync_meta = THE_CHUNK_SERVER.get_config().each_tablet_sync_meta;

      ObTablet* new_tablet = NULL;

      FILL_TRACE_LOG("start merge tablet");
      if (NULL == tablet || frozen_version <= 0)
      {
        TBSYS_LOG(ERROR,"merge : interal error ,param invalid frozen_version:[%ld]",frozen_version);
        ret = OB_ERROR;
      }
      else if ( OB_SUCCESS != (ret = reset()) )
      {
        TBSYS_LOG(ERROR, "reset query thread local buffer error.");
      }
      else if ( NULL == (new_table_schema_ =
            chunk_merge_.current_schema_.get_table_schema(tablet->get_range().table_id_)) )
      {
        //This table has been deleted
        TBSYS_LOG(INFO,"table (%lu) has been deleted",tablet->get_range().table_id_);
        tablet->set_merged();
        ret = OB_CS_TABLE_HAS_DELETED;
      }
      else if (OB_SUCCESS != (ret = fill_sstable_schema(*new_table_schema_,sstable_schema_)))
      {
        TBSYS_LOG(ERROR, "convert table schema to sstable schema failed, table=%ld",
            tablet->get_range().table_id_);
      }

      if (OB_SUCCESS == ret)
      {
        // parse compress name from schema, may be NULL;
        const char *compressor_name = new_table_schema_->get_compress_func_name();
        if (NULL == compressor_name || '\0' == *compressor_name )
        {
          TBSYS_LOG(WARN,"no compressor with this sstable.");
        }
        compressor_string_.assign_ptr(const_cast<char *>(compressor_name),static_cast<int32_t>(strlen(compressor_name)));

        // if schema define max sstable size for table, use it
        if (new_table_schema_->get_max_sstable_size() > 0)
        {
          max_sstable_size = new_table_schema_->get_max_sstable_size();
        }
        else
        {
          TBSYS_LOG(INFO, "schema doesn't specify max_sstable_size, "
              "use default value, max_sstable_size=%ld", max_sstable_size);
        }

        // according to sstable size, compute the average row count of each sstable
        split_row_pos = tablet->get_row_count();
        int64_t over_size_percent = THE_CHUNK_SERVER.get_config().over_size_percent_to_split;
        int64_t split_size = (over_size_percent > 0) ? (max_sstable_size * (100 + over_size_percent) / 100) : 0;
        int64_t occupy_size = tablet->get_occupy_size();
        int64_t row_count = tablet->get_row_count();
        if (occupy_size > split_size && row_count > 0)
        {
          int64_t avg_row_size = occupy_size / row_count;
          if (avg_row_size > 0)
          {
            split_row_pos = max_sstable_size / avg_row_size;
          }
        }
        /**
         * if user specify over_size_percent, and the tablet size is
         * greater than 0 and not greater than split_size threshold, we
         * don't split this tablet. if tablet size is 0, use the default
         * split algorithm
         */
        if (over_size_percent > 0 && occupy_size > 0 && occupy_size <= split_size)
        {
          // set split_row_pos = -1, this tablet will not split
          split_row_pos = -1;
        }
        init_split_row_pos = split_row_pos;
      }


      if (OB_SUCCESS == ret)
      {
        new_range_ = tablet->get_range();
        old_tablet_ = tablet;
        frozen_version_ = frozen_version;
        int64_t sstable_id = 0;
        char range_buf[OB_RANGE_STR_BUFSIZ];
        ObSSTableId sst_id;
        char path[OB_MAX_FILE_NAME_LENGTH];
        path[0] = '\0';

        if ((tablet->get_sstable_id_list()).count() > 0)
          sstable_id = (tablet->get_sstable_id_list()).at(0).sstable_file_id_;

        tablet->get_range().to_string(range_buf,sizeof(range_buf));
        if (0 != sstable_id)
        {
          sst_id.sstable_file_id_ = sstable_id;
          get_sstable_path(sst_id, path, sizeof(path));
        }
        TBSYS_LOG(INFO, "start merge sstable_id:%ld, old_version=%ld, "
            "new_version=%ld, split_row_pos=%ld, table_row_count=%ld, "
            "tablet_occupy_size=%ld, compressor=%.*s, path=%s, range=%s",
            sstable_id, tablet->get_data_version(), frozen_version,
            split_row_pos, tablet->get_row_count(),
            tablet->get_occupy_size(), compressor_string_.length(),
            compressor_string_.ptr(), path, range_buf);

        uint64_t column_group_ids[OB_MAX_COLUMN_GROUP_NUMBER];
        int64_t column_group_num = sizeof(column_group_ids) / sizeof(column_group_ids[0]);

        if ( OB_SUCCESS != (ret = sstable_schema_.get_table_column_groups_id(
                new_table_schema_->get_table_id(), column_group_ids,column_group_num))
            || column_group_num <= 0)
        {
          TBSYS_LOG(ERROR,"get column groups failed, column_group_num=%ld, ret=%d",
              column_group_num, ret);
        }
        bool is_need_join = is_table_need_join(new_table_schema_->get_table_id());

        if (OB_SUCCESS == ret &&
            OB_SUCCESS != (ret = tablet_merge_filter_.init(chunk_merge_.current_schema_,
              column_group_num, tablet, frozen_version, chunk_merge_.frozen_timestamp_)))
        {
          TBSYS_LOG(ERROR, "failed to initialize tablet merge filter, table=%ld",
              tablet->get_range().table_id_);
        }

        FILL_TRACE_LOG("after prepare merge tablet, column_group_num=%d, ret=%d",
          column_group_num, ret);
        while(OB_SUCCESS == ret)
        {
          if ( manager_.is_stoped() )
          {
            TBSYS_LOG(WARN,"stop in merging");
            ret = OB_CS_MERGE_CANCELED;
          }

          // clear all bits while start merge new tablet.
          // generally in table split.
          tablet_merge_filter_.clear_expire_bitmap();
          pre_column_group_row_num_ = 0;
          is_first_column_group_splited  = false;

          for(int32_t group_index = 0; (group_index < column_group_num) && (OB_SUCCESS == ret); ++group_index)
          {
            if ( manager_.is_stoped() )
            {
              TBSYS_LOG(WARN,"stop in merging");
              ret = OB_CS_MERGE_CANCELED;
            }
            // %expire_row_filter will be set in first column group,
            // and test in next merge column group.
            else if ( OB_SUCCESS != ( ret = merge_column_group(
                  group_index, column_group_ids[group_index],
                  split_row_pos, max_sstable_size, is_need_join,
                  is_tablet_splited, is_tablet_unchanged)) )
            {
              TBSYS_LOG(ERROR, "merge column group[%d] = %lu , group num = %ld,"
                  "split_row_pos = %ld error.",
                  group_index, column_group_ids[group_index],
                  column_group_num, split_row_pos);
            }
            else if ( OB_SUCCESS != (ret = check_row_count_in_column_group()) )
            {
              TBSYS_LOG(ERROR, "check row count column group[%d] = %lu",
                  group_index, column_group_ids[group_index]);
            }

            // only when merging the first column group, the function merge_column_group()
            // set the is_tablet_splited flag
            if (OB_SUCCESS == ret && 0 == group_index)
            {
              is_first_column_group_splited = is_tablet_splited;
            }

            FILL_TRACE_LOG("finish merge column group %d, is_splited=%d, "
                           "row_num=%ld, ret=%d",
              group_index, is_tablet_splited, row_num_, ret);

            // reset for next column group..
            // page arena reuse avoid memory explosion
            reset_for_next_column_group();

            // if the first column group is unchaged, the tablet is unchanged,
            // break the merge loop
            if (OB_SUCCESS == ret && 0 == group_index && is_tablet_unchanged)
            {
              break;
            }
          }

          // all column group has been written,finish this sstable

          if (OB_SUCCESS == ret
              && (ret = finish_sstable(is_tablet_unchanged, new_tablet))!= OB_SUCCESS)
          {
            TBSYS_LOG(ERROR,"close sstable failed [%d]",ret);
          }

          // not split, break
          if (OB_SUCCESS == ret && !is_first_column_group_splited)
          {
            break;
          }

          FILL_TRACE_LOG("start merge new splited tablet");

          /**
           * if the merging tablet is no data, we can't set a proper
           * split_row_pos, so we try to merge the first column group, and
           * the first column group split by sstable size, if the first
           * column group size is larger than max_sstable_size, we set the
           * current row count as the split_row_pos, the next clolumn
           * groups of the first splited tablet use this split_row_pos,
           * after merge the first splited tablet, we use the first
           * splited tablet as sampler, re-caculate the average row size
           * of tablet and split_row_pos, the next splited tablet of the
           * tablet will use the new split_row_pos.
           */
          if (OB_SUCCESS == ret && 0 == init_split_row_pos
              && split_row_pos > 0 && NULL != new_tablet
              && new_tablet->get_occupy_size() > 0
              && new_tablet->get_row_count() > 0)
          {
            int64_t avg_row_size = new_tablet->get_occupy_size() / new_tablet->get_row_count();
            if (avg_row_size > 0)
            {
              split_row_pos = max_sstable_size / avg_row_size;
            }
            init_split_row_pos = split_row_pos;
          }

          if (OB_SUCCESS == ret)
          {
            update_range_start_key();

            //prepare the next request param
            new_range_.end_key_ = tablet->get_range().end_key_;
            if (tablet->get_range().end_key_.is_max_row())
            {
              TBSYS_LOG(INFO,"this tablet has max flag,reset it");
              new_range_.end_key_.set_max_row();
              new_range_.border_flag_.unset_inclusive_end();
            }
          }
        } // while (OB_SUCCESS == ret) //finish one tablet

        if (OB_SUCCESS == ret)
        {
          ret = update_meta(old_tablet_, tablet_array_, sync_meta);
        }
        else
        {
          TBSYS_LOG(ERROR,"merge failed,don't add these tablet");

          int64_t t_off = 0;
          int64_t s_size = 0;
          writer_.close_sstable(t_off,s_size);
          if (sstable_id_.sstable_file_id_ != 0)
          {
            if (strlen(path_) > 0)
            {
              unlink(path_); //delete
              TBSYS_LOG(WARN,"delete %s",path_);
            }
            manager_.get_disk_manager().add_used_space((sstable_id_.sstable_file_id_ & DISK_NO_MASK),0);
          }

          int64_t sstable_id = 0;

          for(ObVector<ObTablet *>::iterator it = tablet_array_.begin(); it != tablet_array_.end(); ++it)
          {
            if ( ((*it) != NULL) && ((*it)->get_sstable_id_list().count() > 0))
            {
              sstable_id = (*it)->get_sstable_id_list().at(0).sstable_file_id_;
              if (OB_SUCCESS == get_sstable_path(sstable_id,path_,sizeof(path_)))
              {
                unlink(path_);
              }
            }
          }
          tablet_array_.clear();
          path_[0] = '\0';
          current_sstable_size_ = 0;
        }
      }
      FILL_TRACE_LOG("finish merge tablet, ret=%d", ret);
      PRINT_TRACE_LOG();
      CLEAR_TRACE_LOG();

      return ret;
    }


    int ObTabletMergerV1::fill_sstable_schema(const ObTableSchema& common_schema,ObSSTableSchema& sstable_schema)
    {
      int ret = OB_SUCCESS;
      bool binary_format = chunk_merge_.write_sstable_version_ < SSTableReader::ROWKEY_SSTABLE_VERSION;
      sstable_schema.reset();
      const ObRowkeyInfo & rowkey_info = chunk_merge_.current_schema_.get_table_schema(
          common_schema.get_table_id())->get_rowkey_info();
      // set rowkey info for write old fashion sstable
      if (binary_format)
      {
        TBSYS_LOG(INFO, "table=%ld, rowkey info=%s, binary length=%ld",
            common_schema.get_table_id(), to_cstring(rowkey_info), rowkey_info.get_binary_rowkey_length());
        row_.set_rowkey_info(&rowkey_info);
      }

      ret = build_sstable_schema(common_schema.get_table_id(), chunk_merge_.current_schema_,
          sstable_schema, binary_format);
      if ( 0 == sstable_schema.get_column_count() && OB_SUCCESS == ret ) //this table has moved to updateserver
      {
        ret = OB_CS_TABLE_HAS_DELETED;
      }

      return ret;
    }

    int ObTabletMergerV1::create_new_sstable()
    {
      int ret                          = OB_SUCCESS;
      sstable_id_.sstable_file_id_     = manager_.allocate_sstable_file_seq();
      sstable_id_.sstable_file_offset_ = 0;
      int32_t disk_no                  = manager_.get_disk_manager().get_dest_disk();
      int64_t sstable_block_size       = OB_DEFAULT_SSTABLE_BLOCK_SIZE;
      bool is_sstable_exist            = false;

      if (disk_no < 0)
      {
        TBSYS_LOG(ERROR,"does't have enough disk space");
        sstable_id_.sstable_file_id_ = 0;
        ret = OB_CS_OUTOF_DISK_SPACE;
      }

      // if schema define sstable block size for table, use it
      // for the schema with version 2, the default block size is 64(KB),
      // we skip this case and use the config of chunkserver
      if (NULL != new_table_schema_ && new_table_schema_->get_block_size() > 0
          && 64 != new_table_schema_->get_block_size())
      {
        sstable_block_size = new_table_schema_->get_block_size();
      }

      if (OB_SUCCESS == ret)
      {
        /**
         * mustn't use the same sstable id in the the same disk, because
         * if tablet isn't changed, we just add a hard link pointer to
         * the old sstable file, so maybe different sstable file pointer
         * the same content in disk. if we cancle daily merge, maybe
         * some tablet meta isn't synced into index file, then we
         * restart chunkserver will do daily merge again, it may reuse
         * the same sstable id, if the sstable id is existent and it
         * pointer to a share disk content, the sstable will be
         * truncated if we create sstable with the sstable id.
         */
        do
        {
          sstable_id_.sstable_file_id_     = (sstable_id_.sstable_file_id_ << 8) | (disk_no & 0xff);

          if ((OB_SUCCESS == ret) && (ret = get_sstable_path(sstable_id_,path_,sizeof(path_))) != OB_SUCCESS )
          {
            TBSYS_LOG(ERROR,"Merge : can't get the path of new sstable");
            ret = OB_ERROR;
          }

          if (OB_SUCCESS == ret)
          {
            is_sstable_exist = FileDirectoryUtils::exists(path_);
            if (is_sstable_exist)
            {
              sstable_id_.sstable_file_id_ = manager_.allocate_sstable_file_seq();
            }
          }
        } while (OB_SUCCESS == ret && is_sstable_exist);

        if (OB_SUCCESS == ret)
        {
          TBSYS_LOG(INFO,"create new sstable, sstable_path:%s, compressor:%.*s, "
                         "version=%ld, block_size=%ld\n",
              path_, compressor_string_.length(), compressor_string_.ptr(),
              frozen_version_, sstable_block_size);
          path_string_.assign_ptr(path_,static_cast<int32_t>(strlen(path_) + 1));

          int64_t element_count = DEFAULT_ESTIMATE_ROW_COUNT;
          if (NULL != old_tablet_ && old_tablet_->get_row_count() != 0)
          {
            element_count = old_tablet_->get_row_count();
          }

          if ((ret = writer_.create_sstable(sstable_schema_,
                  path_string_, compressor_string_, frozen_version_,
                  OB_SSTABLE_STORE_DENSE, sstable_block_size, element_count)) != OB_SUCCESS)
          {
            if (OB_IO_ERROR == ret)
              manager_.get_disk_manager().set_disk_status(disk_no,DISK_ERROR);
            TBSYS_LOG(ERROR,"Merge : create sstable failed : [%d]",ret);
          }
        }
      }
      return ret;
    }

    int ObTabletMergerV1::fill_scan_param(const uint64_t column_group_id)
    {
      int ret = OB_SUCCESS;

      ObString table_name_string;

      //scan_param_.reset();
      scan_param_.set(new_range_.table_id_, table_name_string, new_range_);
      /**
       * in merge,we do not use cache,
       * and just read frozen mem table.
       */
      scan_param_.set_is_result_cached(false);
      scan_param_.set_is_read_consistency(false);

      int64_t preread_mode = THE_CHUNK_SERVER.get_config().merge_scan_use_preread;
      if ( 0 == preread_mode )
      {
        scan_param_.set_read_mode(ScanFlag::SYNCREAD);
      }
      else
      {
        scan_param_.set_read_mode(ScanFlag::ASYNCREAD);
      }

      ObVersionRange version_range;
      version_range.border_flag_.set_inclusive_end();

      version_range.start_version_ =  old_tablet_->get_data_version();
      version_range.end_version_   =  old_tablet_->get_data_version() + 1;

      scan_param_.set_version_range(version_range);

      int64_t size = 0;
      const ObSSTableSchemaColumnDef *col = sstable_schema_.get_group_schema(
          new_table_schema_->get_table_id(), column_group_id, size);

      if (NULL == col || size <= 0)
      {
        TBSYS_LOG(ERROR,"cann't find this column group:%lu, table_id=%ld",
            column_group_id, new_table_schema_->get_table_id());
        sstable_schema_.dump();
        ret = OB_ERROR;
      }
      else
      {
        for(int32_t i = 0; i < size; ++i)
        {
          if ( (ret = scan_param_.add_column( (col + i)->column_name_id_ ) ) != OB_SUCCESS)
          {
            TBSYS_LOG(ERROR,"add column id [%u] to scan_param_ error[%d]",(col + i)->column_name_id_, ret);
            break;
          }
        }
      }
      return ret;
    }

    int ObTabletMergerV1::update_range_start_key()
    {
      int ret = OB_SUCCESS;
      // set split_rowkey_ as start key of next range.
      // must copy split_rowkey_ to last_rowkey_buffer_,
      // next split will modify split_rowkey_;
      ObMemBufAllocatorWrapper allocator(last_rowkey_buffer_, ObModIds::OB_SSTABLE_WRITER);
      if (OB_SUCCESS != (ret = split_rowkey_.deep_copy(new_range_.start_key_, allocator)))
      {
        TBSYS_LOG(WARN, "set split_rowkey_(%s) as start key of new range error.", to_cstring(split_rowkey_));
      }
      else
      {
        new_range_.border_flag_.unset_inclusive_start();
      }
      return ret;
    }

    int ObTabletMergerV1::update_range_end_key()
    {
      int ret = OB_SUCCESS;
      int32_t split_pos = new_table_schema_->get_split_pos();

      int64_t rowkey_column_count = 0;
      sstable_schema_.get_rowkey_column_count(new_range_.table_id_, rowkey_column_count);
      common::ObRowkey rowkey(NULL, rowkey_column_count);
      ObMemBufAllocatorWrapper allocator(split_rowkey_buffer_, ObModIds::OB_SSTABLE_WRITER);

      if (rowkey_column_count <= 0)
      {
        TBSYS_LOG(INFO, "table: %ld has %ld column rowkey",
            new_range_.table_id_, rowkey_column_count);
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != (ret = row_.get_rowkey(rowkey)))
      {
        TBSYS_LOG(ERROR, "get rowkey for current row error.");
      }
      else if (OB_SUCCESS != (ret = rowkey.deep_copy(split_rowkey_, allocator)))
      {
        TBSYS_LOG(ERROR, "deep copy current row:%s to split_rowkey_ error", to_cstring(rowkey));
      }
      else
      {
        TBSYS_LOG(INFO, "current rowkey:%s,split rowkey (%s), split pos=%d",
            to_cstring(rowkey), to_cstring(split_rowkey_), split_pos);
      }

      if (split_pos > 0 && split_pos < split_rowkey_.get_obj_cnt())
      {
        const_cast<ObObj*>(split_rowkey_.get_obj_ptr()+split_pos)->set_max_value();
      }

      if (OB_SUCCESS == ret)
      {
        new_range_.end_key_ = split_rowkey_;
        new_range_.border_flag_.set_inclusive_end();
      }
      return ret;
    }

    int ObTabletMergerV1::create_hard_link_sstable(int64_t& sstable_size)
    {
      int ret = OB_SUCCESS;
      ObSSTableId old_sstable_id;
      char old_path[OB_MAX_FILE_NAME_LENGTH];
      sstable_size = 0;

      /**
       * when do daily merge, the old tablet is unchanged, there is no
       * dynamic data in update server for this old tablet. we needn't
       * merge this tablet, just create a hard link for the unchanged
       * tablet at the same disk. althrough the hard link only add the
       * reference count of inode, and both sstable names refer to the
       * same sstable file, there is oly one copy on disk.
       *
       * after create a hard link, we also add the disk usage space.
       * so the disk usage space statistic is not very correct, but
       * when the sstable is recycled, the disk space of the sstable
       * will be decreased. so we can ensure the recycle logical is
       * correct.
       */
      if (old_tablet_->get_sstable_id_list().count() > 0)
      {
        int32_t disk_no = old_tablet_->get_disk_no();

        /**
         * mustn't use the same sstable id in the the same disk, because
         * if tablet isn't changed, we just add a hard link pointer to
         * the old sstable file, so maybe different sstable file pointer
         * the same content in disk. if we cancle daily merge, maybe
         * some tablet meta isn't synced into index file, then we
         * restart chunkserver will do daily merge again, it may reuse
         * the same sstable id, if the sstable id is existent and it
         * pointer to a share disk content, the sstable will be
         * truncated if we create sstable with the sstable id.
         */
        do
        {
          sstable_id_.sstable_file_id_ = manager_.allocate_sstable_file_seq();
          sstable_id_.sstable_file_id_ = (sstable_id_.sstable_file_id_ << 8) | (disk_no & 0xff);

          if ((ret = get_sstable_path(sstable_id_,path_,sizeof(path_))) != OB_SUCCESS )
          {
            TBSYS_LOG(ERROR, "create_hard_link_sstable: can't get the path of hard link sstable");
            ret = OB_ERROR;
          }
        } while (OB_SUCCESS == ret && FileDirectoryUtils::exists(path_));

        if (OB_SUCCESS == ret)
        {
          /**
           * FIXME: current we just support one tablet with only one
           * sstable file
           */
          sstable_size = (*old_tablet_->get_sstable_reader_list().at(0)).get_sstable_size();
          old_sstable_id = old_tablet_->get_sstable_id_list().at(0);
          if ((ret = get_sstable_path(old_sstable_id, old_path, sizeof(old_path))) != OB_SUCCESS )
          {
            TBSYS_LOG(ERROR, "create_hard_link_sstable: can't get the path of old sstable");
            ret = OB_ERROR;
          }
          else if (0 != ::link(old_path, path_))
          {
            TBSYS_LOG(ERROR, "failed create hard link for unchanged sstable, "
                             "old_sstable=%s, new_sstable=%s",
              old_path, path_);
            ret = OB_ERROR;
          }
        }
      }

      return ret;
    }

    int ObTabletMergerV1::finish_sstable(const bool is_tablet_unchanged,
      ObTablet*& new_tablet)
    {
      int64_t trailer_offset = 0;
      int64_t sstable_size = 0;
      int ret = OB_SUCCESS;
      uint64_t row_checksum = 0;
      new_tablet = NULL;
      TBSYS_LOG(DEBUG,"Merge : finish_sstable");
      if (!is_tablet_unchanged)
      {
        row_checksum = writer_.get_row_checksum();
        if (OB_SUCCESS != (ret = writer_.set_tablet_range(new_range_)))
        {
          TBSYS_LOG(ERROR, "failed to set tablet range for sstable writer");
        }
        else if (OB_SUCCESS != (ret = writer_.close_sstable(trailer_offset,sstable_size)))
        {
          TBSYS_LOG(ERROR,"Merge : close sstable failed.");
        }
        else if (sstable_size < 0)
        {
          TBSYS_LOG(ERROR, "Merge : close sstable failed, sstable_size=%ld", sstable_size);
            ret = OB_ERROR;
        }
      }

      if (OB_SUCCESS == ret)
      {
        ObMultiVersionTabletImage& tablet_image = manager_.get_serving_tablet_image();
        const ObSSTableTrailer* trailer = NULL;
        ObTablet *tablet = NULL;
        ObTabletExtendInfo extend_info;
        row_num_ = 0;
        pre_column_group_row_num_ = 0;
        if ((ret = tablet_image.alloc_tablet_object(new_range_,frozen_version_,tablet)) != OB_SUCCESS)
        {
          TBSYS_LOG(ERROR,"alloc_tablet_object failed.");
        }
        else
        {
          if (is_tablet_unchanged)
          {
            tablet->set_disk_no(old_tablet_->get_disk_no());
            row_checksum = old_tablet_->get_row_checksum();
            ret = create_hard_link_sstable(sstable_size);
            if (OB_SUCCESS == ret && sstable_size > 0
                && old_tablet_->get_sstable_id_list().count() > 0)
            {
              trailer = &(dynamic_cast<ObSSTableReader*>(old_tablet_->get_sstable_reader_list().at(0)))->get_trailer();
            }
          }
          else
          {
            trailer = &writer_.get_trailer();
            tablet->set_disk_no( (sstable_id_.sstable_file_id_) & DISK_NO_MASK);
          }

          if (OB_SUCCESS == ret)
          {
            //set new data version
            tablet->set_data_version(frozen_version_);
            if (tablet_merge_filter_.need_filter())
            {
              extend_info.last_do_expire_version_ = frozen_version_;
            }
            else
            {
              extend_info.last_do_expire_version_ = old_tablet_->get_last_do_expire_version();
            }
            if (sstable_size > 0)
            {
              if (NULL == trailer)
              {
                TBSYS_LOG(ERROR, "after close sstable, trailer=NULL, "
                                 "is_tablet_unchanged=%d, sstable_size=%ld",
                  is_tablet_unchanged, sstable_size);
                ret = OB_ERROR;
              }
              else if ((ret = tablet->add_sstable_by_id(sstable_id_)) != OB_SUCCESS)
              {
                TBSYS_LOG(ERROR,"Merge : add sstable to tablet failed.");
              }
              else
              {
                int64_t pos = 0;
                int64_t checksum_len = sizeof(uint64_t);
                char checksum_buf[checksum_len];

                /**
                 * compatible with the old version, before the sstable code
                 * don't support empty sstable, if it's empty, no sstable file
                 * is created. now it support empty sstable, and there is no row
                 * data in sstable, the statistic of tablet remain constant, the
                 * occupy size, row count adn checksum is 0
                 *
                 */
                if (trailer->get_row_count() > 0)
                {
                  extend_info.occupy_size_ = sstable_size;
                  extend_info.row_count_ = trailer->get_row_count();
                  ret = serialization::encode_i64(checksum_buf,
                      checksum_len, pos, trailer->get_sstable_checksum());
                  if (OB_SUCCESS == ret)
                  {
                    extend_info.check_sum_ = ob_crc64(checksum_buf, checksum_len);
                  }
                }
              }
            }
            else if (0 == sstable_size)
            {
              /**
               * the tablet doesn't include sstable, the occupy_size_,
               * row_count_ and check_sum_ in extend_info is 0, needn't set
               * them again
               */
            }

            //inherit sequence number from parent tablet
            extend_info.sequence_num_ = old_tablet_->get_sequence_num();
            extend_info.row_checksum_ = row_checksum;
          }

          /**
           * we set the extent info of tablet here. when we write the
           * index file, we needn't load the sstable of this tablet to get
           * the extend info.
           */
          if (OB_SUCCESS == ret)
          {
            tablet->set_extend_info(extend_info);
          }
        }

        if (OB_SUCCESS == ret)
        {
          ret = tablet_array_.push_back(tablet);
        }

        if (OB_SUCCESS == ret)
        {
          manager_.get_disk_manager().add_used_space(
            (sstable_id_.sstable_file_id_ & DISK_NO_MASK),
            sstable_size, !is_tablet_unchanged);
        }

        if (OB_SUCCESS == ret && NULL != tablet)
        {
          new_tablet = tablet;

          if (is_tablet_unchanged)
          {
            char range_buf[OB_RANGE_STR_BUFSIZ];
            new_range_.to_string(range_buf,sizeof(range_buf));
            TBSYS_LOG(INFO, "finish_sstable, create hard link sstable=%s, range=%s, "
                            "sstable_size=%ld, row_count=%ld",
              path_, range_buf, sstable_size, extend_info.row_count_);
          }
        }
      }
      if (OB_SUCCESS == ret)
      {
        path_[0] = '\0';
        current_sstable_size_ = 0;
      }
      return ret;
    }

    int ObTabletMergerV1::report_tablets(ObTablet* tablet_list[],int32_t tablet_size)
    {
      int  ret = OB_SUCCESS;
      int64_t num = OB_MAX_TABLET_LIST_NUMBER;
      ObTabletReportInfoList *report_info_list =  GET_TSI_MULT(ObTabletReportInfoList, TSI_CS_TABLET_REPORT_INFO_LIST_1);

      if (tablet_size < 0)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else if (NULL == report_info_list)
      {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
      else
      {
        report_info_list->reset();
        ObTabletReportInfo tablet_info;
        for(int32_t i=0; i < tablet_size && num > 0 && OB_SUCCESS == ret; ++i)
        {
          manager_.fill_tablet_info(*tablet_list[i], tablet_info);
          if (OB_SUCCESS != (ret = report_info_list->add_tablet(tablet_info)) )
          {
            TBSYS_LOG(WARN, "failed to add tablet info, num=%ld, err=%d", num, ret);
          }
          else
          {
            --num;
          }
        }
        if (OB_SUCCESS != (ret = manager_.send_tablet_report(*report_info_list,true))) //always has more
        {
          TBSYS_LOG(WARN, "failed to send request report to rootserver err = %d",ret);
        }
      }
      return ret;
    }

    bool ObTabletMergerV1::maybe_change_sstable() const
    {
      bool ret = false;
      int64_t rowkey_cmp_size = new_table_schema_->get_split_pos();
      const uint64_t table_id = row_.get_table_id();
      int64_t rowkey_column_count = 0;
      ObRowkey last_rowkey;

      if (row_.is_binary_rowkey())
      {
        rowkey_column_count = row_.get_rowkey_info()->get_size();
      }
      else
      {
        sstable_schema_.get_rowkey_column_count(table_id, rowkey_column_count);
      }
      last_rowkey.assign(NULL, rowkey_column_count);
      row_.get_rowkey(last_rowkey);

      if (0 < rowkey_cmp_size
          && last_rowkey.get_obj_cnt() >= rowkey_cmp_size
          && cell_->row_key_.get_obj_cnt() >= rowkey_cmp_size)
      {
        for (int i = 0; i < rowkey_cmp_size; ++i)
        {
          if (last_rowkey.get_obj_ptr()[i] != cell_->row_key_.get_obj_ptr()[i])
          {
            ret = true;
          }
        }
      }
      else
      {
        ret = true;
      }
      return ret;
    }
  } /* chunkserver */
} /* oceanbase */
