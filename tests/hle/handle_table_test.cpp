#include "hle/handle_table.hpp"

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

namespace {

using bb::hle::HandleTable;

TEST(HandleTable, HandlesStartAtOneAndAreNeverReused) {
    HandleTable<int> table;
    EXPECT_EQ(table.insert(std::make_shared<int>(10)), 1U);
    EXPECT_EQ(table.insert(std::make_shared<int>(20)), 2U);
    ASSERT_TRUE(table.take(2));
    EXPECT_EQ(table.insert(std::make_shared<int>(30)), 3U);
    EXPECT_EQ(*table.find(1), 10);
    EXPECT_EQ(*table.find(3), 30);
}

TEST(HandleTable, UnknownAndTakenHandlesFindNothing) {
    HandleTable<int> table;
    EXPECT_EQ(table.find(0), nullptr);
    EXPECT_EQ(table.find(1), nullptr);
    EXPECT_EQ(table.take(1), nullptr);
    const std::uint64_t handle = table.insert(std::make_shared<int>(7));
    const std::shared_ptr<int> taken = table.take(handle);
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(*taken, 7);
    EXPECT_EQ(table.find(handle), nullptr);
    EXPECT_EQ(table.take(handle), nullptr);
}

TEST(HandleTable, AFoundObjectOutlivesItsRemoval) {
    HandleTable<int> table;
    const std::uint64_t handle = table.insert(std::make_shared<int>(42));
    const std::shared_ptr<int> found = table.find(handle);
    table.take(handle);  // the returned reference is dropped at once
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, 42);
    EXPECT_EQ(found.use_count(), 1);
}

}  // namespace
