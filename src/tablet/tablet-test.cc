// Copyright (c) 2012, Cloudera, inc.

#include <glog/logging.h>
#include <time.h>

#include "common/iterator.h"
#include "common/row.h"
#include "tablet/memstore.h"
#include "tablet/tablet.h"
#include "tablet/tablet-test-base.h"
#include "util/slice.h"
#include "util/test_macros.h"

namespace kudu {
namespace tablet {

using std::tr1::unordered_set;

DEFINE_int32(testflush_num_inserts, 1000,
             "Number of rows inserted in TestFlush");


TEST_F(TestTablet, TestFlush) {
  // Insert 1000 rows into memstore
  RowBuilder rb(schema_);
  InsertTestRows(0, FLAGS_testflush_num_inserts);

  // Flush it.
  ASSERT_STATUS_OK(tablet_->Flush());
}

// Test that inserting a row which already exists causes an AlreadyPresent
// error
TEST_F(TestTablet, TestInsertDuplicateKey) {
  RowBuilder rb(schema_);
  rb.AddString(Slice("hello world"));
  rb.AddUint32(12345);
  rb.AddUint32(0);
  ASSERT_STATUS_OK(tablet_->Insert(rb.data()));

  // Insert again, should fail!
  Status s = tablet_->Insert(rb.data());
  ASSERT_TRUE(s.IsAlreadyPresent()) <<
    "expected AlreadyPresent, but got: " << s.ToString();

  ASSERT_EQ(1, TabletCount());

  // Flush, and make sure that inserting duplicate still fails
  ASSERT_STATUS_OK(tablet_->Flush());

  ASSERT_EQ(1, TabletCount());

  s = tablet_->Insert(rb.data());
  ASSERT_TRUE(s.IsAlreadyPresent()) <<
    "expected AlreadyPresent, but got: " << s.ToString();

  ASSERT_EQ(1, TabletCount());
}

// Test iterating over a tablet which contains data
// in the memstore as well as two layers. This simple test
// only puts one row in each with no updates.
TEST_F(TestTablet, TestRowIteratorSimple) {
  // Put a row in disk layer 1 (insert and flush)
  RowBuilder rb(schema_);
  rb.AddString(Slice("hello from layer 1"));
  rb.AddUint32(1);
  rb.AddUint32(0);
  ASSERT_STATUS_OK(tablet_->Insert(rb.data()));
  ASSERT_STATUS_OK(tablet_->Flush());

  // Put a row in disk layer 2 (insert and flush)
  rb.Reset();
  rb.AddString(Slice("hello from layer 2"));
  rb.AddUint32(2);
  rb.AddUint32(0);
  ASSERT_STATUS_OK(tablet_->Insert(rb.data()));
  ASSERT_STATUS_OK(tablet_->Flush());

  // Put a row in memstore
  rb.Reset();
  rb.AddString(Slice("hello from memstore"));
  rb.AddUint32(3);
  rb.AddUint32(0);
  ASSERT_STATUS_OK(tablet_->Insert(rb.data()));

  // Now iterate the tablet and make sure the rows show up
  scoped_ptr<Tablet::RowIterator> iter;
  ASSERT_STATUS_OK(tablet_->NewRowIterator(schema_, &iter));
  ASSERT_TRUE(iter->HasNext());

  scoped_array<uint8_t> buf(new uint8_t[schema_.byte_size() * 100]);
  RowBlock block(schema_, &buf[0], 100, &arena_);

  // First call to CopyNextRows should fetch the whole memstore.
  size_t n = 100;
  ASSERT_STATUS_OK(iter->CopyNextRows(&n, &block));
  ASSERT_EQ(1, n) << "should get only the one row from memstore";
  ASSERT_EQ("(string key=hello from memstore, uint32 val=3, uint32 update_count=0)",
            schema_.DebugRow(&buf[0]))
    << "should have retrieved the row data from memstore";

  // Next, should fetch the older layer
  ASSERT_TRUE(iter->HasNext());
  n = 100;
  ASSERT_STATUS_OK(iter->CopyNextRows(&n, &block));
  ASSERT_EQ(1, n) << "should get only the one row from layer 1";
  ASSERT_EQ("(string key=hello from layer 1, uint32 val=1, uint32 update_count=0)",
            schema_.DebugRow(&buf[0]))
    << "should have retrieved the row data from layer 1";

  // Next, should fetch the newer layer
  ASSERT_TRUE(iter->HasNext());
  n = 100;
  ASSERT_STATUS_OK(iter->CopyNextRows(&n, &block));
  ASSERT_EQ(1, n) << "should get only the one row from layer 2";
  ASSERT_EQ("(string key=hello from layer 2, uint32 val=2, uint32 update_count=0)",
            schema_.DebugRow(&buf[0]))
    << "should have retrieved the row data from layer 2";

  ASSERT_FALSE(iter->HasNext());
}

// Test iterating over a tablet which has a memstore
// and several layers, each with many rows of data.
TEST_F(TestTablet, TestRowIteratorComplex) {
  // Put a row in disk layer 1 (insert and flush)
  RowBuilder rb(schema_);
  char keybuf[256];
  unordered_set<uint32_t> inserted;
  for (uint32_t i = 0; i < 1000; i++) {
    rb.Reset();
    snprintf(keybuf, sizeof(keybuf), "hello %d", i);
    rb.AddString(Slice(keybuf));
    rb.AddUint32(i);
    rb.AddUint32(0);
    ASSERT_STATUS_OK(tablet_->Insert(rb.data()));
    inserted.insert(i);

    if (i % 300 == 0) {
      LOG(INFO) << "Flushing after " << i << " rows inserted";
      ASSERT_STATUS_OK(tablet_->Flush());
    }
  }
  LOG(INFO) << "Successfully inserted " << inserted.size() << " rows";

  // At this point, we should have several layers as well
  // as some data in memstore.

  // Update a subset of the rows
  ScopedRowDelta update(schema_);
  for (uint32_t i = 0; i < 1000; i += 15) {
    snprintf(keybuf, sizeof(keybuf), "hello %d", i);
    Slice key_slice(keybuf);
    uint32_t new_val = 10000 + i;
    update.get().UpdateColumn(schema_, 1, &new_val);
    ASSERT_STATUS_OK_FAST(
      tablet_->UpdateRow(&key_slice, update.get()));
    inserted.erase(i);
    inserted.insert(new_val);
  }

  // Now iterate the tablet and make sure the rows show up.
  scoped_ptr<Tablet::RowIterator> iter;
  ASSERT_STATUS_OK(tablet_->NewRowIterator(schema_, &iter));
  scoped_array<uint8_t> buf(new uint8_t[schema_.byte_size() * 100]);
  RowBlock block(schema_, &buf[0], 100, &arena_);

  while (iter->HasNext()) {
    arena_.Reset();
    size_t n = 100;
    ASSERT_STATUS_OK(iter->CopyNextRows(&n, &block));
    LOG(INFO) << "Fetched batch of " << n;
    for (size_t i = 0; i < n; i++) {
      const char *row_ptr = reinterpret_cast<const char *>(
        &buf[schema_.byte_size() * i]);
      Slice row_slice(row_ptr, schema_.byte_size());
      uint32_t val_read = *schema_.ExtractColumnFromRow<UINT32>(row_slice, 1);
      bool removed = inserted.erase(val_read);
      ASSERT_TRUE(removed) << "Got value " << val_read << " but either "
                           << "the value was invalid or was already "
                           << "seen once!";
    }
  }

  ASSERT_TRUE(inserted.empty())
    << "expected to see all inserted data through iterator. "
    << inserted.size() << " elements were not seen.";
}

// Test that, when a tablet hsa flushed data and is
// reopened, that the data persists
TEST_F(TestTablet, TestInsertsPersist) {
  InsertTestRows(0, 1000);
  ASSERT_EQ(1000, TabletCount());

  // Flush it.
  ASSERT_STATUS_OK(tablet_->Flush());

  ASSERT_EQ(1000, TabletCount());

  // Close and re-open tablet
  tablet_.reset(new Tablet(schema_, test_dir_));
  ASSERT_STATUS_OK(tablet_->Open());

  // Ensure that rows exist
  VerifyTestRows(0, 1000);
  ASSERT_EQ(1000, TabletCount());

  // TODO: add some more data, re-flush
}

TEST_F(TestTablet, TestCompaction) {
  // Create three layers by inserting and flushing
  InsertTestRows(0, 1000);
  ASSERT_STATUS_OK(tablet_->Flush());

  InsertTestRows(1000, 1000);
  ASSERT_STATUS_OK(tablet_->Flush());

  InsertTestRows(2000, 1000);
  ASSERT_STATUS_OK(tablet_->Flush());
  ASSERT_EQ(3000, TabletCount());

  // Issue compaction
  ASSERT_STATUS_OK(tablet_->Compact());
  ASSERT_EQ(3000, TabletCount());
}

} // namespace tablet
} // namespace kudu


int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  google::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
