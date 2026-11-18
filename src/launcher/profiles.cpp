#include "launcher/profiles.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>

#include "common/runtime_dirs.hpp"

namespace bb::launcher {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t max_name_length = 32;

// The newest modification time of a regular file in `dir` and below, at most a few levels deep.
// Folders don't count: a profile that was just made or renamed has not been played. Nothing when
// there is no file.
std::optional<std::time_t> newest_change(const fs::path& dir) {
    std::optional<fs::file_time_type> newest;
    std::error_code ec;
    constexpr int max_depth = 8;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() >= max_depth) {
            it.disable_recursion_pending();
        }
        std::error_code tec;
        if (it->symlink_status(tec).type() != fs::file_type::regular || tec) {  // no symlinks
            continue;
        }
        const auto t = fs::last_write_time(it->path(), tec);
        if (!tec && (!newest || t > *newest)) {
            newest = t;
        }
    }
    if (!newest) {
        return std::nullopt;
    }
    return std::chrono::system_clock::to_time_t(std::chrono::file_clock::to_sys(*newest));
}

}  // namespace

NameProblem check_profile_name(std::string_view name, const fs::path* data_dir) {
    if (name.empty()) {
        return NameProblem::empty;
    }
    if (name.size() > max_name_length) {
        return NameProblem::too_long;
    }
    if (!common::valid_profile_name(name)) {
        return NameProblem::bad_character;
    }
    if (data_dir != nullptr) {
        std::error_code ec;
        if (fs::exists(profile_folder(*data_dir, name), ec)) {
            return NameProblem::exists;
        }
    }
    return NameProblem::ok;
}

fs::path profile_folder(const fs::path& data_dir, std::string_view name) {
    return data_dir / "profiles" / std::string(name);
}

std::vector<ProfileInfo> list_profiles(const fs::path& data_dir, std::string_view current) {
    std::vector<ProfileInfo> out;
    std::error_code ec;
    for (fs::directory_iterator it(data_dir / "profiles", ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code tec;
        const std::string name = it->path().filename().string();
        if (it->is_directory(tec) && !it->is_symlink(tec) && common::valid_profile_name(name)) {
            out.push_back({name, newest_change(it->path())});
        }
    }
    if (!current.empty() && common::valid_profile_name(current) &&
        std::none_of(out.begin(), out.end(), [&](const ProfileInfo& p) { return p.name == current; })) {
        out.push_back({std::string(current), std::nullopt});
    }
    std::sort(out.begin(), out.end(), [](const ProfileInfo& a, const ProfileInfo& b) {
        if (a.last_played != b.last_played) {
            return a.last_played.value_or(0) > b.last_played.value_or(0);
        }
        return a.name < b.name;
    });
    return out;
}

NameProblem create_profile(const fs::path& data_dir, std::string_view name) {
    if (const NameProblem p = check_profile_name(name, &data_dir); p != NameProblem::ok) {
        return p;
    }
    const fs::path folder = profile_folder(data_dir, name);
    fs::create_directories(folder);
    fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace);
    return NameProblem::ok;
}

NameProblem rename_profile(const fs::path& data_dir, const fs::path& cache_dir,
                           std::string_view from, std::string_view to) {
    if (!common::valid_profile_name(from)) {
        return NameProblem::bad_character;
    }
    if (const NameProblem p = check_profile_name(to, &data_dir); p != NameProblem::ok) {
        return p;
    }
    std::error_code ec;
    if (fs::exists(profile_folder(data_dir, from), ec)) {
        fs::rename(profile_folder(data_dir, from), profile_folder(data_dir, to));
    }
    if (!cache_dir.empty() && fs::exists(profile_folder(cache_dir, from), ec) &&
        !fs::exists(profile_folder(cache_dir, to), ec)) {
        fs::rename(profile_folder(cache_dir, from), profile_folder(cache_dir, to), ec);
    }
    return NameProblem::ok;
}

std::string file_url(const fs::path& folder) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string url = "file://";
    for (const unsigned char c : folder.string()) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
                           c == '.' || c == '~';
        if (plain) {
            url += static_cast<char>(c);
        } else {
            url += '%';
            url += hex[c >> 4];
            url += hex[c & 15];
        }
    }
    return url;
}

}  // namespace bb::launcher
