/**
 * (C) 2010-2011 Alibaba Group Holding Limited.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 * 
 * Version: $Id$
 *
 * test_get.cpp for ...
 *
 * Authors:
 *   rizhao <rizhao.ych@taobao.com>
 *
 */
#include <iostream>
#include <sstream>
#include <algorithm>
#include <gtest/gtest.h>
#include "tblog.h"
#include "test_helper.h"
#include "test_init.h"
#include "updateserver/ob_ups_table_mgr.h"
#include "updateserver/ob_update_server_main.h"
#include "test_ups_table_mgr_helper.h"

using namespace std;
using namespace oceanbase::common;
using namespace oceanbase::updateserver;


namespace oceanbase
{
namespace tests
{
namespace updateserver
{

class TestGet : public ::testing::Test
{
public:
  virtual void SetUp()
  {

  }

  virtual void TearDown()
  {

  }
};

TEST_F(TestGet, test_get_one_row)
{
  int err = 0;
  CommonSchemaManagerWrapper schema_mgr;
  tbsys::CConfig config;
  bool ret_val = schema_mgr.parse_from_file("scan_schema.ini", config);
  ASSERT_TRUE(ret_val);
  ObUpsTableMgr& mgr = ObUpdateServerMain::get_instance()->get_update_server().get_table_mgr();
  err = mgr.init();
  ASSERT_EQ(0, err);
  err = mgr.set_schemas(schema_mgr);
  ASSERT_EQ(0, err);

  TestUpsTableMgrHelper test_helper(mgr);

  TableMgr& table_mgr = test_helper.get_table_mgr();
  table_mgr.sstable_scan_finished(10);

  TableItem* active_memtable_item = table_mgr.get_active_memtable();
  MemTable& active_memtable = active_memtable_item->get_memtable();

  // construct multi-row mutator
  static const int64_t ROW_NUM = 1;
  static const int64_t COL_NUM = 10;

  ObCellInfo cell_infos[ROW_NUM][COL_NUM];
  char row_key_strs[ROW_NUM][50];
  uint64_t table_id = 10;
  // init cell infos
  for (int64_t i = 0; i < ROW_NUM; ++i)
  {
    sprintf(row_key_strs[i], "row_key_%08ld", i);
    for (int64_t j = 0; j < COL_NUM; ++j)
    {
      cell_infos[i][j].table_id_ = table_id;
      cell_infos[i][j].row_key_.assign(row_key_strs[i], strlen(row_key_strs[i]));

      cell_infos[i][j].column_id_ = j + 1;

      cell_infos[i][j].value_.set_int(1000 + i * COL_NUM + j);
    }
  }

  ObUpsMutator ups_mutator;
  ObMutator &mutator = ups_mutator.get_mutator();
  // add cell to array
  for (int64_t i = 0; i < ROW_NUM; ++i)
  {
    for (int64_t j = 0; j < COL_NUM; ++j)
    {
      ObMutatorCellInfo mutator_cell;
      mutator_cell.cell_info = cell_infos[i][j];
      mutator_cell.op_type.set_ext(ObActionFlag::OP_UPDATE);
      err = mutator.add_cell(mutator_cell);
      EXPECT_EQ(0, err);
    }
  }

  // write row to active memtable
  MemTableTransHandle write_handle;
  err = active_memtable.start_transaction(WRITE_TRANSACTION, write_handle);
  ASSERT_EQ(0, err);
  ups_mutator.set_mutate_timestamp(0);
  err = active_memtable.set(write_handle, ups_mutator);
  EXPECT_EQ(0, err);
  err = active_memtable.end_transaction(write_handle);
  ASSERT_EQ(0, err);
  /*
  ObString text;
  text.assign("/tmp", strlen("/tmp"));
  active_memtable.dump2text(text);
  */

  for (int64_t i = 0; i < ROW_NUM; ++i)
  {
    ObScanner scanner;
    ObGetParam get_param;
    //get_param.set_timestamp(version);
    ObVersionRange version_range;
    //version_range.start_version_ = version;
    //version_range.end_version_ = version;
    version_range.start_version_ = 2;
    version_range.end_version_ = 2;
    version_range.border_flag_.set_inclusive_start();
    version_range.border_flag_.set_inclusive_end();
    get_param.set_version_range(version_range);
    for (int64_t j = 0; j < COL_NUM; ++j)
    {
      get_param.add_cell(cell_infos[i][j]);
    }

    int64_t count = 0;
    err = mgr.get(get_param, scanner, tbsys::CTimeUtil::getTime(), 2000000000 * 1000L * 1000L);
    EXPECT_EQ(0, err);

    // check result
    count = 0;
    while (OB_SUCCESS == scanner.next_cell())
    {
      ObCellInfo* p_cell = NULL;
      scanner.get_cell(&p_cell);
      ASSERT_TRUE(p_cell != NULL);
      ObCellInfo expected = cell_infos[count / COL_NUM + i][count % COL_NUM];
      check_cell(expected, *p_cell);
      ++count;
    }
    EXPECT_EQ(COL_NUM, count);
  }
}

TEST_F(TestGet, test_get_multi_rows)
{
}

} // end namespace updateserver
} // end namespace tests
} // end namespace oceanbase


int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}



