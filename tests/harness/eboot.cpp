#include "harness/eboot.hpp"

#include <filesystem>

#include "game/image.hpp"
#include "hle/hle.hpp"
#include "loader/relocate.hpp"
#include "support/game.hpp"

namespace bb::test {

namespace {

loader::Image load(const std::filesystem::path& game) {
    loader::Image image = loader::map_eboot(game, loader::default_base);
    loader::relocate(image, hle::resolve, loader::eboot_module_id);
    image.finalize_protection();
    return image;
}

// The eboot and its harness, from the first test that needs them until the process exits.
struct Eboot {
    explicit Eboot(const std::filesystem::path& game) : image(load(game)), harness(image) {
        game::set_image(&image);
    }
    ~Eboot() { game::set_image(nullptr); }

    loader::Image image;
    harness::Harness harness;
};

}  // namespace

void EbootTest::SetUp() {
    BB_REQUIRE_GAME(game);
    static Eboot eboot(game);
    image_ = &eboot.image;
    harness_ = &eboot.harness;
    harness_->restore();
}

}  // namespace bb::test
