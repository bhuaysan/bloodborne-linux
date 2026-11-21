// bb-launcher: sets up the game and starts it (docs/design/launcher.md).
#include <algorithm>
#include <exception>
#include <iostream>
#include <string_view>

#include "common/version.hpp"
#include "launcher/app.hpp"

int main(int argc, char** argv) {
    if (std::find(argv + 1, argv + argc, std::string_view("--version")) != argv + argc) {
        std::cout << "bb-launcher " << bb::common::build_version() << '\n';
        return 0;
    }
    try {
        return bb::launcher::run_launcher();
    } catch (const std::exception& e) {
        std::cerr << "bb-launcher: " << e.what() << '\n';
        return 1;
    }
}
