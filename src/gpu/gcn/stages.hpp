// The graphics stages of the translator: their initial registers, exports and interpolation
// (docs/design/graphics.md, "Stage interfaces and ShaderKey" and "Pixel shaders"). Internal to
// the translator.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/spirv_builder.hpp"
#include "gpu/gcn/translate.hpp"

namespace bb::gcn::detail {

// What the stage code needs from the translator: its registers and predication.
class StageRegisters {
public:
    using Id = SpirvBuilder::Id;

    virtual Id vgpr(unsigned n) = 0;  // the variable of v<n>, a u32
    virtual Id sgpr(unsigned n) = 0;  // the variable of s<n>, a u32
    virtual Id exec_var() = 0;        // the variable of this lane's EXEC bit, a bool
    virtual Id active() = 0;          // this lane's EXEC bit, loaded
    virtual void write_vgpr(unsigned n, Id value) = 0;  // predicated by EXEC
    // Runs `body` in a selection on `cond`.
    virtual void guarded(Id cond, const std::function<void()>& body) = 0;
    [[noreturn]] virtual void fail(const std::string& message) const = 0;

protected:
    ~StageRegisters() = default;
};

class StageIo {
public:
    using Id = SpirvBuilder::Id;

    StageIo(SpirvBuilder& b, const ShaderKey& key, StageRegisters& regs)
        : b_(b), key_(key), regs_(regs) {}

    // Throws GcnError for the graphics register values that aren't supported.
    static void check_key(const ShaderKey& key);

    spv::ExecutionModel model() const;

    // The initial registers after the user data: the vertex and instance indices of a VS. The key
    // can't express VGT_PRIMITIVEID_EN or a domain shader on the VS stage yet; they would change
    // these registers (docs/design/graphics.md, "Not in the key yet").
    // For a PS: the primitive mask SGPR, the input VGPRs and EXEC (the covered pixels).
    void prologue(unsigned user_sgprs);

    void exp(const Instruction& inst);
    void interpolate(const Instruction& inst);

    // The execution modes, after the function.
    void finish(Id main);

    // The global variables the stage declared, for the entry point.
    const std::vector<Id>& interface() const { return interface_; }

private:
    enum class Interp { flat, smooth, noperspective };
    struct Input {
        Id var;
        Interp interp;
        spv::Decoration where;  // Centroid, Sample, or Max for the pixel center
    };
    struct Barycentric {
        Interp interp;
        spv::Decoration where;
        unsigned component;  // 0 for I, 1 for J
    };

    void pixel_prologue(unsigned user_sgprs);
    void pixel_export(const Instruction& inst);
    Id builtin(spv::BuiltIn which, spv::StorageClass storage, Id type);
    Id output(std::uint32_t location, Id type);
    Id input(std::uint32_t location, Interp interp, spv::Decoration where);
    void store_components(Id var, Id type, const Id (&values)[4]);
    void render_target(const Instruction& inst, unsigned n);
    Id value(const Operand& o);  // a VGPR as a float
    Id half(Id packed, unsigned high, unsigned format, Id type);  // a 16-bit value of a pair
    Id valid();
    Id load(Id type, Id pointer) { return b_.op(spv::Op::OpLoad, type, {pointer}); }
    void store(Id pointer, Id value) { b_.op_void(spv::Op::OpStore, {pointer, value}); }
    Id f32() { return b_.type_float(32); }
    Id u32() { return b_.type_int(32, false); }

    SpirvBuilder& b_;
    const ShaderKey& key_;
    StageRegisters& regs_;
    std::vector<Id> interface_;
    std::map<std::uint32_t, Id> builtins_;   // by BuiltIn
    std::map<std::uint32_t, Id> outputs_;    // by location
    std::map<std::uint32_t, Input> inputs_;  // by location
    std::map<unsigned, Barycentric> barycentrics_;  // by VGPR
    Id valid_ = 0;  // the valid mask of the last export with `vm`, a bool variable
};

}  // namespace bb::gcn::detail
