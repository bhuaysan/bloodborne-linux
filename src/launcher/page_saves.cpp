// The Saves page. An empty stub for now: it only has a heading, and the app draws Back.
#include <imgui.h>

#include "launcher/page.hpp"

namespace bb::launcher {

namespace {

// This page's own strings, in both languages.
struct Text {
    const char* title;
};
constexpr Text english{"Saves"};
constexpr Text german{"Spielst\xc3\xa4nde"};

const Text& text(const Context& ctx) {
    return ctx.lang == Lang::de ? german : english;
}

class SavesPage : public Page {
public:
    const char* title(const Context& ctx) const override { return text(ctx).title; }
    void draw(const Context& ctx, float) override { ImGui::TextUnformatted(title(ctx)); }
};

}  // namespace

std::unique_ptr<Page> make_saves_page() {
    return std::make_unique<SavesPage>();
}

}  // namespace bb::launcher
