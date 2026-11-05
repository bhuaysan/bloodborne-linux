// A test fixture for differential tests against the game (docs/design/runtime.md, "Process
// setup").
#pragma once

#include <gtest/gtest.h>

#include "harness/harness.hpp"
#include "loader/image.hpp"

namespace bb::test {

// Maps the eboot once per test binary at the default base, relocates it with bb::hle::resolve,
// finalizes its protection and sets it as bb::game::image(). Applies no hooks and runs no init
// code. Keeps one harness on it for the rest of the process, so a binary that uses this fixture
// must not create another harness. Skips the test when the game isn't available.
class EbootTest : public testing::Test {
protected:
    void SetUp() override;

    loader::Image& image() { return *image_; }
    harness::Harness& harness() { return *harness_; }

private:
    loader::Image* image_ = nullptr;
    harness::Harness* harness_ = nullptr;
};

}  // namespace bb::test
