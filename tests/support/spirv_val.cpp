#include "support/spirv_val.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

#include <sys/wait.h>
#include <unistd.h>

namespace bb::test::gcn {

namespace fs = std::filesystem;

bool have_spirv_val() {
    return !std::string_view(BB_SPIRV_VAL).empty();
}

std::string validate(const std::vector<std::uint32_t>& module) {
    // spirv-val reads the module from stdin with "-". Its messages go to a temporary file, since
    // the pipe only goes one way.
    std::string log = (fs::temp_directory_path() / "bb-spirv-val-XXXXXX").string();
    const int fd = ::mkstemp(log.data());
    if (fd < 0) {
        throw std::runtime_error("cannot create " + log);
    }
    ::close(fd);
    const std::string command =
        std::string("'") + BB_SPIRV_VAL + "' --target-env vulkan1.3 - >'" + log + "' 2>&1";
    FILE* pipe = popen(command.c_str(), "w");
    if (pipe == nullptr) {
        fs::remove(log);
        throw std::runtime_error("cannot run " + command);
    }
    std::fwrite(module.data(), sizeof module[0], module.size(), pipe);
    const int status = pclose(pipe);

    std::ifstream in(log);
    std::string out{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    fs::remove(log);
    if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return "";
    }
    return out.empty() ? "spirv-val failed with status " + std::to_string(status) : out;
}

}  // namespace bb::test::gcn
