// How a failed assertion of the derived translator reaches its caller.
#pragma once

#include <stdexcept>
#include <string_view>

namespace bb::shader {

// Thrown by ASSERT, UNREACHABLE and UNIMPLEMENTED in src/shadps4. what() is the assertion's log
// line, with its file, line and message. In a noexcept function of the derived code (Value::Hash,
// PhiScc::HasCommonOp, TypeOf and UndefOpcode in the SSA pass) the throw ends in std::terminate
// instead, so a caller that must survive every program isolates it. The translator's pools hold partial state afterwards:
// call Pools::ReleaseContents (TranslateProgram does) before using them again.
class TranslationFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace bb::shader

namespace bb::shadps4 {

// Logs `what` as fatal and aborts. For host functions that src/shadps4 links against but that the
// port doesn't provide (yet), so that reaching one is never silent.
[[noreturn]] void host_missing(std::string_view what);

}  // namespace bb::shadps4
