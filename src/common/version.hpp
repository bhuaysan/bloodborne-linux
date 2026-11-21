// The version of this build, from the release tags (docs/releasing.md). The definition is
// generated at build time by cmake/version.cmake into the bbversion library.
#pragma once

#include <string_view>

namespace bb::common {

// v0.2.0 on a release, v0.2.0-3-gabc1234 three commits after it, -dirty with uncommitted
// changes, dev-<commit> before the first release tag, "unknown" without git.
std::string_view build_version();

}  // namespace bb::common
