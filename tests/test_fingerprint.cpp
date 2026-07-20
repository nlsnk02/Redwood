#include <gtest/gtest.h>
#include "cbtree/fingerprint.hpp"

TEST(Fingerprint, StableForSameKey) {
  EXPECT_EQ(cbtree::fingerprint(42), cbtree::fingerprint(42));
}

TEST(Fingerprint, UsuallyDiffers) {
  EXPECT_NE(cbtree::fingerprint(1), cbtree::fingerprint(2));
}
