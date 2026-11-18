// The Help with problems page (docs/design/launcher.md): the known problems in words with what
// to do, the one that applies now first, a button that copies a problem report and one that
// opens the log folder.
#include <SDL3/SDL_misc.h>
#include <sys/utsname.h>
#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>

#include "launcher/gpu_probe.hpp"
#include "launcher/help_report.hpp"
#include "launcher/page.hpp"
#include "launcher/widgets.hpp"

#ifndef BB_LAUNCHER_VERSION
#define BB_LAUNCHER_VERSION "unknown"
#endif

namespace bb::launcher {

namespace {

namespace fs = std::filesystem;

struct ProblemText {
    const char* heading;
    const char* what_to_do;
};

// This page's own strings, in both languages.
struct Text {
    const char* title;
    const char* applies;  // marks the problem that applies now
    ProblemText problems[4];  // in the order of enum Problem
    const char* copy;
    const char* copied;
    const char* open_logs;
    const char* intro;
};

constexpr Text english{
    "Help with problems",
    "This is the one now",
    {{"The game folder moved or can't be read",
      "Choose the folder with eboot.bin again: go back and press Set up game. Check that the "
      "drive is plugged in and that you may read the folder."},
     {"Wrong game version",
      "The launcher needs Bloodborne (CUSA03173) with update 1.09 applied. Choose the folder of "
      "the updated game."},
     {"No graphics card or driver for Vulkan",
      "Install your graphics driver (on AMD and Intel the Mesa Vulkan package, on Nvidia the "
      "proprietary driver) and update it, then start the launcher again."},
     {"The game stopped unexpectedly",
      "Press Play to try again. If it keeps happening, copy the problem report below and paste "
      "it into a new issue on the project's page."}},
    "Copy problem report",
    "Copied to the clipboard",
    "Open log folder",
    "Known problems, the one that applies to you first:"};

constexpr Text german{
    "Hilfe bei Problemen",
    "Das trifft gerade zu",
    {{"Der Spielordner wurde verschoben oder ist nicht lesbar",
      "W\xc3\xa4hle den Ordner mit eboot.bin erneut: zur\xc3\xbc" "ck und dann Spiel einrichten. Pr\xc3\xbc" "fe, ob das "
      "Laufwerk angeschlossen ist und du den Ordner lesen darfst."},
     {"Falsche Spielversion",
      "Der Launcher braucht Bloodborne (CUSA03173) mit Update 1.09. W\xc3\xa4hle den Ordner des "
      "aktualisierten Spiels."},
     {"Keine Grafikkarte oder kein Treiber f\xc3\xbc" "r Vulkan",
      "Installiere den Grafiktreiber (bei AMD und Intel das Mesa-Vulkan-Paket, bei Nvidia den "
      "propriet\xc3\xa4" "ren Treiber) und aktualisiere ihn, dann starte den Launcher neu."},
     {"Das Spiel wurde unerwartet beendet",
      "Dr\xc3\xbc" "cke Spielen, um es noch einmal zu versuchen. Tritt es immer wieder auf, kopiere den "
      "Problembericht unten und f\xc3\xbc" "ge ihn in ein neues Issue auf der Projektseite ein."}},
    "Problembericht kopieren",
    "In die Zwischenablage kopiert",
    "Log-Ordner \xc3\xb6" "ffnen",
    "Bekannte Probleme, das zutreffende zuerst:"};

const Text& text(const Context& ctx) {
    return ctx.lang == Lang::de ? german : english;
}

std::string os_pretty_name() {
    std::ifstream in("/etc/os-release");
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("PRETTY_NAME=")) {
            std::string v = line.substr(12);
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
                v = v.substr(1, v.size() - 2);
            }
            return v;
        }
    }
    return "unknown";
}

class HelpPage : public Page {
public:
    const char* title(const Context& ctx) const override { return text(ctx).title; }

    void draw(const Context& ctx, float height) override {
        const Text& t = text(ctx);
        const float s = ctx.scale;
        const float buttons = 2 * 40 * s + 8 * s;
        ProblemInput in;
        in.have_folder = ctx.have_folder;
        in.folder = ctx.folder_check;
        in.gpu_ok = ctx.gpu_ok;
        in.last_run_failed = ctx.last_run_failed;
        ImGui::BeginChild("problems", ImVec2(432 * s, height - buttons - 8 * s));
        ImGui::TextWrapped("%s", t.intro);
        for (const Problem p : order_problems(in)) {
            const ProblemText& pt = t.problems[static_cast<int>(p)];
            ImGui::Spacing();
            const bool now = problem_applies(p, in);
            if (now) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1.0f));
            }
            ImGui::TextWrapped("%s%s%s", pt.heading, now ? " - " : "", now ? t.applies : "");
            if (now) {
                ImGui::PopStyleColor();
            }
            ImGui::TextWrapped("%s", pt.what_to_do);
        }
        ImGui::EndChild();
        ImGui::SetCursorPosX(24 * s);
        if (!focused_) {  // gamepad navigation starts at the first button
            ImGui::SetKeyboardFocusHere();
            ImGui::GetCurrentContext()->NavCursorVisible = true;
            focused_ = true;
        }
        if (bevel_button(copied_ ? t.copied : t.copy, ImVec2(432 * s, 40 * s))) {
            ImGui::SetClipboardText(collect_problem_report(ctx).c_str());
            copied_ = true;
        }
        ImGui::SetCursorPosX(24 * s);
        if (bevel_button(t.open_logs, ImVec2(432 * s, 40 * s))) {
            open_log_folder(ctx);
        }
    }

    static void open_log_folder(const Context& ctx) {
        if (ctx.state_dir.empty()) {
            return;
        }
        std::error_code ec;
        const fs::path dir = ctx.state_dir / "logs";
        fs::create_directories(dir, ec);
        SDL_OpenURL(std::format("file://{}", dir.string()).c_str());
    }

    bool copied_ = false;
    bool focused_ = false;
};

}  // namespace

std::string collect_problem_report(const Context& ctx) {
    ReportInput in;
    in.launcher_version = BB_LAUNCHER_VERSION;
    std::error_code ec;
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    in.bb_run = n > 0 && fs::exists(fs::path(std::string(self, static_cast<std::size_t>(n)))
                                            .parent_path() / "bb-run", ec)
                    ? "found next to the launcher, built with it"
                    : "missing next to the launcher";
    in.distribution = os_pretty_name();
    utsname u{};
    if (uname(&u) == 0) {
        in.kernel = std::format("{} {} {}", u.sysname, u.release, u.machine);
    }
    in.gpu = ctx.gpu_description.empty() ? vulkan_gpu_summary() : ctx.gpu_description;
    if (!ctx.state_dir.empty()) {
        std::ifstream st(last_run_status(ctx.state_dir));
        std::getline(st, in.last_exit);
        in.log = read_tail(last_run_log(ctx.state_dir), 256 * 1024);
    }
    const passwd* pw = getpwuid(getuid());
    const char* home = std::getenv("HOME");
    in.home = home != nullptr ? home : (pw != nullptr ? pw->pw_dir : "");
    in.user = pw != nullptr ? pw->pw_name : "";
    if (const char* user = std::getenv("USER"); user != nullptr && in.user.empty()) {
        in.user = user;
    }
    std::string report = build_report(in);
    // A second user name of the environment (USER differing from the passwd entry) goes too.
    if (const char* user = std::getenv("USER"); user != nullptr && in.user != user) {
        report = scrub(report, "", user);
    }
    return report;
}

std::unique_ptr<Page> make_help_page() {
    return std::make_unique<HelpPage>();
}

}  // namespace bb::launcher
