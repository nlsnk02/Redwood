#include <gtest/gtest.h>
#include <filesystem>
#include <array>
#include <vector>
#include "cbtree/ssd_page_store.hpp"

class SsDPageStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::filesystem::temp_directory_path() / "cbtree_test_ssd.pages";
    store_ = std::make_unique<cbtree::SsDPageStore>(path_.string());
  }
  void TearDown() override {
    store_.reset();
    std::filesystem::remove(path_);
  }
  std::filesystem::path path_;
  std::unique_ptr<cbtree::SsDPageStore> store_;
};

TEST_F(SsDPageStoreTest, AllocWriteRead) {
  auto id = store_->alloc_page();
  std::array<std::byte, cbtree::kPageSize> buf{};
  buf[0] = std::byte{0xAB};
  ASSERT_EQ(store_->write_page(id, buf), cbtree::Status::Ok);
  std::array<std::byte, cbtree::kPageSize> out{};
  ASSERT_EQ(store_->read_page(id, out), cbtree::Status::Ok);
  EXPECT_EQ(out[0], std::byte{0xAB});
}

TEST_F(SsDPageStoreTest, PutGetRecord) {
  auto id = store_->alloc_page();
  ASSERT_EQ(store_->put_record(id, 1, 10), cbtree::Status::Ok);
  ASSERT_EQ(store_->put_record(id, 2, 20), cbtree::Status::Ok);
  auto r1 = store_->get_record(id, 1);
  EXPECT_EQ(r1.status, cbtree::Status::Ok);
  EXPECT_EQ(r1.value, 10u);
  auto r2 = store_->get_record(id, 2);
  EXPECT_EQ(r2.status, cbtree::Status::Ok);
  EXPECT_EQ(r2.value, 20u);
  auto r3 = store_->get_record(id, 99);
  EXPECT_EQ(r3.status, cbtree::Status::NotFound);
}

TEST_F(SsDPageStoreTest, PutRecordUpsert) {
  auto id = store_->alloc_page();
  store_->put_record(id, 1, 10);
  ASSERT_EQ(store_->put_record(id, 1, 99), cbtree::Status::Ok);
  auto r = store_->get_record(id, 1);
  EXPECT_EQ(r.value, 99u);
}

TEST_F(SsDPageStoreTest, DumpSorted) {
  auto id = store_->alloc_page();
  store_->put_record(id, 3, 30);
  store_->put_record(id, 1, 10);
  store_->put_record(id, 2, 20);
  std::vector<std::pair<cbtree::Key, cbtree::Value>> out;
  ASSERT_EQ(store_->dump_sorted(id, &out), cbtree::Status::Ok);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], (std::pair<cbtree::Key, cbtree::Value>{1, 10}));
  EXPECT_EQ(out[1], (std::pair<cbtree::Key, cbtree::Value>{2, 20}));
  EXPECT_EQ(out[2], (std::pair<cbtree::Key, cbtree::Value>{3, 30}));
}

TEST_F(SsDPageStoreTest, SplitPage) {
  auto id = store_->alloc_page();
  for (uint64_t i = 0; i < 10; ++i) {
    store_->put_record(id, i, i);
  }
  cbtree::PageId right_id{0};
  ASSERT_EQ(store_->split_page(id, 5, &right_id), cbtree::Status::Ok);
  EXPECT_NE(right_id, 0u);
  // 左页应含 key < 5
  auto r_left = store_->get_record(id, 3);
  EXPECT_EQ(r_left.status, cbtree::Status::Ok);
  auto r_left_miss = store_->get_record(id, 7);
  EXPECT_EQ(r_left_miss.status, cbtree::Status::NotFound);
  // 右页应含 key >= 5
  auto r_right = store_->get_record(right_id, 7);
  EXPECT_EQ(r_right.status, cbtree::Status::Ok);
  auto r_right_miss = store_->get_record(right_id, 3);
  EXPECT_EQ(r_right_miss.status, cbtree::Status::NotFound);
}
