// A test fixture for differential tests against the game with its bundled PRXs loaded and
// initialised, so eboot functions can allocate (docs/design/runtime.md, "Process setup").
#pragma once

#include <gtest/gtest.h>

#include "harness/harness.hpp"
#include "loader/image.hpp"
#include "loader/load_set.hpp"

namespace bb::test {

// Once per test binary, on the thread of the first test that uses it: maps the load set at the
// default base, links it (bb::hle::link), finalizes its protection, sets the eboot as
// bb::game::image() and runs the PRX init (bb::hle::run_prx_init). Then maps the game's heap
// with one malloc and its free, creates the thread's TLS blocks of both PRXs, and takes the
// baseline of a harness on the eboot that also covers the PRXs' writable segments, the guest
// mappings, the thread's TLS blocks and its guest errno. Applies no hooks and runs no eboot init
// code. The load set and the harness are never destroyed, so LeakSanitizer can scan the game's
// memory at exit. A binary that uses this fixture must not create another harness, nor use
// EbootTest. Skips the test when the game isn't available; fails it on any other thread than the
// first one, whose TLS the baseline holds.
class LoadSetTest : public testing::Test {
protected:
    void SetUp() override;

    loader::LoadSet& load_set() { return *set_; }
    loader::Image& image() { return set_->eboot().image; }
    harness::Harness& harness() { return *harness_; }

private:
    loader::LoadSet* set_ = nullptr;
    harness::Harness* harness_ = nullptr;
};

}  // namespace bb::test
