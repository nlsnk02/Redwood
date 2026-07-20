#include <gtest/gtest.h>
#include "cbtree/types.hpp"

TEST(TypesSmoke, Constants) {
  EXPECT_EQ(cbtree::kCacheSlots, 16);
  EXPECT_EQ(cbtree::kPageSize, 4096u);
}
