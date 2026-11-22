#include "launcher/data_move.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/config.hpp"
#include "common/runtime_dirs.hpp"
#include "common/sha256.hpp"
#include "launcher/logic.hpp"

namespace bb::launcher {

namespace {

namespace fs = std::filesystem;

struct Failure {
    MoveProblem problem;
    std::string detail;
};

std::string digest(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    common::Sha256 hash;
    std::array<char, 65536> buffer;
    while (in.read(buffer.data(), buffer.size()) || in.gcount() > 0) {
        hash.update(std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(in.gcount()))));
    }
    if (!in.eof()) {
        throw Failure{MoveProblem::verify_failed, file.string() + ": cannot read the file"};
    }
    return common::to_hex(hash.finish());
}

// Flushes a file or folder to disk. A file that can't be flushed fails the move; a folder is best
// effort (some file systems can't sync one).
void flush_path(const fs::path& path, bool is_file) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | (is_file ? 0 : O_DIRECTORY));
    if (fd < 0) {
        if (is_file) {
            throw Failure{MoveProblem::copy_failed, path.string() + ": " + std::strerror(errno)};
        }
        return;
    }
    const bool ok = ::fsync(fd) == 0;
    const int err = errno;
    ::close(fd);
    if (!ok && is_file) {
        throw Failure{MoveProblem::copy_failed, path.string() + ": " + std::strerror(err)};
    }
}

// Same file or folder (device and inode), whatever path leads there.
bool same_object(const fs::path& a, const fs::path& b) {
    struct stat sa;
    struct stat sb;
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev &&
           sa.st_ino == sb.st_ino;
}

// True when `target` or one of its existing parents is `from` or one of its data folders, or
// `from` or one of its parents is `target`: a bind mount or a second mount can lead there under
// another name, which the textual check misses.
bool aliases(const fs::path& from, const fs::path& target) {
    std::vector<fs::path> ours{from};
    for (const char* name : {"cache", "profiles", "logs"}) {
        ours.push_back(from / name);
    }
    for (fs::path p = target; ; p = p.parent_path()) {
        for (const fs::path& o : ours) {
            if (same_object(p, o)) {
                return true;
            }
        }
        if (p == p.root_path() || p.empty()) {
            break;
        }
    }
    for (fs::path p = from; ; p = p.parent_path()) {
        if (same_object(p, target)) {
            return true;
        }
        if (p == p.root_path() || p.empty()) {
            break;
        }
    }
    return false;
}

// Copies one file with its mode and time, flushes it, and checks the copy by size and SHA-256.
void copy_verified(const fs::path& src, const fs::path& dst, const MoveRequest& request) {
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::none, ec);
    if (ec) {
        throw Failure{MoveProblem::copy_failed, src.string() + ": " + ec.message()};
    }
    fs::last_write_time(dst, fs::last_write_time(src, ec), ec);
    flush_path(dst, true);
    if (request.fail_after_copy && request.fail_after_copy(dst)) {
        throw Failure{MoveProblem::copy_failed, dst.string() + ": copy failed (injected)"};
    }
    if (fs::file_size(src, ec) != fs::file_size(dst, ec) || digest(src) != digest(dst)) {
        throw Failure{MoveProblem::verify_failed, dst.string() + ": the copy differs"};
    }
}

void make_directory(const fs::path& dir) {
    if (::mkdir(dir.c_str(), 0700) != 0 && !(errno == EEXIST && fs::is_directory(dir))) {
        throw Failure{MoveProblem::cannot_create, dir.string() + ": " + std::strerror(errno)};
    }
}

void copy_tree(const fs::path& src, const fs::path& dst, const MoveRequest& request) {
    make_directory(dst);
    for (const fs::directory_entry& entry : fs::directory_iterator(src)) {
        const fs::path target = dst / entry.path().filename();
        const fs::file_status status = entry.symlink_status();
        if (fs::is_directory(status)) {
            copy_tree(entry.path(), target, request);
        } else if (fs::is_regular_file(status)) {
            copy_verified(entry.path(), target, request);
        } else {
            throw Failure{MoveProblem::copy_failed,
                          entry.path().string() + ": not a file or folder"};
        }
    }
    flush_path(dst, false);
}

// The config file: copied as it is, except that a game_dir stored relative to the old folder
// (game/<dump> in an AppImage, ../game/<dump> in the tar.gz) becomes the absolute path it names.
void copy_config(const fs::path& src, const fs::path& dst, const MoveRequest& request) {
    copy_verified(src, dst, request);
    common::Config old_config;
    try {
        std::ifstream in(dst, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        const auto raw = common::parse_config(text.str(), dst.string()).config.game_dir;
        if (!raw || !raw->is_relative()) {
            return;
        }
        old_config = common::read_config(src).config;  // resolved against the old folder
    } catch (const common::ConfigError&) {
        return;  // a file bb-run can't read either stays as the player left it
    }
    try {
        common::write_config(dst, old_config);
        // write_config doesn't flush: the rewritten file and its folder go to disk here.
        flush_path(dst, true);
        flush_path(dst.parent_path(), false);
        const auto written = common::read_config(dst).config.game_dir;
        std::error_code ec;
        if (!written || fs::weakly_canonical(*written, ec) !=
                            fs::weakly_canonical(*old_config.game_dir, ec)) {
            throw common::ConfigError("game_dir changed");
        }
    } catch (const common::ConfigError& e) {
        throw Failure{MoveProblem::config_failed, e.what()};
    }
}

bool within(const fs::path& inner, const fs::path& outer) {
    return std::mismatch(outer.begin(), outer.end(), inner.begin(), inner.end()).first ==
           outer.end();
}

// The entries the runtime keeps in a data folder. Anything else in it is the player's.
bool is_runtime_entry(const std::string& name) {
    for (const char* own : {"config.toml", "profiles", "logs", "cache", "game", "data-dir"}) {
        if (name == own) {
            return true;
        }
    }
    return false;
}

bool holds_only_runtime_entries(const fs::path& folder) {
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(folder, ec)) {
        if (!is_runtime_entry(e.path().filename().string())) {
            return false;
        }
    }
    return true;
}

}  // namespace

void remove_old_cache(const fs::path& folder) {
    std::error_code ec;
    if (holds_only_runtime_entries(folder)) {
        fs::remove_all(folder / "cache", ec);
        return;
    }
    // A folder shared with other files (~/Games, say): only what the runtime makes in cache/.
    for (const char* sub : {"system", "pipeline", "profiles"}) {
        fs::remove_all(folder / "cache" / sub, ec);
    }
    fs::remove(folder / "cache", ec);  // only when that left it empty
}

DataPlan plan_data_folder(const PlanInput& in) {
    DataPlan plan;
    const auto config_pointer = [] {
        try {
            return common::data_dir_pointer_path();
        } catch (const std::exception&) {
            return fs::path();
        }
    };
    if (!in.appimage) {
        const fs::path program_file = common::program_data_dir_path(in.exe_dir);
        std::error_code ec;
        const bool writable = ::access(in.exe_dir.c_str(), W_OK | X_OK) == 0;
        // The runtime reads this file before any other, so it is the one to write, and it can only
        // be replaced when the program's folder can be written.
        if (writable || fs::symlink_status(program_file, ec).type() != fs::file_type::not_found ||
            common::default_data_folder_usable(in.exe_dir)) {
            plan.pointer_file = program_file;
            plan.default_folder = in.exe_dir / "user";
            plan.default_removes_pointer = true;
            plan.changeable = writable;
            return plan;
        }
    }
    plan.pointer_file = config_pointer();
    const bool writable = in.appimage && !in.appimage_file.empty() &&
                          ::access(in.appimage_file.parent_path().c_str(), W_OK | X_OK) == 0;
    plan.default_folder = propose_data_folder(in.appimage ? in.appimage_file : fs::path(),
                                              in.home, writable);
    plan.changeable = !plan.pointer_file.empty();
    return plan;
}

MoveResult move_data_folder(const MoveRequest& request) {
    const auto fail = [](MoveProblem p, std::string detail) {
        return MoveResult{p, std::move(detail), {}, {}};
    };
    const fs::path& to_in = request.to;
    if (to_in.empty() || request.pointer_file.empty()) {
        return fail(MoveProblem::no_place, "no folder or no file to keep the choice in");
    }
    if (!to_in.is_absolute()) {
        return fail(MoveProblem::not_absolute, to_in.string());
    }
    std::error_code ec;
    const fs::path from = fs::weakly_canonical(request.from, ec);
    const fs::path to = fs::weakly_canonical(to_in, ec);
    if (from == to) {
        return fail(MoveProblem::same_folder, to.string());
    }
    if (within(to, from) || within(from, to) || aliases(from, to)) {
        return fail(MoveProblem::nested, to.string());
    }
    if (inside_game_folder(to)) {
        return fail(fs::exists(to / "eboot.bin", ec) ? MoveProblem::is_game
                                                     : MoveProblem::inside_game, to.string());
    }
    try {
        common::check_data_folder(to, request.forbidden);
    } catch (const std::exception& e) {
        return fail(MoveProblem::overlaps, e.what());
    }
    if (fs::exists(to, ec)) {
        // Empty, or holding only what an earlier use left: game/ (never overwritten) and cache/.
        bool usable = fs::is_directory(to, ec);
        for (const fs::directory_entry& e : fs::directory_iterator(to, ec)) {
            const std::string name = e.path().filename().string();
            usable = usable && (name == "game" || name == "cache") && e.is_directory(ec);
        }
        if (!usable) {
            return fail(MoveProblem::not_empty, to.string());
        }
    }

    // Everything below can be undone until the pointer is switched.
    std::optional<fs::path> created;  // the topmost folder made here
    std::vector<fs::path> made;       // every folder made here
    const auto undo = [&] {
        if (created) {
            fs::remove_all(*created, ec);
        } else {
            for (const char* name : {"config.toml", "profiles", "logs"}) {  // game/, cache/ stay
                fs::remove_all(to / name, ec);
            }
        }
    };
    try {
        fs::path current;
        for (const fs::path& part : to) {
            current /= part;
            if (!fs::is_directory(current, ec)) {
                make_directory(current);
                made.push_back(current);
                created = created.value_or(current);
            }
        }
        if (::access(to.c_str(), W_OK | X_OK) != 0) {
            throw Failure{MoveProblem::cannot_create, to.string() + ": can't be written"};
        }
        if (fs::exists(from / "config.toml", ec)) {
            copy_config(from / "config.toml", to / "config.toml", request);
        }
        for (const char* name : {"profiles", "logs"}) {
            if (fs::is_directory(from / name, ec)) {
                copy_tree(from / name, to / name, request);
            }
        }
        // The old files go only after the copy is on disk: flush the folders made for it and
        // their parents, then the whole target file system.
        for (const fs::path& dir : made) {
            flush_path(dir, false);
            flush_path(dir.parent_path(), false);
        }
        flush_path(to, false);
        const int fd = ::open(to.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            ::syncfs(fd);  // best effort
            ::close(fd);
        }
    } catch (const Failure& f) {
        undo();
        return fail(f.problem, f.detail);
    } catch (const fs::filesystem_error& e) {
        undo();
        return fail(MoveProblem::copy_failed, e.what());
    }

    try {
        if (request.remove_pointer) {
            if (!fs::remove(request.pointer_file, ec) && ec) {
                throw std::runtime_error(request.pointer_file.string() + ": " + ec.message());
            }
            flush_path(request.pointer_file.parent_path(), false);  // durable before the deletes
        } else {
            common::write_data_dir_pointer(fs::canonical(to), request.pointer_file);
        }
    } catch (const std::exception& e) {
        undo();
        return fail(MoveProblem::pointer_failed, e.what());
    }

    MoveResult result;
    for (const char* name : {"config.toml", "profiles", "logs"}) {
        fs::remove_all(from / name, ec);
        if (ec) {
            result.leftovers.push_back(from / name);
        }
    }
    remove_old_cache(from);
    for (const fs::directory_entry& e : fs::directory_iterator(from, ec)) {
        if (!is_runtime_entry(e.path().filename().string())) {
            result.kept.push_back(e.path());  // not ours: left alone
        }
    }
    if (fs::is_empty(from, ec)) {
        fs::remove(from, ec);
    }
    return result;
}

}  // namespace bb::launcher
