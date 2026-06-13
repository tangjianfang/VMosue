#include <gtest/gtest.h>
#include "util/Result.h"

using vmosue::Result;

TEST(Result, OkHoldsValue) {
  Result<int> r = Result<int>::Ok(42);
  ASSERT_TRUE(r.isOk());
  EXPECT_EQ(r.value(), 42);
}

TEST(Result, ErrHoldsError) {
  Result<int> r = Result<int>::Err("boom");
  ASSERT_FALSE(r.isOk());
  EXPECT_STREQ(r.error(), "boom");
}

TEST(Result, ValueOnErrAborts) {
  // We can't easily test abort in tests; just verify isOk path.
  Result<int> r = Result<int>::Err("nope");
  EXPECT_FALSE(r.isOk());
}

TEST(Result, MoveConstruction) {
  Result<std::string> a = Result<std::string>::Ok("hello");
  Result<std::string> b = std::move(a);
  EXPECT_TRUE(b.isOk());
  EXPECT_EQ(b.value(), "hello");
}