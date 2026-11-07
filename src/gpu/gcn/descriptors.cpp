// The descriptor source pass: a forward dataflow over the SGPRs. Each SGPR dword holds either
// nothing known or a path (see ResourceSource): the user-data slot it started as, then the
// offsets of the loads that produced it. Only s_mov and s_load_* keep a path; every other write
// forgets the dwords it writes. At a join the paths that agree stay.

#include "gpu/gcn/descriptors.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string_view>

namespace bb::gcn {

namespace {

using Path = std::vector<std::uint32_t>;
using State = std::map<unsigned, Path>;  // SGPR -> path of its dword

// The path of the dword pair at `sgpr` when it is a pointer: two consecutive dwords of one load.
std::optional<Path> pointer(const State& state, unsigned sgpr) {
    const auto lo = state.find(sgpr), hi = state.find(sgpr + 1);
    if (lo == state.end() || hi == state.end() || lo->second.size() != hi->second.size() ||
        !std::equal(lo->second.begin(), lo->second.end() - 1, hi->second.begin()) ||
        lo->second.back() + 1 != hi->second.back()) {
        return std::nullopt;
    }
    return lo->second;
}

State transfer(State state, const Instruction& inst) {
    const std::string_view name = inst.name;
    std::map<unsigned, Path> written;  // what the instruction sets
    if (name.starts_with("s_load_dword") && inst.src[1].kind == OperandKind::offset) {
        if (const auto base = pointer(state, inst.src[0].code)) {
            for (unsigned d = 0; d < inst.dst[0].dwords; ++d) {
                written[inst.dst[0].code + d] = *base;
                written[inst.dst[0].code + d].push_back(inst.src[1].value + d);
            }
        }
    } else if ((name == "s_mov_b32" || name == "s_mov_b64") &&
               inst.src[0].kind == OperandKind::scalar) {
        for (unsigned d = 0; d < inst.dst[0].dwords; ++d) {
            if (const auto from = state.find(inst.src[0].code + d); from != state.end()) {
                written[inst.dst[0].code + d] = from->second;
            }
        }
    }
    for (unsigned i = 0; i < inst.num_dst; ++i) {
        if (inst.dst[i].kind == OperandKind::scalar) {
            for (unsigned d = 0; d < inst.dst[i].dwords; ++d) {
                state.erase(inst.dst[i].code + d);
            }
        }
    }
    for (auto& [sgpr, path] : written) {
        state[sgpr] = std::move(path);
    }
    return state;
}

// Keeps the entries that both states have.
bool intersect(State& into, const State& other) {
    bool changed = false;
    for (auto it = into.begin(); it != into.end();) {
        const auto o = other.find(it->first);
        if (o == other.end() || o->second != it->second) {
            it = into.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

}  // namespace

const Operand* buffer_descriptor(const Instruction& inst) {
    const bool buffer = inst.format == Format::mubuf || inst.format == Format::mtbuf;
    if (buffer && inst.num_src >= 3) {
        return &inst.src[inst.num_src - 2];  // the descriptor and soffset come last
    }
    if (inst.format == Format::smrd && std::string_view(inst.name).starts_with("s_buffer_load")) {
        return &inst.src[0];
    }
    return nullptr;
}

namespace {

// Runs the dataflow to its fixed point, then calls `visit(state, index)` for each instruction of
// the blocks that a path reaches, with the state before it, in block order.
template <class Visit>
void walk(std::span<const Instruction> program, const Cfg& cfg, unsigned user_sgprs,
          Visit&& visit) {
    State entry;
    for (unsigned k = 0; k < user_sgprs; ++k) {
        entry[k] = {k};
    }
    // The state at the start of each block. A block that no path has reached yet has none.
    std::vector<std::optional<State>> in(cfg.blocks.size());
    in[0] = entry;
    const auto run_block = [&](std::uint32_t b, auto&& each) {
        State state = *in[b];
        const Block& block = cfg.blocks[b];
        for (std::uint32_t i = block.first; i < block.first + block.count; ++i) {
            each(state, i);
            state = transfer(std::move(state), program[i]);
        }
        return state;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t b = 0; b < cfg.blocks.size(); ++b) {
            if (!in[b]) {
                continue;
            }
            const State out = run_block(b, [](const State&, std::uint32_t) {});
            for (std::uint32_t s : successors(cfg.blocks[b])) {
                if (!in[s]) {
                    in[s] = out;
                    changed = true;
                } else {
                    changed |= intersect(*in[s], out);
                }
            }
        }
    }
    for (std::uint32_t b = 0; b < cfg.blocks.size(); ++b) {
        if (in[b]) {
            run_block(b, visit);
        }
    }
}

// The source of the descriptor in the `dwords` SGPRs from `sgpr`: consecutive dwords of one
// source, named by the first dword's path.
std::optional<ResourceSource> source_of(const State& state, unsigned sgpr, unsigned dwords) {
    const auto first = state.find(sgpr);
    if (first == state.end()) {
        return std::nullopt;
    }
    const Path& path = first->second;
    for (unsigned k = 1; k < dwords; ++k) {
        const auto next = state.find(sgpr + k);
        if (next == state.end() || next->second.size() != path.size() ||
            !std::equal(path.begin(), path.end() - 1, next->second.begin()) ||
            next->second.back() != path.back() + k) {
            return std::nullopt;
        }
    }
    return ResourceSource{path};
}

// The position of `source` in `list`, added at the end when it is new.
std::uint32_t position(std::vector<ResourceSource>& list, const ResourceSource& source) {
    const auto at = static_cast<std::uint32_t>(std::ranges::find(list, source) - list.begin());
    if (at == list.size()) {
        list.push_back(source);
    }
    return at;
}

}  // namespace

std::vector<ResourceSource> find_buffer_sources(std::span<const Instruction> program,
                                                const Cfg& cfg, unsigned user_sgprs) {
    std::vector<ResourceSource> sources;
    walk(program, cfg, user_sgprs, [&](const State& state, std::uint32_t i) {
        if (const Operand* d = buffer_descriptor(program[i])) {
            if (const auto source = source_of(state, d->code, 4)) {
                position(sources, *source);
            }
        }
    });
    return sources;
}

ImageSources find_image_sources(std::span<const Instruction> program, const Cfg& cfg,
                                unsigned user_sgprs) {
    ImageSources result;
    walk(program, cfg, user_sgprs, [&](const State& state, std::uint32_t i) {
        const Instruction& inst = program[i];
        if (inst.format != Format::mimg) {
            return;
        }
        // The T# is the first 8-SGPR operand (r128 uses its first 4), the S# the one after it.
        const auto* t = std::ranges::find_if(inst.src.begin(), inst.src.begin() + inst.num_src,
                                             [](const Operand& o) { return o.dwords == 8; });
        const auto image = source_of(state, t->code, inst.flags & flag::r128 ? 4 : 8);
        const bool sampled = t + 1 < inst.src.begin() + inst.num_src;
        const auto sampler = sampled ? source_of(state, t[1].code, 4) : std::nullopt;
        if (!image || (sampled && !sampler)) {
            return;
        }
        ImageSources::Use use{.image = position(result.images, *image)};
        if (sampler) {
            use.sampler = position(result.samplers, *sampler);
        }
        result.uses[i] = use;
    });
    return result;
}

}  // namespace bb::gcn
