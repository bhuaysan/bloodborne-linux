// A small SPIR-V 1.6 module builder that emits the word stream directly (docs/design/graphics.md,
// "SPIR-V builder"). Enum values come from the Khronos SPIR-V headers.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
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
// the SPIR-V logical layout: capabilities, memory model, entry points, execution modes, then
// types and constants in the order they were declared, then the functions.
//
// Types and constants are shared: asking twice for the same one returns the same id. So far the
// builder has the scalar, vector, pointer and function types and scalar constants. Names,
// decorations, extensions, variables, aggregates and images follow with the modules that need
// them.
//
// Misuse (an instruction outside a block, a block that doesn't end in a terminator, a nested
// function) throws std::logic_error. Whether the module is valid is left to spirv-val.
class SpirvBuilder {
public:
    using Id = spv::Id;
    using Words = std::vector<std::uint32_t>;

    // A new result id, e.g. for a label that a branch names before its block starts.
    Id id() { return next_id_++; }

    // Module-level declarations. A capability is recorded once.
    void capability(spv::Capability capability);
    void memory_model(spv::AddressingModel addressing, spv::MemoryModel memory);
    // `interface` lists every global variable the entry point uses (SPIR-V 1.4 and later).
    void entry_point(spv::ExecutionModel model, Id function, std::string_view name,
                     const std::vector<Id>& interface);
    void execution_mode(Id function, spv::ExecutionMode mode, const Words& literals = {});

    // Types.
    Id type_void();
    Id type_bool();
    Id type_int(std::uint32_t width, bool is_signed);
    Id type_float(std::uint32_t width);
    Id type_vector(Id component, std::uint32_t count);
    Id type_pointer(spv::StorageClass storage, Id pointee);
    Id type_function(Id result, const std::vector<Id>& parameters = {});

    // Constants.
    Id constant_bool(bool value);
    Id constant_u32(std::uint32_t value);
    Id constant(Id type, const Words& value);  // a scalar of any width, low-order word first

    // Functions. Parameters come before the first block.
    Id begin_function(Id result_type, Id function_type,
                      spv::FunctionControlMask control = spv::FunctionControlMask::MaskNone);
    Id parameter(Id type);
    void end_function();

    // Starts the block of `label` (an id from id()). The previous block must have ended in a
    // terminator (OpBranch, OpReturn, …).
    void begin_block(Id label);

    // An instruction inside the current block. The operands follow the result type and result
    // id. `op` returns the new result id; `op_void` is for instructions without a result.
    Id op(spv::Op opcode, Id result_type, const Words& operands);
    void op_void(spv::Op opcode, const Words& operands = {});

    // The finished module. Every function must be ended and the memory model set.
    Words words() const;

private:
    struct Function {
        Words words;
        bool has_block = false;
        bool in_block = false;
    };

    // A type or constant: `type` is its result type (0 for types), `operands` follow the result
    // id. Returns the id of an earlier identical declaration if there is one.
    Id declare(spv::Op opcode, Id type, const Words& operands);
    Function& function(const char* what);

    Id next_id_ = 1;
    Words capabilities_, memory_model_, entry_points_, execution_modes_, globals_, functions_;
    std::map<Words, Id> shared_;  // opcode, result type and operands → id
    std::vector<std::uint32_t> declared_capabilities_;
    std::optional<Function> function_;
};

}  // namespace bb::gcn
