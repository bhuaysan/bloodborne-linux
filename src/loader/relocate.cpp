#include "loader/relocate.hpp"

#include <sys/mman.h>

#include <cstring>
#include <format>
#include <stdexcept>
#include <string_view>

#include "loader/bytes.hpp"

namespace bb::loader {
namespace {

// Throws LoadError for relocation `r` with `problem`.
[[noreturn]] void fail(const Rela& r, std::string_view problem) {
    throw LoadError(std::format("relocation type {} at {:#x}: {}", r.type, r.offset, problem));
}

// The runtime address of the 8-byte target of `r`. Checks it as relocate() describes.
std::uintptr_t target(const Image& image, const Rela& r) {
    for (const Segment& s : image.segments) {
        if (r.offset >= s.link_addr && in_bounds(r.offset - s.link_addr, 8, s.mem_size)) {
            if (!(s.prot & PROT_WRITE) && !image.dyn.textrel) {
                fail(r, "target in a read-only segment without DT_TEXTREL");
            }
            return image.base + r.offset;
        }
    }
    fail(r, "target outside the PT_LOAD segments");
}

// The value S of the symbol of `r`.
std::uint64_t symbol_value(const Image& image, const Rela& r, const Resolver& resolve) {
    const Symbol& sym = image.dyn.symbols[r.sym];  // index checked by parse_dynamic
    if (sym.shndx != 0) {
        return image.base + sym.value;
    }
    switch (sym.kind) {
    case Symbol::Kind::import: {
        std::uintptr_t addr = resolve(sym.decoded);
        if (addr == 0) {
            fail(r, std::format("import {} resolved to 0", sym.name));
        }
        return addr;
    }
    case Symbol::Kind::plain_weak:
        return 0;  // as for any unresolved weak symbol
    default:
        fail(r, std::format("undefined symbol {} '{}'", r.sym, sym.name));
    }
}

void apply(Image& image, const Rela& r, const Resolver& resolve) {
    if (r.type != r_x86_64_relative && r.type != r_x86_64_64 && r.type != r_x86_64_glob_dat &&
        r.type != r_x86_64_jump_slot) {
        fail(r, "unsupported type");
    }
    std::uintptr_t addr = target(image, r);  // before the resolver sees the import
    std::uint64_t value;
    switch (r.type) {
    case r_x86_64_relative:
        value = image.base + static_cast<std::uint64_t>(r.addend);
        break;
    case r_x86_64_64:
        value = symbol_value(image, r, resolve) + static_cast<std::uint64_t>(r.addend);
        break;
    default:  // R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT
        value = symbol_value(image, r, resolve);
        break;
    }
    std::memcpy(reinterpret_cast<void*>(addr), &value, sizeof value);
}

}  // namespace

void relocate(Image& image, const Resolver& resolve) {
    if (image.finalized()) {
        throw std::logic_error("relocate after finalize_protection");
    }
    for (const Rela& r : image.dyn.relocs) {
        apply(image, r, resolve);
    }
    for (const Rela& r : image.dyn.plt_relocs) {
        apply(image, r, resolve);
    }
}

}  // namespace bb::loader
