#include "hle/handle_table.hpp"

#include <memory>

#include <gtest/gtest.h>

namespace {

// Handles start at 1 and are never reused; a found object outlives its removal.
TEST(HandleTable, HandlesAndLifetimes) {
    bb::hle::HandleTable<int> table;
    EXPECT_EQ(table.take(1), nullptr);
    EXPECT_EQ(table.insert(std::make_shared<int>(10)), 1U);
    EXPECT_EQ(table.insert(std::make_shared<int>(20)), 2U);
    const std::shared_ptr<int> found = table.find(2);
    EXPECT_EQ(*table.take(2), 20);
    EXPECT_EQ(table.find(2), nullptr);
    EXPECT_EQ(found.use_count(), 1);
    EXPECT_EQ(*found, 20);
    EXPECT_EQ(table.insert(std::make_shared<int>(30)), 3U);
    EXPECT_EQ(*table.find(1), 10);
}

}  // namespace
