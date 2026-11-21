#include "launcher/find_game.hpp"

#include <algorithm>
#include <deque>
#include <set>

namespace bb::launcher {

namespace {

namespace fs = std::filesystem;

bool is_plain_directory(const fs::directory_entry& e) {
    std::error_code ec;
    const fs::file_status st = e.symlink_status(ec);  // a symbolic link is not followed
    return !ec && fs::is_directory(st);
}

bool looks_like_game(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "eboot.bin", ec) &&
           fs::is_regular_file(dir / "sce_sys" / "param.sfo", ec);
}

}  // namespace

std::vector<fs::path> default_search_roots(std::string_view home, std::string_view user) {
    std::vector<fs::path> roots;
    if (!home.empty()) {
        roots.emplace_back(home);
    }
    if (!user.empty()) {
        roots.push_back(fs::path("/run/media") / user);
    }
    roots.emplace_back("/media");
    return roots;
}

std::vector<fs::path> find_in_game_dir(const fs::path& dir, std::string_view eboot_sha256) {
    std::vector<fs::path> found;
    std::error_code ec;
    const fs::path real = fs::canonical(dir, ec);
    if (ec || !fs::is_directory(real, ec)) {
        return found;
    }
    const auto take = [&](const fs::path& folder) {
        std::error_code canon_error;
        const fs::path target = fs::canonical(folder, canon_error);
        if (!canon_error && looks_like_game(target) &&
            common::check_game_folder(target, eboot_sha256).ok() &&
            std::ranges::find(found, target) == found.end()) {
            found.push_back(target);
        }
    };
    take(real);
    if (!found.empty()) {
        return found;  // the dump's own files are in game/
    }
    fs::directory_iterator it(real, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        // One level only, so a symbolic link is followed: a dump linked into game/ counts.
        if (!name.empty() && name[0] != '.' && it->is_directory(ec)) {
            take(it->path());
        }
    }
    std::ranges::sort(found);
    return found;
}

FindResult find_game_folders(const std::vector<fs::path>& roots, const FindOptions& options,
                             const std::atomic<bool>& cancel) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto deadline = start + options.time_limit;
    FindResult result;
    struct Item {
        fs::path dir;
        int depth;
    };
    std::deque<Item> queue;
    std::set<fs::path> seen;
    std::set<fs::path> hits;
    for (const fs::path& root : roots) {
        std::error_code ec;
        const fs::path real = fs::canonical(root, ec);
        if (!ec && fs::is_directory(real, ec) && seen.insert(real).second) {
            queue.push_back({real, 0});
        }
    }
    const auto stop = [&] {
        if (cancel.load(std::memory_order_relaxed)) {
            result.cancelled = true;
        } else if (clock::now() >= deadline) {
            result.timed_out = true;
        }
        return result.cancelled || result.timed_out;
    };
    while (!queue.empty() && !stop()) {
        const Item item = queue.front();
        queue.pop_front();
        if (item.depth > 0 && looks_like_game(item.dir)) {
            if (common::check_game_folder(item.dir, options.eboot_sha256).ok()) {
                hits.insert(item.dir);
            }
            continue;
        }
        if (item.depth >= options.max_depth) {
            continue;
        }
        std::error_code ec;
        fs::directory_iterator it(item.dir, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (stop()) {
                break;
            }
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.' || !is_plain_directory(*it)) {
                continue;
            }
            if (seen.insert(it->path()).second) {
                queue.push_back({it->path(), item.depth + 1});
            }
        }
    }
    result.folders.assign(hits.begin(), hits.end());
    std::stable_sort(result.folders.begin(), result.folders.end(), [](const fs::path& a, const fs::path& b) {
        return std::distance(a.begin(), a.end()) < std::distance(b.begin(), b.end());
    });
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start);
    return result;
}

}  // namespace bb::launcher
