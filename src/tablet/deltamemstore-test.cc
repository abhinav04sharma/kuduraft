// Copyright (c) 2012, Cloudera, inc.

#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include <gtest/gtest.h>
#include <stdlib.h>
#include <tr1/unordered_set>

#include "common/schema.h"
#include "gutil/casts.h"
#include "gutil/gscoped_ptr.h"
#include "tablet/deltamemstore.h"
#include "util/test_macros.h"

namespace kudu {
namespace tablet {

using std::tr1::unordered_set;

static void GenerateRandomIndexes(uint32_t range, uint32_t count,
                                  unordered_set<uint32_t> *out) {
  CHECK_LE(count, range / 2) <<
    "this will be too slow unless count is much smaller than range";
  out->clear();

  for (int i = 0; i < count; i++) {
    bool inserted = false;
    do {
      inserted = out->insert(random() % range).second;
    } while (!inserted);
  }
}

static void ApplyUpdates(DeltaMemStore *dms,
                         const MvccSnapshot &snapshot,
                         uint32_t row_idx,
                         size_t col_idx,
                         ColumnBlock *cb) {
  ColumnSchema col_schema(dms->schema().column(col_idx));
  Schema single_col_projection(boost::assign::list_of(col_schema), 0);

  gscoped_ptr<DeltaIteratorInterface> iter(
    dms->NewDeltaIterator(single_col_projection, snapshot));
  ASSERT_STATUS_OK(iter->Init());
  ASSERT_STATUS_OK(iter->SeekToOrdinal(row_idx));
  ASSERT_STATUS_OK(iter->PrepareBatch(cb->nrows()));
  ASSERT_STATUS_OK(iter->ApplyUpdates(0, cb));
  
}

TEST(TestDeltaMemStore, TestDMSSparseUpdates) {
  Schema schema(boost::assign::list_of
                (ColumnSchema("col1", UINT32)),
                1);

  shared_ptr<DeltaMemStore> dms(new DeltaMemStore(schema));
  faststring buf;
  RowChangeListEncoder update(schema, &buf);
  MvccManager mvcc;

  int n_rows = 1000;

  // Update 100 random rows out of the 1000.
  srand(12345);
  unordered_set<uint32_t> indexes_to_update;
  GenerateRandomIndexes(n_rows, 100, &indexes_to_update);
  BOOST_FOREACH(uint32_t idx_to_update, indexes_to_update) {
    ScopedTransaction tx(&mvcc);
    buf.clear();
    update.AddColumnUpdate(0, &idx_to_update);

    dms->Update(tx.txid(), idx_to_update, RowChangeList(buf));
  }
  ASSERT_EQ(100, dms->Count());

  // Now apply the updates from the DMS back to an array
  ScopedColumnBlock<UINT32> read_back(1000);
  for (int i = 0; i < 1000; i++) {
    read_back[i] = 0xDEADBEEF;
  }
  MvccSnapshot snap(mvcc);
  ApplyUpdates(dms.get(), snap, 0, 0, &read_back);

  // And verify that only the rows that we updated are modified within
  // the array.
  for (int i = 0; i < 1000; i++) {
    // If this wasn't one of the ones we updated, expect our marker
    if (indexes_to_update.find(i) == indexes_to_update.end()) {
      // If this wasn't one of the ones we updated, expect our marker
      ASSERT_EQ(0xDEADBEEF, read_back[i]);
    } else {
      // Otherwise expect the updated value
      ASSERT_EQ(i, read_back[i]);
    }
  }
}

// Test when a slice column has been updated multiple times in the
// memstore that the referred to values properly end up in the
// right arena.
TEST(TestDeltaMemStore, TestReUpdateSlice) {
  Schema schema(boost::assign::list_of
                (ColumnSchema("col1", STRING)),
                1);
  shared_ptr<DeltaMemStore> dms(new DeltaMemStore(schema));
  faststring update_buf;
  RowChangeListEncoder update(schema, &update_buf);
  MvccManager mvcc;

  // Update a cell, taking care that the buffer we use to perform
  // the update gets cleared after usage. This ensures that the
  // underlying data is properly copied into the DMS arena.
  {
    ScopedTransaction tx(&mvcc);
    char buf[256] = "update 1";
    Slice s(buf);
    update.AddColumnUpdate(0, &s);
    dms->Update(tx.txid(), 123, RowChangeList(update_buf));
    memset(buf, 0xff, sizeof(buf));
  }
  MvccSnapshot snapshot_after_first_update(mvcc);

  // Update the same cell again with a different value
  {
    ScopedTransaction tx(&mvcc);
    char buf[256] = "update 2";
    Slice s(buf);
    update_buf.clear();
    update.AddColumnUpdate(0, &s);
    dms->Update(tx.txid(), 123, RowChangeList(update_buf));
    memset(buf, 0xff, sizeof(buf));
  }
  MvccSnapshot snapshot_after_second_update(mvcc);

  // Ensure we end up with a second entry for the cell, at the
  // new txid
  ASSERT_EQ(2, dms->Count());

  // Ensure that we ended up with the right data, and that the old MVCC snapshot
  // yields the correct old value.
  ScopedColumnBlock<STRING> read_back(1);
  ApplyUpdates(dms.get(), snapshot_after_first_update, 123, 0, &read_back);
  ASSERT_EQ("update 1", read_back[0].ToString());

  ApplyUpdates(dms.get(), snapshot_after_second_update, 123, 0, &read_back);
  ASSERT_EQ("update 2", read_back[0].ToString());
}


TEST(TestDeltaMemStore, TestDMSBasic) {
  Schema schema(boost::assign::list_of
                 (ColumnSchema("col1", STRING))
                 (ColumnSchema("col2", STRING))
                 (ColumnSchema("col3", UINT32)),
                1);
  shared_ptr<DeltaMemStore> dms(new DeltaMemStore(schema));
  MvccManager mvcc;
  faststring update_buf;
  RowChangeListEncoder update(schema, &update_buf);

  char buf[256];
  for (uint32_t i = 0; i < 1000; i++) {
    ScopedTransaction tx(&mvcc);
    update_buf.clear();

    uint32_t val = i * 10;
    update.AddColumnUpdate(2, &val);

    snprintf(buf, sizeof(buf), "hello %d", i);
    Slice s(buf);
    update.AddColumnUpdate(0, &s);

    dms->Update(tx.txid(), i, RowChangeList(update_buf));
  }

  ASSERT_EQ(1000, dms->Count());

  // Read back the values and check correctness.
  MvccSnapshot snap(mvcc);
  ScopedColumnBlock<UINT32> read_back(1000);
  ScopedColumnBlock<STRING> read_back_slices(1000);
  ApplyUpdates(dms.get(), snap, 0, 2, &read_back);
  ApplyUpdates(dms.get(), snap, 0, 0, &read_back_slices);

  // When reading back the slice, do so into a different buffer -
  // otherwise if the slice references weren't properly copied above,
  // we'd be writing our comparison value into the same buffer that
  // we're comparing against!
  char buf2[256];
  for (uint32_t i = 0; i < 1000; i++) {
    ASSERT_EQ(i * 10, read_back[i]) << "failed at iteration " << i;
    snprintf(buf2, sizeof(buf2), "hello %d", i);
    Slice s(buf2);
    ASSERT_EQ(0, s.compare(read_back_slices[i]));
  }


  // Update the same rows again, with new transactions. Even though
  // the same rows are updated, new entries should be added because
  // these are separate transactions and we need to maintain the
  // old ones for snapshot consistency purposes.
  for (uint32_t i = 0; i < 1000; i++) {
    ScopedTransaction tx(&mvcc);
    update_buf.clear();

    uint32_t val = i * 20;
    update.AddColumnUpdate(2, &val);
    dms->Update(tx.txid(), i, RowChangeList(update_buf));
  }

  ASSERT_EQ(2000, dms->Count());
}

TEST(TestDMSIterator, TestIteratorDoesUpdates) {
  // TODO: share some code with above test case
  Schema schema(boost::assign::list_of
                (ColumnSchema("col1", UINT32)),
                1);
  shared_ptr<DeltaMemStore> dms(new DeltaMemStore(schema));
  faststring update_buf;
  RowChangeListEncoder update(schema, &update_buf);
  MvccManager mvcc;

  for (uint32_t i = 0; i < 1000; i++) {
    ScopedTransaction tx(&mvcc);
    update_buf.clear();
    uint32_t val = i * 10;
    update.AddColumnUpdate(0, &val);
    dms->Update(tx.txid(), i, RowChangeList(update_buf));
  }
  ASSERT_EQ(1000, dms->Count());

  // TODO: test snapshot reads from different points
  MvccSnapshot snap(mvcc);
  ScopedColumnBlock<UINT32> block(100);
  gscoped_ptr<DMSIterator> iter(down_cast<DMSIterator *>(dms->NewDeltaIterator(schema, snap)));
  ASSERT_STATUS_OK(iter->Init(););

  int block_start_row = 50;
  ASSERT_STATUS_OK(iter->SeekToOrdinal(block_start_row));
  ASSERT_STATUS_OK(iter->PrepareBatch(block.nrows()));
  VLOG(1) << "prepared: " << Slice(iter->prepared_buf_).ToDebugString();

  ASSERT_STATUS_OK(iter->ApplyUpdates(0, &block));

  for (int i = 0; i < 100; i++) {
    int actual_row = block_start_row + i;
    ASSERT_EQ(actual_row * 10, block[i]);
  }

  // Apply the next block
  block_start_row += block.nrows();
  ASSERT_STATUS_OK(iter->PrepareBatch(block.nrows()));
  ASSERT_STATUS_OK(iter->ApplyUpdates(0, &block));
  for (int i = 0; i < 100; i++) {
    int actual_row = block_start_row + i;
    ASSERT_EQ(actual_row * 10, block[i]);
  }
}

} // namespace tabletype
} // namespace kudu
