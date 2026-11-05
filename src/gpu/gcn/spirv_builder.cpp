// SPIR-V module builder (SPIR-V specification, "Physical Layout of a SPIR-V Module and
// Instruction" and "Logical Layout of a Module").

#include "gpu/gcn/spirv_builder.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <stdexcept>

namespace bb::gcn {

namespace {

using Words = SpirvBuilder::Words;

// Appends one instruction: the word count and opcode, then the operands.
void emit(Words& out, spv::Op opcode, const Words& operands) {
    const std::size_t count = operands.size() + 1;
    if (count > 0xffff) {
        throw std::logic_error(std::format("SPIR-V instruction {} has too many operands",
                                           spirv_word(opcode)));
    }
    out.push_back(static_cast<std::uint32_t>(count) << 16 | spirv_word(opcode));
    out.insert(out.end(), operands.begin(), operands.end());
}

// Appends a literal string: UTF-8, nul-terminated, padded with zeros to a whole word.
void append_string(Words& out, std::string_view text) {
    for (std::size_t i = 0; i <= text.size(); i += 4) {
        std::uint32_t word = 0;
        for (std::size_t j = 0; j < 4 && i + j < text.size(); ++j) {
            word |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + j])) << (8 * j);
        }
        out.push_back(word);
    }
}

bool is_terminator(spv::Op opcode) {
    switch (opcode) {
    case spv::Op::OpBranch:
    case spv::Op::OpBranchConditional:
    case spv::Op::OpSwitch:
    case spv::Op::OpReturn:
    case spv::Op::OpReturnValue:
    case spv::Op::OpKill:
    case spv::Op::OpTerminateInvocation:
    case spv::Op::OpUnreachable:
        return true;
    default:
        return false;
    }
}

}  // namespace

void SpirvBuilder::capability(spv::Capability capability) {
    if (std::ranges::find(declared_capabilities_, spirv_word(capability)) ==
        declared_capabilities_.end()) {
        declared_capabilities_.push_back(spirv_word(capability));
        emit(capabilities_, spv::Op::OpCapability, {spirv_word(capability)});
    }
}

void SpirvBuilder::memory_model(spv::AddressingModel addressing, spv::MemoryModel memory) {
    memory_model_.clear();
    emit(memory_model_, spv::Op::OpMemoryModel, {spirv_word(addressing), spirv_word(memory)});
}

void SpirvBuilder::entry_point(spv::ExecutionModel model, Id function, std::string_view name,
                               const std::vector<Id>& interface) {
    Words operands{spirv_word(model), function};
    append_string(operands, name);
    operands.insert(operands.end(), interface.begin(), interface.end());
    emit(entry_points_, spv::Op::OpEntryPoint, operands);
}

void SpirvBuilder::execution_mode(Id function, spv::ExecutionMode mode, const Words& literals) {
    Words operands{function, spirv_word(mode)};
    operands.insert(operands.end(), literals.begin(), literals.end());
    emit(execution_modes_, spv::Op::OpExecutionMode, operands);
}

void SpirvBuilder::name(Id target, std::string_view name) {
    Words operands{target};
    append_string(operands, name);
    emit(names_, spv::Op::OpName, operands);
}

void SpirvBuilder::decorate(Id target, spv::Decoration decoration, const Words& literals) {
    Words operands{target, spirv_word(decoration)};
    operands.insert(operands.end(), literals.begin(), literals.end());
    emit(decorations_, spv::Op::OpDecorate, operands);
}

SpirvBuilder::Id SpirvBuilder::declare(spv::Op opcode, Id type, const Words& operands) {
    Words key{spirv_word(opcode), type};
    key.insert(key.end(), operands.begin(), operands.end());
    if (auto it = shared_.find(key); it != shared_.end()) {
        return it->second;
    }
    const Id result = id();
    Words all;
    if (type != 0) {
        all.push_back(type);
    }
    all.push_back(result);
    all.insert(all.end(), operands.begin(), operands.end());
    emit(globals_, opcode, all);
    shared_.emplace(std::move(key), result);
    return result;
}

SpirvBuilder::Id SpirvBuilder::type_void() { return declare(spv::Op::OpTypeVoid, 0, {}); }

SpirvBuilder::Id SpirvBuilder::type_bool() { return declare(spv::Op::OpTypeBool, 0, {}); }

SpirvBuilder::Id SpirvBuilder::type_int(std::uint32_t width, bool is_signed) {
    return declare(spv::Op::OpTypeInt, 0, {width, is_signed ? 1u : 0u});
}

SpirvBuilder::Id SpirvBuilder::type_float(std::uint32_t width) {
    return declare(spv::Op::OpTypeFloat, 0, {width});
}

SpirvBuilder::Id SpirvBuilder::type_vector(Id component, std::uint32_t count) {
    return declare(spv::Op::OpTypeVector, 0, {component, count});
}

SpirvBuilder::Id SpirvBuilder::type_pointer(spv::StorageClass storage, Id pointee) {
    return declare(spv::Op::OpTypePointer, 0, {spirv_word(storage), pointee});
}

SpirvBuilder::Id SpirvBuilder::type_function(Id result, const std::vector<Id>& parameters) {
    Words operands{result};
    operands.insert(operands.end(), parameters.begin(), parameters.end());
    return declare(spv::Op::OpTypeFunction, 0, operands);
}

SpirvBuilder::Id SpirvBuilder::constant_bool(bool value) {
    return declare(value ? spv::Op::OpConstantTrue : spv::Op::OpConstantFalse, type_bool(), {});
}

SpirvBuilder::Id SpirvBuilder::constant_u32(std::uint32_t value) {
    return constant(type_int(32, false), {value});
}

SpirvBuilder::Id SpirvBuilder::constant_i32(std::int32_t value) {
    return constant(type_int(32, true), {static_cast<std::uint32_t>(value)});
}

SpirvBuilder::Id SpirvBuilder::constant_f32(float value) {
    return constant(type_float(32), {std::bit_cast<std::uint32_t>(value)});
}

SpirvBuilder::Id SpirvBuilder::constant(Id type, const Words& value) {
    return declare(spv::Op::OpConstant, type, value);
}

SpirvBuilder::Function& SpirvBuilder::function(const char* what) {
    if (!function_) {
        throw std::logic_error(std::format("SPIR-V {} outside a function", what));
    }
    return *function_;
}

SpirvBuilder::Id SpirvBuilder::begin_function(Id result_type, Id function_type,
                                              spv::FunctionControlMask control) {
    if (function_) {
        throw std::logic_error("SPIR-V function started inside another function");
    }
    function_.emplace();
    const Id result = id();
    emit(function_->words, spv::Op::OpFunction,
         {result_type, result, spirv_word(control), function_type});
    return result;
}

SpirvBuilder::Id SpirvBuilder::parameter(Id type) {
    Function& fn = function("parameter");
    if (fn.has_block) {
        throw std::logic_error("SPIR-V parameter after the first block");
    }
    const Id result = id();
    emit(fn.words, spv::Op::OpFunctionParameter, {type, result});
    return result;
}

void SpirvBuilder::begin_block(Id label) {
    Function& fn = function("block");
    if (fn.in_block) {
        throw std::logic_error("SPIR-V block started before the previous one ended");
    }
    emit(fn.words, spv::Op::OpLabel, {label});
    fn.has_block = true;
    fn.in_block = true;
}

void SpirvBuilder::end_function() {
    Function& fn = function("function end");
    if (!fn.has_block || fn.in_block) {
        throw std::logic_error("SPIR-V function ended without a terminated block");
    }
    functions_.insert(functions_.end(), fn.words.begin(), fn.words.end());
    emit(functions_, spv::Op::OpFunctionEnd, {});
    function_.reset();
}

SpirvBuilder::Id SpirvBuilder::op(spv::Op opcode, Id result_type, const Words& operands) {
    Function& fn = function("instruction");
    if (!fn.in_block) {
        throw std::logic_error("SPIR-V instruction outside a block");
    }
    const Id result = id();
    Words all{result_type, result};
    all.insert(all.end(), operands.begin(), operands.end());
    emit(fn.words, opcode, all);
    return result;
}

void SpirvBuilder::op_void(spv::Op opcode, const Words& operands) {
    Function& fn = function("instruction");
    if (!fn.in_block) {
        throw std::logic_error("SPIR-V instruction outside a block");
    }
    emit(fn.words, opcode, operands);
    if (is_terminator(opcode)) {
        fn.in_block = false;
    }
}

Words SpirvBuilder::words() const {
    if (function_) {
        throw std::logic_error("SPIR-V module has an unfinished function");
    }
    if (memory_model_.empty()) {
        throw std::logic_error("SPIR-V module has no memory model");
    }
    // Magic, version 1.6, generator 0 (unregistered), id bound, reserved schema.
    Words out{spv::MagicNumber, 0x00010600, 0, next_id_, 0};
    for (const Words* section :
         {&capabilities_, &memory_model_, &entry_points_, &execution_modes_, &names_,
          &decorations_, &globals_, &functions_}) {
        out.insert(out.end(), section->begin(), section->end());
    }
    return out;
}

}  // namespace bb::gcn
