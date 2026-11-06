// A small SPIR-V 1.6 module builder that emits the word stream directly (docs/design/graphics.md,
// "SPIR-V builder"). Enum values come from the Khronos SPIR-V headers.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <spirv/unified1/spirv.hpp11>

namespace bb::gcn {

// The operand word of a SPIR-V enum value, e.g. spirv_word(spv::StorageClass::Function).
template <typename Enum>
    requires std::is_enum_v<Enum>
constexpr std::uint32_t spirv_word(Enum value) {
    return static_cast<std::uint32_t>(value);
}

// Builds one module. Whatever the order of the calls, every instruction lands in its section of
// the SPIR-V logical layout: capabilities, extensions, imports, memory model, entry points,
// execution modes, names, decorations, then types, constants and global variables in the order
// they were declared, then the functions.
//
// Types and constants are shared: asking twice for the same one returns the same id. Arrays,
// runtime arrays and structs are the exception and get a new id on every call, because their
// layout decorations (ArrayStride, Offset, Block) belong to one use.
//
// Misuse (an instruction outside a block, a block that doesn't end in a terminator, a nested
// function) throws std::logic_error. Whether the module is valid is left to spirv-val.
class SpirvBuilder {
public:
    using Id = spv::Id;
    using Words = std::vector<std::uint32_t>;

    // A new result id, e.g. for a label that a branch names before its block starts.
    Id id() { return next_id_++; }

    // Module-level declarations. Capabilities, extensions and imports are recorded once each.
    void capability(spv::Capability capability);
    void extension(std::string_view name);
    // OpExtInstImport of an extended instruction set, e.g. "GLSL.std.450". Returns its id.
    Id import_extended(std::string_view name);
    void memory_model(spv::AddressingModel addressing, spv::MemoryModel memory);
    // `interface` lists every global variable the entry point uses (SPIR-V 1.4 and later).
    void entry_point(spv::ExecutionModel model, Id function, std::string_view name,
                     const std::vector<Id>& interface);
    void execution_mode(Id function, spv::ExecutionMode mode, const Words& literals = {});
    void name(Id target, std::string_view name);
    void decorate(Id target, spv::Decoration decoration, const Words& literals = {});
    void member_decorate(Id structure, std::uint32_t member, spv::Decoration decoration,
                         const Words& literals = {});

    // Types.
    Id type_void();
    Id type_bool();
    Id type_int(std::uint32_t width, bool is_signed);
    Id type_float(std::uint32_t width);
    Id type_vector(Id component, std::uint32_t count);
    Id type_array(Id element, Id length);  // `length` is the id of an integer constant
    Id type_runtime_array(Id element);
    Id type_struct(const std::vector<Id>& members);
    Id type_pointer(spv::StorageClass storage, Id pointee);
    Id type_function(Id result, const std::vector<Id>& parameters = {});
    // OpTypeImage: `depth` and `sampled` are the operands' 0, 1 or 2 (unknown, or known only at
    // run time).
    Id type_image(Id sampled_type, spv::Dim dim, std::uint32_t depth, bool arrayed,
                  bool multisampled, std::uint32_t sampled,
                  spv::ImageFormat format = spv::ImageFormat::Unknown);
    Id type_sampler();
    Id type_sampled_image(Id image);

    // Constants.
    Id constant_bool(bool value);
    Id constant_u32(std::uint32_t value);
    Id constant_i32(std::int32_t value);
    Id constant_f32(float value);
    Id constant(Id type, const Words& value);  // a scalar of any width, low-order word first
    Id constant_composite(Id type, const std::vector<Id>& parts);
    Id constant_null(Id type);

    // A variable outside the functions, in any storage class but Function. `initializer` is a
    // constant id, or 0 for none.
    Id variable(Id pointer_type, spv::StorageClass storage, Id initializer = 0);

    // Functions. Parameters come before the first block. A local variable may be declared
    // anywhere in the function and goes to the start of its first block, where SPIR-V requires it.
    Id begin_function(Id result_type, Id function_type,
                      spv::FunctionControlMask control = spv::FunctionControlMask::MaskNone);
    Id parameter(Id type);
    Id local_variable(Id pointer_type, Id initializer = 0);
    void end_function();

    // Starts the block of `label` (an id from id()). The previous block must have ended in a
    // terminator (OpBranch, OpReturn, …).
    void begin_block(Id label);

    // An instruction inside the current block. The operands follow the result type and result
    // id. `op` returns the result id: `result`, when an earlier instruction (an OpPhi) had to name
    // it, otherwise a new one. `op_void` is for instructions without a result.
    Id op(spv::Op opcode, Id result_type, const Words& operands, Id result = 0);
    void op_void(spv::Op opcode, const Words& operands = {});

    // The finished module. Every function must be ended and the memory model set.
    Words words() const;

private:
    struct Function {
        Words head;       // OpFunction, the parameters and the first OpLabel
        Words variables;  // the local variables, at the start of the first block
        Words body;       // everything after them
        bool has_block = false;
        bool in_block = false;
    };

    // A type or constant: `type` is its result type (0 for types), `operands` follow the result
    // id. Shared declarations are looked up first.
    Id declare(spv::Op opcode, Id type, const Words& operands, bool shared = true);
    Function& function(const char* what);

    Id next_id_ = 1;
    Words capabilities_, extensions_, imports_, memory_model_, entry_points_, execution_modes_,
        names_, decorations_, globals_, functions_;
    std::map<Words, Id> shared_;  // opcode, result type and operands → id
    std::vector<std::uint32_t> declared_capabilities_;
    std::vector<std::string> declared_extensions_;
    std::map<std::string, Id, std::less<>> imports_by_name_;
    std::optional<Function> function_;
};

}  // namespace bb::gcn
