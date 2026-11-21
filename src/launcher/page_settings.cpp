// Settings and the two small pages under it, Set up game and Controller (docs/design/launcher.md,
// "Settings"). Picture and sound and Advanced have their own files.
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "launcher/page.hpp"
#include "launcher/widgets.hpp"

namespace bb::launcher {

namespace {

struct Text {
    const char* found_in;
    const char* no_game;
    const char* choose;
    const char* connected;
    const char* no_controller;
    const char* plug_in;
};

constexpr Text english{
    .found_in = "Found in",
    .no_game = "Drag your game folder into this window,\nthe one with eboot.bin, or choose it.",
    .choose = "Choose folder",
    .connected = "Connected",
    .no_controller = "No controller connected.",
    .plug_in = "Plug one in by USB or connect it\nover Bluetooth.",
};

constexpr Text german{
    .found_in = "Gefunden in",
    .no_game = "Ziehe den Spielordner in dieses Fenster,\nden mit der eboot.bin, oder w\xc3\xa4hle ihn aus.",
    .choose = "Ordner w\xc3\xa4hlen",
    .connected = "Verbunden",
    .no_controller = "Kein Controller verbunden.",
    .plug_in = "Schlie\xc3\x9f" "e einen per USB an oder verbinde\nihn \xc3\xbc" "ber Bluetooth.",
};

const Text& text(const Context& ctx) {
    return ctx.lang == Lang::de ? german : english;
}

constexpr float entry_w = 432;
constexpr float entry_h = 46;

// One entry across the page at the cursor's y; the cursor moves below it.
bool entry(const Context& ctx, const char* label, bool focus_now) {
    const float s = ctx.scale;
    ImGui::SetCursorPosX(24 * s);
    return menu_entry(label, ImVec2(entry_w * s, entry_h * s), entry_size * s, focus_now);
}

float body(const char* lines, float y, ImU32 colour, const Context& ctx) {
    return centred_text(lines, y, colour, entry_w * ctx.scale, text_size * ctx.scale);
}

class SettingsPage : public Page {
public:
    const char* title(const Context& ctx) const override { return ctx.text->settings; }

    Action draw(const Context& ctx, float) override {
        const Strings& t = *ctx.text;
        Action a = Action::none;
        a = entry(ctx, t.set_up, ctx.focus_first) ? Action::game_folder : a;
        a = entry(ctx, t.picture_sound, false) ? Action::picture : a;
        a = entry(ctx, t.advanced, false) ? Action::advanced : a;
        a = entry(ctx, t.controller, false) ? Action::controller : a;
        return a;
    }
};

class GameFolderPage : public Page {
public:
    const char* title(const Context& ctx) const override { return ctx.text->set_up; }

    Action draw(const Context& ctx, float) override {
        const Text& t = text(ctx);
        const float s = ctx.scale;
        float y = ImGui::GetCursorPosY();
        if (ctx.game_ready) {
            y = body(t.found_in, y, colour::dim, ctx);
            const std::string folder = ellipsize_left(ctx.game_dir.string(), entry_w * s);
            y = body(folder.c_str(), y, colour::text, ctx);
            const std::string game =
                std::string("Bloodborne \xc2\xb7 ") + ctx.text->status_version + " " +
                display_version(ctx.version);
            y = body(game.c_str(), y + 12 * s, colour::text, ctx);
        } else {
            y = body(t.no_game, y, colour::dim, ctx);
        }
        if (!ctx.notice.empty()) {
            y = body(ctx.notice.c_str(), y + 12 * s, colour::warning, ctx);
        }
        ImGui::SetCursorPosY(y + 20 * s);
        const char* label = ctx.game_ready ? find_text(ctx.lang).choose_another : t.choose;
        if (entry(ctx, label, ctx.focus_first)) {
            return ctx.game_ready ? Action::choose_another : Action::set_up;
        }
        return Action::none;
    }
};

class ControllerPage : public Page {
public:
    const char* title(const Context& ctx) const override { return ctx.text->controller; }

    Action draw(const Context& ctx, float) override {
        const Text& t = text(ctx);
        float y = ImGui::GetCursorPosY() + 12 * ctx.scale;
        if (!ctx.controller) {
            y = body(t.no_controller, y, colour::warning, ctx);
            body(t.plug_in, y + 12 * ctx.scale, colour::dim, ctx);
            return Action::none;
        }
        y = body(t.connected, y, colour::dim, ctx);
        int count = 0;
        if (SDL_JoystickID* pads = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i) {
                const char* name = SDL_GetGamepadNameForID(pads[i]);
                y = body(name != nullptr ? name : "?", y, colour::text, ctx);
            }
            SDL_free(pads);
        }
        return Action::none;
    }
};

}  // namespace

std::unique_ptr<Page> make_settings_page() {
    return std::make_unique<SettingsPage>();
}

std::unique_ptr<Page> make_game_folder_page() {
    return std::make_unique<GameFolderPage>();
}

std::unique_ptr<Page> make_controller_page() {
    return std::make_unique<ControllerPage>();
}

}  // namespace bb::launcher
