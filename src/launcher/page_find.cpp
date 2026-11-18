// The first-start search's words and the list for several hits (docs/design/launcher.md, "First
// start"). Not a page of the Help-and-settings kind: the app opens it by itself.
#include <imgui.h>

#include "launcher/page.hpp"
#include "launcher/widgets.hpp"

namespace bb::launcher {

namespace {

constexpr FindText english{
    .title = "Game folders found",
    .hint = "Several copies of Bloodborne were found. Pick the one to play.",
    .searching = "Looking for your game...",
    .found = "Found:",
    .choose_another = "Choose another folder",
};

constexpr FindText german{
    .title = "Gefundene Spielordner",
    .hint = "Es wurden mehrere Kopien von Bloodborne gefunden. W\xc3\xa4hle die zum Spielen.",
    .searching = "Suche nach deinem Spiel...",
    .found = "Gefunden:",
    .choose_another = "Anderen Ordner w\xc3\xa4hlen",
};

class FindPage : public Page {
public:
    FindPage(std::vector<std::filesystem::path> folders,
             std::function<void(const std::filesystem::path&)> chosen)
        : folders_(std::move(folders)), chosen_(std::move(chosen)) {}

    const char* title(const Context& ctx) const override { return find_text(ctx.lang).title; }

    void draw(const Context& ctx, float) override {
        const float s = ctx.scale;
        ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x - 24 * s);
        ImGui::TextUnformatted(find_text(ctx.lang).hint);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8 * s));
        const ImVec2 size(432 * s, 40 * s);
        bool first = true;
        for (const auto& folder : folders_) {
            ImGui::SetCursorPosX(24 * s);
            const std::string label = ellipsize_left(folder.string(), size.x - 16 * s);
            if (bevel_button(label.c_str(), size, false, first && !focused_)) {
                chosen_(folder);
            }
            first = false;
            ImGui::Dummy(ImVec2(0, 4 * s));
        }
        focused_ = true;
    }

private:
    std::vector<std::filesystem::path> folders_;
    std::function<void(const std::filesystem::path&)> chosen_;
    bool focused_ = false;
};

}  // namespace

const FindText& find_text(Lang lang) {
    return lang == Lang::de ? german : english;
}

std::unique_ptr<Page> make_find_page(std::vector<std::filesystem::path> folders,
                                     std::function<void(const std::filesystem::path&)> chosen) {
    return std::make_unique<FindPage>(std::move(folders), std::move(chosen));
}

}  // namespace bb::launcher
