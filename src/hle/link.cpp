#include "hle/link.hpp"

#include <string>
#include <unordered_set>
#include <vector>

#include "hle/hle.hpp"
#include "loader/relocate.hpp"

namespace bb::hle {

void link(loader::LoadSet& set) {
    std::vector<std::vector<loader::Export>> exports;
    std::unordered_set<std::string> exported;  // import_key(library, nid)
    for (const loader::LoadedModule& m : set.modules) {
        for (const loader::Export& e : exports.emplace_back(loader::exports(m.image))) {
            register_export(e.library, e.nid, e.address);
            exported.insert(import_key(e.library, e.nid));
        }
    }
    // libSceFios2 imports some of libc's functions from libSceLibcInternal. A NID is the hash of
    // the name alone, so the same NID there is the same function.
    for (std::size_t i = 0; i < set.modules.size(); ++i) {
        if (set.modules[i].image.dyn.self.name != "libc") {
            continue;
        }
        for (const loader::Export& e : exports[i]) {
            if (!exported.contains(import_key("libSceLibcInternal", e.nid))) {
                register_export("libSceLibcInternal", e.nid, e.address);
            }
        }
    }
    for (loader::LoadedModule& m : set.modules) {
        loader::relocate(m.image, resolve, m.id);
    }
}

}  // namespace bb::hle
