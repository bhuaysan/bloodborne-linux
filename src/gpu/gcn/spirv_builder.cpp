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

void SpirvBuilder::extension(std::string_view name) {
    if (std::ranges::find(declared_extensions_, name) == declared_extensions_.end()) {
        declared_extensions_.emplace_back(name);
        Words operands;
        append_string(operands, name);
        emit(extensions_, spv::Op::OpExtension, operands);
    }
}

SpirvBuilder::Id SpirvBuilder::import_extended(std::string_view name) {
    if (auto it = imports_by_name_.find(name); it != imports_by_name_.end()) {
        return it->second;
    }
    const Id result = id();
    imports_by_name_.emplace(name, result);
    Words operands{result};
    append_string(operands, name);
    emit(imports_, spv::Op::OpExtInstImport, operands);
    return result;
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

void SpirvBuilder::member_decorate(Id structure, std::uint32_t member, spv::Decoration decoration,
                                   const Words& literals) {
    Words operands{structure, member, spirv_word(decoration)};
    operands.insert(operands.end(), literals.begin(), literals.end());
    emit(decorations_, spv::Op::OpMemberDecorate, operands);
}

SpirvBuilder::Id SpirvBuilder::declare(spv::Op opcode, Id type, const Words& operands,
                                       bool shared) {
    Words key;
    if (shared) {
        key = {spirv_word(opcode), type};
        key.insert(key.end(), operands.begin(), operands.end());
        if (auto it = shared_.find(key); it != shared_.end()) {
            return it->second;
        }
    }
    const Id result = id();
    Words all;
    if (type != 0) {
        all.push_back(type);
    }
    all.push_back(result);
    all.insert(all.end(), operands.begin(), operands.end());
    emit(globals_, opcode, all);
    if (shared) {
        shared_.emplace(std::move(key), result);
    }
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

SpirvBuilder::Id SpirvBuilder::type_array(Id element, Id length) {
    return declare(spv::Op::OpTypeArray, 0, {element, length}, false);
}

SpirvBuilder::Id SpirvBuilder::type_runtime_array(Id element) {
    return declare(spv::Op::OpTypeRuntimeArray, 0, {element}, false);
}

SpirvBuilder::Id SpirvBuilder::type_struct(const std::vector<Id>& members) {
    return declare(spv::Op::OpTypeStruct, 0, members, false);
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

SpirvBuilder::Id SpirvBuilder::constant_composite(Id type, const std::vector<Id>& parts) {
    return declare(spv::Op::OpConstantComposite, type, parts);
}

SpirvBuilder::Id SpirvBuilder::constant_null(Id type) {
    return declare(spv::Op::OpConstantNull, type, {});
}

SpirvBuilder::Id SpirvBuilder::variable(Id pointer_type, spv::StorageClass storage,
                                        Id initializer) {
    if (storage == spv::StorageClass::Function) {
        throw std::logic_error("SPIR-V Function variables are declared with local_variable");
    }
    Words operands{spirv_word(storage)};
    if (initializer != 0) {
        operands.push_back(initializer);
    }
    return declare(spv::Op::OpVariable, pointer_type, operands, false);
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
    emit(function_->head, spv::Op::OpFunction,
         {result_type, result, spirv_word(control), function_type});
    return result;
}

SpirvBuilder::Id SpirvBuilder::parameter(Id type) {
    Function& fn = function("parameter");
    if (fn.has_block) {
        throw std::logic_error("SPIR-V parameter after the first block");
    }
    const Id result = id();
    emit(fn.head, spv::Op::OpFunctionParameter, {type, result});
    return result;
}

SpirvBuilder::Id SpirvBuilder::local_variable(Id pointer_type, Id initializer) {
    Function& fn = function("local variable");
    const Id result = id();
    Words operands{pointer_type, result, spirv_word(spv::StorageClass::Function)};
    if (initializer != 0) {
        operands.push_back(initializer);
    }
    emit(fn.variables, spv::Op::OpVariable, operands);
    return result;
}

void SpirvBuilder::begin_block(Id label) {
    Function& fn = function("block");
    if (fn.in_block) {
        throw std::logic_error("SPIR-V block started before the previous one ended");
    }
    emit(fn.has_block ? fn.body : fn.head, spv::Op::OpLabel, {label});
    fn.has_block = true;
    fn.in_block = true;
}

void SpirvBuilder::end_function() {
    Function& fn = function("function end");
    if (!fn.has_block || fn.in_block) {
        throw std::logic_error("SPIR-V function ended without a terminated block");
    }
    functions_.insert(functions_.end(), fn.head.begin(), fn.head.end());
    functions_.insert(functions_.end(), fn.variables.begin(), fn.variables.end());
    functions_.insert(functions_.end(), fn.body.begin(), fn.body.end());
    emit(functions_, spv::Op::OpFunctionEnd, {});
    function_.reset();
}

SpirvBuilder::Id SpirvBuilder::op(spv::Op opcode, Id result_type, const Words& operands,
                                  Id result) {
    Function& fn = function("instruction");
    if (!fn.in_block) {
        throw std::logic_error("SPIR-V instruction outside a block");
    }
    if (result == 0) {
        result = id();
    }
    Words all{result_type, result};
    all.insert(all.end(), operands.begin(), operands.end());
    emit(fn.body, opcode, all);
    return result;
}

void SpirvBuilder::op_void(spv::Op opcode, const Words& operands) {
    Function& fn = function("instruction");
    if (!fn.in_block) {
        throw std::logic_error("SPIR-V instruction outside a block");
    }
    emit(fn.body, opcode, operands);
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
         {&capabilities_, &extensions_, &imports_, &memory_model_, &entry_points_,
          &execution_modes_, &names_, &decorations_, &globals_, &functions_}) {
        out.insert(out.end(), section->begin(), section->end());
    }
    return out;
}

}  // namespace bb::gcn
