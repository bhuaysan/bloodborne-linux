#include "launcher/app.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <imgui.h>

#include "common/config.hpp"
#include "common/file.hpp"
#include "common/game_folder.hpp"
#include "launcher/find_game.hpp"
#include "common/runtime_dirs.hpp"
#include "launcher/fonts.hpp"
#include "launcher/gpu_probe.hpp"
#include "launcher/header_image.hpp"
#include "launcher/logic.hpp"
#include "launcher/main_screen.hpp"
#include "launcher/page.hpp"
#include "launcher/run_log.hpp"
#include "launcher/text.hpp"
#include "launcher/widgets.hpp"

extern char** environ;

namespace bb::launcher {

namespace {

namespace fs = std::filesystem;

constexpr int window_width = 480;
constexpr int window_height = 600;

// The result of the folder dialog, which SDL reports from another thread.
struct DialogMailbox {
    std::mutex mutex;
    std::optional<std::string> folder;
    bool open = false;
};

void on_folder_chosen(void* userdata, const char* const* list, int) {
    // The mailbox is shared with the App, so a late callback after the window closed is harmless.
    // SDL calls back exactly once, which frees the reference.
    const std::unique_ptr<std::shared_ptr<DialogMailbox>> holder(
        static_cast<std::shared_ptr<DialogMailbox>*>(userdata));
    DialogMailbox& box = **holder;
    const std::lock_guard lock(box.mutex);
    box.open = false;
    if (list != nullptr && list[0] != nullptr) {
        box.folder = list[0];
    }
}

// The first-start search, which runs on a thread of its own and hands over its result here.
struct SearchBox {
    std::mutex mutex;
    std::atomic<bool> cancel{false};
    std::optional<FindResult> result;
};

fs::path own_directory() {
    try {
        return common::executable_dir();
    } catch (const std::exception&) {
        return ".";
    }
}

ImGuiKey gamepad_key(PadButton b) {
    switch (b) {
    case PadButton::accept: return ImGuiKey_GamepadFaceDown;
    case PadButton::back: return ImGuiKey_GamepadFaceRight;
    case PadButton::up: return ImGuiKey_GamepadDpadUp;
    case PadButton::down: return ImGuiKey_GamepadDpadDown;
    case PadButton::left: return ImGuiKey_GamepadDpadLeft;
    default: return ImGuiKey_GamepadDpadRight;
    }
}

class App {
public:
    App() {
        lang_ = language_from_environment();
        if (const char* path = std::getenv("BB_LAUNCHER_SCRIPT"); path != nullptr && *path) {
            const auto bytes = common::read_file(path);
            script_ = parse_script(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                    bytes.size()));
        }
    }

    int run() {
        init_window();
        load_config();
        gpu_ok_ = vulkan_gpu_usable();
        start_search();
        while (running_) {
            frame();
        }
        close_pages();  // a page left open at the end saves its choices too
        shutdown();
        return 0;
    }

private:
    // ---- setup ------------------------------------------------------------------------------
    void init_window() {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            throw std::runtime_error(std::format("SDL: {}", SDL_GetError()));
        }
        window_ = SDL_CreateWindow("bloodborne-linux", window_width, window_height,
                                   SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window_ == nullptr) {
            throw std::runtime_error(std::format("window: {}", SDL_GetError()));
        }
        // The design's 480 x 600 logical pixels, scaled with the display's content scale.
        float content = SDL_GetWindowDisplayScale(window_);
        const float density = SDL_GetWindowPixelDensity(window_);
        content = content > 0 ? std::min(content, 4.0f) : 1.0f;
        scale_ = content / (density > 0 ? density : 1.0f);
        SDL_SetWindowSize(window_, static_cast<int>(window_width * scale_),
                          static_cast<int>(window_height * scale_));
        SDL_SetWindowResizable(window_, false);
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            throw std::runtime_error(std::format("renderer: {}", SDL_GetError()));
        }
        SDL_SetRenderVSync(renderer_, 1);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // nothing is written outside user/ in the game's folder
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        apply_theme(ImGui::GetStyle());
        ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
        ImGui_ImplSDLRenderer3_Init(renderer_);
        load_fonts(text_size * scale_);
    }

    void shutdown() {
        if (search_thread_.joinable()) {
            search_->cancel = true;
            search_thread_.join();
        }
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        if (header_ != nullptr) {
            SDL_DestroyTexture(header_);
        }
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    void load_config() {
        std::optional<common::RuntimeDirs> dirs;
        try {
            dirs = common::RuntimeDirs::from_environment(common::default_profile, {});
        } catch (const std::exception& e) {
            notice_ = std::string("No place for the game's data: ") + e.what();
            no_place_ = notice_;
            return;
        }
        try {
            dirs->ensure(dirs->root());
            if (::access(dirs->root().c_str(), W_OK) != 0) {
                throw std::runtime_error("read-only");  // an existing user/ without write access
            }
        } catch (const std::exception&) {
            // Copied to /usr/bin, say: everything the game writes belongs in its own folder.
            notice_ = "This folder can't be written. Unpack the game into a folder of its own.";
            no_place_ = notice_;
            return;
        }
        config_path_ = dirs->config() / "config.toml";
        std::error_code ec;
        if (!fs::exists(config_path_, ec)) {
            return;
        }
        try {
            const common::ParsedConfig parsed = common::read_config(config_path_);
            if (parsed.config.game_dir) {
                tried_folder_ = true;
                accept_check(common::check_game_folder(*parsed.config.game_dir),
                             *parsed.config.game_dir);
            }
        } catch (const common::ConfigError& e) {
            notice_ = e.what();
        }
    }

    // ---- the game folder ----------------------------------------------------------------------
    // The first start: look for the dump while no game folder is set up (docs/design/launcher.md).
    void start_search() {
        if (game_ready_ || tried_folder_ || config_path_.empty()) {
            return;
        }
        const char* home = std::getenv("HOME");
        const char* user = std::getenv("USER");
        auto roots = default_search_roots(home != nullptr ? home : "", user != nullptr ? user : "");
        searching_ = true;
        auto box = search_;
        search_thread_ = std::thread([box, roots = std::move(roots), game = own_directory() / "game"] {
            // game/ next to the launcher first: one dump there is taken without the wider search.
            const auto started = std::chrono::steady_clock::now();
            const std::vector<fs::path> local = find_in_game_dir(game);
            FindResult r;
            if (local.size() == 1) {
                r.folders = local;
                r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
            } else {
                r = find_game_folders(roots, FindOptions{}, box->cancel);
                for (auto it = local.rbegin(); it != local.rend(); ++it) {
                    if (std::ranges::find(r.folders, *it) == r.folders.end()) {
                        r.folders.insert(r.folders.begin(), *it);
                    }
                }
            }
            const std::lock_guard lock(box->mutex);
            box->result = std::move(r);
        });
    }

    void poll_search() {
        if (!searching_) {
            return;
        }
        std::optional<FindResult> result;
        {
            const std::lock_guard lock(search_->mutex);
            result = std::move(search_->result);
        }
        if (!result) {
            return;
        }
        search_thread_.join();
        searching_ = false;
        std::cerr << std::format("bb-launcher: game folder search took {} ms, {} hit(s){}\n",
                                 result->elapsed.count(), result->folders.size(),
                                 result->timed_out ? " (time limit)" : "");
        if (game_ready_ || tried_folder_) {
            return;  // the player was faster: a drop or the dialog
        }
        if (result->folders.size() == 1) {
            set_up_folder(result->folders[0]);
        } else if (result->folders.size() > 1) {
            close_pages();  // open pages save their choices first; the list replaces them
            pages_.push_back(
                make_find_page(result->folders, [this](const fs::path& f) { pending_choice_ = f; }));
            focus_first_ = true;
            refocus_ = false;
        }
    }

    // Takes the result of a check of `folder`: a passing folder becomes the game, a failing one
    // becomes the message of the status line.
    void accept_check(const common::FolderResult& r, const fs::path& folder) {
        if (r.ok()) {
            std::error_code ec;
            game_dir_ = fs::canonical(folder, ec);
            title_ = r.title;
            version_ = r.app_version;
            game_ready_ = true;
            notice_.clear();
            if (header_ != nullptr) {
                SDL_DestroyTexture(header_);
            }
            header_ = load_header_texture(renderer_, game_dir_ / "sce_sys" / "pic1.png",
                                          header_width, header_height);
            SDL_SetWindowTitle(window_, std::format("{} (bloodborne-linux)",
                                                    title_.empty() ? "Bloodborne" : title_).c_str());
        } else {
            notice_ = check_message(lang_, r);
        }
    }

    void set_up_folder(const fs::path& folder) {
        tried_folder_ = true;
        if (config_path_.empty()) {  // no user/ folder: say why, not what writing "" fails with
            notice_ = no_place_;
            return;
        }
        const common::SetupResult r = common::setup_game_folder(folder, config_path_);
        if (r.folder.ok() && !r.config_written) {
            notice_ = r.config_error;
            return;
        }
        accept_check(r.folder, folder);
    }

    void open_folder_dialog() {
        {
            // SDL may call back from this thread, so the lock is released before the call.
            const std::lock_guard lock(dialog_->mutex);
            if (dialog_->open) {
                return;
            }
            dialog_->open = true;
        }
        // The callback gets its own reference; SDL calls back once.
        auto* ref = new std::shared_ptr<DialogMailbox>(dialog_);
        // SDL reads the properties during the call, as its own SDL_ShowOpenFolderDialog does.
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window_);
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                              strings(lang_).dialog_title);
        SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, on_folder_chosen, ref, props);
        SDL_DestroyProperties(props);
    }

    // ---- the game ---------------------------------------------------------------------------
    // The runtime's state directory (user/), empty when the game's folder can't be determined.
    fs::path state_dir() const {
        try {
            return common::RuntimeDirs::from_environment(common::default_profile, {}).state();
        } catch (const std::exception&) {
            return {};
        }
    }

    // Creates the log folder and returns the file of the next run's capture, or an empty path.
    fs::path prepare_run_log() const {
        try {
            const auto dirs = common::RuntimeDirs::from_environment(common::default_profile, {});
            dirs.ensure(dirs.logs());
            return last_run_log(dirs.state());
        } catch (const std::exception&) {
            return {};
        }
    }

    void write_run_status(const std::string& text) const {
        const fs::path dir = state_dir();
        if (!dir.empty()) {
            std::ofstream(last_run_status(dir), std::ios::trunc) << text << '\n';
        }
    }

    void play() {
        notice_.clear();
        const fs::path dir = own_directory();
        const auto args = bb_run_arguments(dir / "bb-run", play_config_.empty() ? config_path_
                                                                                : play_config_);
        std::vector<char*> argv;
        for (const std::string& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
        // The output of bb-run goes to a log: it may have no log file of its own when it stops
        // early. No O_CLOEXEC: the log has to stay open as bb-run's fd 1.
        const fs::path log = prepare_run_log();
        if (!log.empty()) {
            posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                             0600);
            posix_spawn_file_actions_adddup2(&actions, 1, 2);
        }
        pid_t pid = 0;
        const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (rc != 0) {
            prompt_ = Prompt::run_failed;
            write_run_status(std::format("could not start bb-run ({})", strerror(rc)));
            return;
        }
        SDL_HideWindow(window_);
        int limit = 0;  // seconds, from a script's waitgame step
        if (step_ < script_.size() && script_[step_].kind == StepKind::waitgame) {
            limit = std::max(1, script_[step_++].number);
        }
        const auto start = std::chrono::steady_clock::now();
        int status = 0;
        bool terminated = false;
        pid_t waited = 0;
        while ((waited = waitpid(pid, &status, WNOHANG)) == 0 ||
               (waited < 0 && errno == EINTR)) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {  // the window is hidden; drop what arrives
            }
            if (limit > 0 && !terminated &&
                std::chrono::steady_clock::now() - start > std::chrono::seconds(limit)) {
                kill(pid, SIGTERM);
                terminated = true;
            }
            SDL_Delay(100);
        }
        // Any result other than the child's own exit with status 0 is a failed run.
        write_run_status(waited == pid ? describe_wait_status(status) : "wait failed");
        if (waited != pid || !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
            prompt_ = Prompt::run_failed;
        }
        SDL_ShowWindow(window_);
        SDL_RaiseWindow(window_);
        focus_first_ = true;
    }

    // ---- one frame --------------------------------------------------------------------------
    void run_script() {
        ImGuiIO& io = ImGui::GetIO();
        if (release_key_ != ImGuiKey_None) {
            io.AddKeyEvent(release_key_, false);
            release_key_ = ImGuiKey_None;
        }
        if (wait_left_ > 0) {
            --wait_left_;
            return;
        }
        if (step_ >= script_.size()) {
            return;
        }
        const ScriptStep step = script_[step_++];
        wait_left_ = 2;  // the frame shows the step's effect before the next step
        switch (step.kind) {
        case StepKind::wait: wait_left_ = step.number; break;
        case StepKind::drop: set_up_folder(step.text); break;
        case StepKind::press:
            release_key_ = gamepad_key(step.button);
            io.AddKeyEvent(release_key_, true);
            break;
        case StepKind::shot: shot_path_ = step.text; break;
        case StepKind::config: play_config_ = step.text; break;
        case StepKind::waitgame: break;  // only counts while the game runs
        case StepKind::nopad: script_pad_ = false; break;
        case StepKind::quit: running_ = false; break;
        }
    }

    // Leaves the page on top; the one under it, or the main screen, shows again.
    void close_page() {
        if (pages_.empty()) {
            return;
        }
        popup_open_ = false;
        pages_.back()->on_close(context());
        pages_.pop_back();
        refocus_ = true;  // the next frame puts the focus on the first entry
    }

    void close_pages() {
        while (!pages_.empty()) {
            close_page();
        }
    }

    // A script decides on its own, whatever is plugged into the machine that runs it.
    bool controller() const {
        return script_.empty() ? SDL_HasGamepad() : script_pad_;
    }

    Context context() const {
        Context ctx;
        ctx.lang = lang_;
        ctx.scale = scale_;
        ctx.text = &strings(lang_);
        ctx.config_path = config_path_;
        ctx.game_ready = game_ready_;
        ctx.game_dir = game_dir_;
        ctx.version = version_;
        ctx.notice = notice_;
        ctx.controller = controller();
        ctx.focus_first = focus_first_;
        return ctx;
    }

    MainView view() const {
        StateInput in;
        in.have_folder = game_ready_ || tried_folder_;
        in.folder_ok = game_ready_;
        MainView v;
        v.state = select_state(in);
        v.header = shows_picture(v.state) ? header_ : nullptr;
        v.focus_first = focus_first_;
        if (v.state == MainState::no_folder && notice_.empty()) {
            v.status = searching_ ? find_text(lang_).searching : strings(lang_).status_no_game;
            v.status_kind = StatusKind::warning;
        } else if (!game_ready_) {
            v.status = notice_;
            v.status_kind = StatusKind::error;
        }
        return v;
    }

    void open(std::unique_ptr<Page> page) {
        pages_.push_back(std::move(page));
        focus_first_ = true;  // a page's popups must keep the focus too
    }

    void act(Action a) {
        switch (a) {
        case Action::set_up:
        case Action::choose_another: open_folder_dialog(); break;
        case Action::play:
            if (prompt_ != Prompt::none) {
                break;  // a prompt came up in the frame of the press; it goes first
            }
            prompt_ = before_play(gpu_ok_, controller());
            if (prompt_ == Prompt::none) {
                play();
            }
            break;
        case Action::play_anyway: play(); break;
        case Action::settings: open(make_settings_page()); break;
        case Action::game_folder: open(make_game_folder_page()); break;
        case Action::picture: open(make_picture_page()); break;
        case Action::advanced: open(make_advanced_page()); break;
        case Action::controller: open(make_controller_page()); break;
        case Action::saves:
            if (game_ready_) {
                open(make_saves_page());
            } else {
                notice_ = strings(lang_).set_up_first;
            }
            break;
        case Action::quit: running_ = false; break;
        case Action::none: break;
        }
    }

    Action draw_page(const Context& ctx) {
        Page& page = *pages_.back();
        const bool back = draw_page_frame(ctx, page.title(ctx));
        ImGui::SetCursorPos(ImVec2(24 * ctx.scale, page_top * ctx.scale));
        const Action a = page.draw(ctx, (page_bottom - page_top) * ctx.scale);
        if (back) {
            close_page();
        }
        return a;
    }

    // The prompt over the window, if one is up. Returns Play when the player plays anyway.
    Action draw_prompt() {
        if (prompt_ == Prompt::none) {
            return Action::none;
        }
        const Strings& t = strings(lang_);
        constexpr const char* id = "##prompt";
        if (!ImGui::IsPopupOpen(id)) {
            ImGui::OpenPopup(id);
        }
        int chosen = -1;
        switch (prompt_) {
        case Prompt::no_controller:
            chosen = prompt(id, t.no_controller_title, t.no_controller_text,
                            {t.play_anyway, t.cancel}, scale_);
            break;
        case Prompt::no_gpu:
            chosen = prompt(id, t.no_gpu_title, t.status_no_gpu, {t.ok}, scale_);
            break;
        case Prompt::run_failed:
            chosen = prompt(id, t.status_last_run_failed, t.run_failed_text, {t.ok}, scale_);
            break;
        case Prompt::notice:
            chosen = prompt(id, t.notice_title, prompted_notice_.c_str(), {t.ok}, scale_);
            break;
        case Prompt::none: break;
        }
        if (chosen < 0) {
            return Action::none;
        }
        const bool anyway = prompt_ == Prompt::no_controller && chosen == 0;
        prompt_ = Prompt::none;
        refocus_ = true;
        return anyway ? Action::play_anyway : Action::none;
    }

    void frame() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running_ = false;
            } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data != nullptr) {
                set_up_folder(e.drop.data);
            }
        }
        {
            const std::lock_guard lock(dialog_->mutex);
            if (dialog_->folder) {
                const std::string folder = *dialog_->folder;
                dialog_->folder.reset();
                set_up_folder(folder);
            }
        }
        poll_search();
        if (pending_choice_) {
            const fs::path chosen = *pending_choice_;
            pending_choice_.reset();
            close_page();
            set_up_folder(chosen);
        }
        // A problem while a game is set up (a refused drop, say) comes up once as a prompt;
        // without a game the main screen and the game folder page show it.
        if (game_ready_ && !notice_.empty() && prompt_ == Prompt::none) {
            prompted_notice_ = std::move(notice_);  // shown now; the same problem again shows again
            notice_.clear();
            prompt_ = Prompt::notice;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        if (!script_.empty()) {
            // Without a real controller ImGui would ignore the scripted gamepad keys.
            ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
            run_script();
        }
        ImGui::NewFrame();
        if (refocus_) {
            focus_first_ = true;
            refocus_ = false;
        }
        const Context ctx = context();
        MainView v = view();
        if (v.state != last_state_) {  // the first button is another one now
            last_state_ = v.state;
            focus_first_ = true;
            v.focus_first = true;
        }
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        if (focus_first_) {
            ImGui::SetNextWindowFocus();  // gamepad navigation needs a focused window
        }
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        Action action = Action::none;
        if (!pages_.empty()) {
            const std::size_t open = pages_.size();
            action = draw_page(ctx);
            // Back closes an open choice list or prompt first, and the page only on the next press.
            const bool popup_was_open = popup_open_;
            popup_open_ = ImGui::IsPopupOpen(ImGuiID(0), ImGuiPopupFlags_AnyPopupId) ||
                          prompt_ != Prompt::none;  // a prompt opens late in its first frame
            // A click on the Back entry in the same frame has closed the page already.
            if (!popup_was_open && !popup_open_ && pages_.size() == open &&
                (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))) {
                close_page();
            }
        } else {
            action = draw_main_screen(ctx, v);
        }
        if (const Action a = draw_prompt(); a != Action::none) {
            action = a;
        }
        ImGui::End();
        if (notice_ != logged_notice_) {  // for the log and the tests: the status line's problems
            logged_notice_ = notice_;
            if (!notice_.empty()) {
                std::cerr << "bb-launcher: " << notice_ << '\n';
            }
        }
        focus_first_ = false;
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer_, 6, 5, 5, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        if (!shot_path_.empty()) {
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr)) {
                SDL_SavePNG(s, shot_path_.c_str());
                SDL_DestroySurface(s);
            }
            shot_path_.clear();
        }
        SDL_RenderPresent(renderer_);
        act(action);
        if (!script_.empty()) {
            SDL_Delay(8);
        }
    }

    Lang lang_ = Lang::en;
    float scale_ = 1.0f;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* header_ = nullptr;
    MainState last_state_ = MainState::ready;
    bool running_ = true;
    bool focus_first_ = true;
    bool refocus_ = false;  // a page was closed in the last frame
    bool popup_open_ = false;  // a choice list of the page was open in the last frame
    fs::path config_path_;
    fs::path play_config_;  // set by a script's config step
    fs::path game_dir_;
    std::string title_;
    std::string version_;
    bool game_ready_ = false;
    bool searching_ = false;
    std::shared_ptr<SearchBox> search_ = std::make_shared<SearchBox>();
    std::thread search_thread_;
    std::optional<fs::path> pending_choice_;  // picked from the list of several hits
    bool tried_folder_ = false;  // a folder was set or chosen, whatever its check said
    bool gpu_ok_ = true;
    Prompt prompt_ = Prompt::none;
    std::string prompted_notice_;  // the text of the notice prompt
    std::string notice_;  // the last problem: in the status line without a game, else a prompt
    std::string logged_notice_;  // the last notice written to stderr
    std::string no_place_;  // why there is no user/ folder (and no config file) this run
    std::vector<std::unique_ptr<Page>> pages_;  // the open pages, the one shown last
    std::shared_ptr<DialogMailbox> dialog_ = std::make_shared<DialogMailbox>();
    std::vector<ScriptStep> script_;
    bool script_pad_ = true;  // a script plays a controller until its nopad step
    std::size_t step_ = 0;
    int wait_left_ = 0;
    ImGuiKey release_key_ = ImGuiKey_None;
    std::string shot_path_;
};

}  // namespace

int run_launcher() {
    // Before SDL and fontconfig start, so their caches land in the game's folder.
    try {
        common::redirect_library_caches(
            common::RuntimeDirs::from_environment(common::default_profile, {}));
    } catch (const std::exception&) {
        // load_config reports it in the status line.
    }
    return App().run();
}

}  // namespace bb::launcher
