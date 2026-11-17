// bb-launcher: sets up the game and starts it (docs/design/launcher.md).
#include <exception>
#include <iostream>

#include "launcher/app.hpp"

int main() {
    try {
        return bb::launcher::run_launcher();
    } catch (const std::exception& e) {
        std::cerr << "bb-launcher: " << e.what() << '\n';
        return 1;
    }
}
