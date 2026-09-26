#include "pagos/mir/mir.h"

#include <algorithm>
#include <ostream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace pagos::mir {
namespace {

using Predecessors = std::unordered_map<BlockId, std::unordered_set<BlockId>>;
using Successors = std::unordered_map<BlockId, std::vector<BlockId>>;

struct Definition {
    BlockId block;
    std::size_t index;
};

std::string block_name(BlockId block) { return "bb" + std::to_string(block); }

std::string value_name(ValueId value) { return "%" + std::to_string(value); }

using Functions = std::unordered_map<std::string, const Function*>;

std::expected<void, std::string> verify_function(const Function& function,
                                                 const Functions& functions) {
    if (!function.result_type.valid()) {
        return std::unexpected("invalid MIR result type");
    }
    if (function.declaration) {
        if (!function.c_abi || function.internal || !function.blocks.empty())
            return std::unexpected("invalid MIR external declaration");
        return {};
    }
    if (function.blocks.empty()) {
        return std::unexpected("MIR function `" + function.name +
                               "` has no blocks");
    }

    std::unordered_map<BlockId, const BasicBlock*> blocks;
    std::unordered_map<ValueId, Type> values;
    std::unordered_map<ValueId, Definition> definitions;
    for (const auto& block : function.blocks) {
        if (!blocks.emplace(block.id, &block).second) {
            return std::unexpected("duplicate MIR block `" +
                                   block_name(block.id) + "`");
        }
        if (std::holds_alternative<std::monostate>(block.terminator)) {
            return std::unexpected("MIR block `" + block_name(block.id) +
                                   "` has no terminator");
        }
        bool saw_non_phi = false;
        for (std::size_t index = 0; index < block.instructions.size();
             ++index) {
            const auto& instruction = block.instructions[index];
            if (!instruction.type.valid() &&
                !(instruction.type == Type::Void &&
                  std::holds_alternative<StoreOperation>(
                      instruction.operation))) {
                return std::unexpected("invalid MIR value type");
            }
            const bool is_phi =
                std::holds_alternative<PhiOperation>(instruction.operation);
            if (is_phi && saw_non_phi) {
                return std::unexpected("phi in `" + block_name(block.id) +
                                       "` appears after a non-phi instruction");
            }
            saw_non_phi = saw_non_phi || !is_phi;
            if (!values.emplace(instruction.result, instruction.type).second) {
                return std::unexpected("duplicate MIR value `" +
                                       value_name(instruction.result) + "`");
            }
            definitions.emplace(instruction.result,
                                Definition{.block = block.id, .index = index});
        }
    }
    if (!blocks.contains(function.entry)) {
        return std::unexpected("MIR entry block does not exist");
    }

    const auto value_type =
        [&values](ValueId value) -> std::expected<Type, std::string> {
        const auto iterator = values.find(value);
        if (iterator == values.end()) {
            return std::unexpected("undefined MIR value `" + value_name(value) +
                                   "`");
        }
        if (iterator->second == Type::Void)
            return std::unexpected("MIR store has no value");
        return iterator->second;
    };
    const auto require_target =
        [&blocks](BlockId target) -> std::expected<void, std::string> {
        if (!blocks.contains(target)) {
            return std::unexpected("undefined MIR block `" +
                                   block_name(target) + "`");
        }
        return {};
    };

    Predecessors predecessors;
    Successors successors;
    for (const auto& block : function.blocks) {
        if (const auto* branch = std::get_if<Branch>(&block.terminator)) {
            if (auto valid = require_target(branch->target); !valid) {
                return valid;
            }
            predecessors[branch->target].insert(block.id);
            successors[block.id].push_back(branch->target);
            continue;
        }
        if (const auto* branch =
                std::get_if<ConditionalBranch>(&block.terminator)) {
            const auto condition = value_type(branch->condition);
            if (!condition) {
                return std::unexpected(condition.error());
            }
            if (*condition != Type::Bool) {
                return std::unexpected("conditional branch requires `bool`");
            }
            if (auto valid = require_target(branch->then_target); !valid) {
                return valid;
            }
            if (auto valid = require_target(branch->else_target); !valid) {
                return valid;
            }
            predecessors[branch->then_target].insert(block.id);
            predecessors[branch->else_target].insert(block.id);
            successors[block.id].push_back(branch->then_target);
            successors[block.id].push_back(branch->else_target);
            continue;
        }
        const auto& return_operation = std::get<Return>(block.terminator);
        const auto result = value_type(return_operation.value);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result != function.result_type) {
            return std::unexpected("return type does not match MIR function");
        }
    }

    std::unordered_set<BlockId> reachable;
    std::vector<BlockId> pending{function.entry};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!reachable.insert(current).second) {
            continue;
        }
        for (const auto successor : successors[current]) {
            pending.push_back(successor);
        }
    }
    if (reachable.size() != blocks.size()) {
        return std::unexpected("MIR function contains an unreachable block");
    }

    std::unordered_set<BlockId> all_blocks;
    for (const auto& [id, unused] : blocks) {
        (void)unused;
        all_blocks.insert(id);
    }
    std::unordered_map<BlockId, std::unordered_set<BlockId>> dominators;
    for (const auto& block : function.blocks) {
        dominators[block.id] = block.id == function.entry
                                   ? std::unordered_set<BlockId>{block.id}
                                   : all_blocks;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& block : function.blocks) {
            if (block.id == function.entry) {
                continue;
            }
            const auto& incoming_blocks = predecessors[block.id];
            auto iterator = incoming_blocks.begin();
            auto intersection = dominators.at(*iterator);
            ++iterator;
            for (; iterator != incoming_blocks.end(); ++iterator) {
                for (auto candidate = intersection.begin();
                     candidate != intersection.end();) {
                    if (!dominators.at(*iterator).contains(*candidate)) {
                        candidate = intersection.erase(candidate);
                    } else {
                        ++candidate;
                    }
                }
            }
            intersection.insert(block.id);
            if (intersection != dominators.at(block.id)) {
                dominators[block.id] = std::move(intersection);
                changed = true;
            }
        }
    }

    const auto require_dominance =
        [&definitions, &dominators](
            ValueId value, BlockId use_block,
            std::size_t use_index) -> std::expected<void, std::string> {
        const auto definition = definitions.at(value);
        const bool valid =
            definition.block == use_block
                ? definition.index < use_index
                : dominators.at(use_block).contains(definition.block);
        if (!valid) {
            return std::unexpected("MIR value `" + value_name(value) +
                                   "` does not dominate its use");
        }
        return {};
    };

    for (const auto& block : function.blocks) {
        for (std::size_t index = 0; index < block.instructions.size();
             ++index) {
            const auto& instruction = block.instructions[index];
            if (const auto* parameter =
                    std::get_if<ParameterOperation>(&instruction.operation)) {
                if (parameter->index >= function.parameters.size() ||
                    instruction.type != function.parameters[parameter->index])
                    return std::unexpected(
                        "invalid MIR parameter index or type");
                continue;
            }
            if (const auto* call =
                    std::get_if<CallOperation>(&instruction.operation)) {
                const auto found = functions.find(call->callee);
                if (found == functions.end())
                    return std::unexpected("undefined MIR callee `" +
                                           call->callee + "`");
                const auto& callee = *found->second;
                if (instruction.type != callee.result_type ||
                    call->arguments.size() != callee.parameters.size())
                    return std::unexpected("MIR call signature mismatch");
                for (std::size_t argument = 0;
                     argument < call->arguments.size(); ++argument) {
                    const auto value = call->arguments[argument];
                    const auto type = value_type(value);
                    if (!type)
                        return std::unexpected(type.error());
                    if (*type != callee.parameters[argument])
                        return std::unexpected(
                            "MIR call argument type mismatch");
                    if (auto valid = require_dominance(value, block.id, index);
                        !valid)
                        return valid;
                }
                continue;
            }
            if (const auto* constant =
                    std::get_if<ConstantOperation>(&instruction.operation)) {
                if (const auto* pointer =
                        std::get_if<PointerConstant>(&constant->value)) {
                    if (!instruction.type.is_pointer() ||
                        pointer->address.type() !=
                            IntegerType{instruction.type.pointer_bits, false,
                                        true})
                        return std::unexpected("invalid MIR pointer constant");
                    continue;
                }
                const auto* integers =
                    std::get_if<std::vector<IntegerValue>>(&constant->value);
                const auto* booleans =
                    std::get_if<std::vector<bool>>(&constant->value);
                if (const auto* record =
                        std::get_if<RecordConstant>(&constant->value)) {
                    if (!instruction.type.is_record() ||
                        record->name != instruction.type.record_name ||
                        record->fields.size() !=
                            instruction.type.fields.size()) {
                        return std::unexpected(
                            "MIR record constant shape mismatch");
                    }
                    for (std::size_t field = 0; field < record->fields.size();
                         ++field) {
                        const auto type = std::visit(
                            [](auto scalar) -> Type {
                                if constexpr (std::is_same_v<decltype(scalar),
                                                             bool>)
                                    return Type::Bool;
                                else
                                    return Type::integer(
                                        scalar.type().width,
                                        scalar.type().is_signed,
                                        scalar.type().is_usize);
                            },
                            record->fields[field]);
                        if (type != instruction.type.fields[field]) {
                            return std::unexpected(
                                "MIR record constant field type mismatch");
                        }
                    }
                    continue;
                }
                const auto matches_integer = [](const Type& type,
                                                const IntegerValue& value) {
                    return type.kind == Type::Integer &&
                           type.integer_type == value.type();
                };
                const auto* integer =
                    std::get_if<IntegerValue>(&constant->value);
                const bool valid =
                    (instruction.type.is_array() && integers &&
                     integers->size() == instruction.type.length &&
                     std::ranges::all_of(
                         *integers,
                         [&](const auto& value) {
                             return matches_integer(
                                 instruction.type.element_type(), value);
                         })) ||
                    (instruction.type.is_array() &&
                     instruction.type.element_type() == Type::Bool &&
                     booleans && booleans->size() == instruction.type.length) ||
                    (integer && matches_integer(instruction.type, *integer)) ||
                    (instruction.type == Type::Bool &&
                     std::holds_alternative<bool>(constant->value));
                if (!valid) {
                    return std::unexpected("constant type mismatch for `" +
                                           value_name(instruction.result) +
                                           "`");
                }
                continue;
            }
            if (std::holds_alternative<ExternalInputOperation>(
                    instruction.operation)) {
                if (instruction.type != Type::U32) {
                    return std::unexpected("external input must produce `u32`");
                }
                continue;
            }
            if (const auto* unary =
                    std::get_if<UnaryOperation>(&instruction.operation)) {
                const auto operand = value_type(unary->operand);
                if (!operand) {
                    return std::unexpected(operand.error());
                }
                if (auto valid =
                        require_dominance(unary->operand, block.id, index);
                    !valid) {
                    return valid;
                }
                const auto expected = unary->operation == UnaryOperator::Not
                                          ? Type(Type::Bool)
                                          : *operand;
                if ((unary->operation != UnaryOperator::Not &&
                     unary->operation != UnaryOperator::BitNot &&
                     unary->operation != UnaryOperator::Negate) ||
                    (unary->operation != UnaryOperator::Not &&
                     operand->kind != Type::Integer) ||
                    (unary->operation == UnaryOperator::Negate &&
                     !operand->integer_type.is_signed) ||
                    *operand != expected || instruction.type != expected) {
                    return std::unexpected("invalid MIR unary operation");
                }
                continue;
            }
            if (const auto* binary =
                    std::get_if<BinaryOperation>(&instruction.operation)) {
                const auto left = value_type(binary->left);
                const auto right = value_type(binary->right);
                if (!left) {
                    return std::unexpected(left.error());
                }
                if (!right) {
                    return std::unexpected(right.error());
                }
                if (auto valid =
                        require_dominance(binary->left, block.id, index);
                    !valid) {
                    return valid;
                }
                if (auto valid =
                        require_dominance(binary->right, block.id, index);
                    !valid) {
                    return valid;
                }
                using enum BinaryOperator;
                if (binary->operation == Equal ||
                    binary->operation == NotEqual) {
                    if (left->is_array() || left->is_record() ||
                        *left != *right || instruction.type != Type::Bool) {
                        return std::unexpected(
                            "invalid MIR equality operation");
                    }
                } else {
                    const bool comparison = binary->operation == Less ||
                                            binary->operation == LessEqual ||
                                            binary->operation == Greater ||
                                            binary->operation == GreaterEqual;
                    const auto result_type =
                        comparison ? Type(Type::Bool) : *left;
                    if (left->kind != Type::Integer || *right != *left ||
                        instruction.type != result_type) {
                        return std::unexpected(
                            "invalid MIR arithmetic operation");
                    }
                }
                continue;
            }
            if (const auto* load =
                    std::get_if<LoadOperation>(&instruction.operation)) {
                const auto pointer = value_type(load->pointer);
                if (!pointer)
                    return std::unexpected(pointer.error());
                if (auto valid =
                        require_dominance(load->pointer, block.id, index);
                    !valid)
                    return valid;
                if (!pointer->is_pointer() ||
                    instruction.type != pointer->pointee_type())
                    return std::unexpected("invalid MIR pointer load");
                continue;
            }
            if (const auto* store =
                    std::get_if<StoreOperation>(&instruction.operation)) {
                const auto pointer = value_type(store->pointer);
                const auto value = value_type(store->value);
                if (!pointer || !value)
                    return std::unexpected("invalid MIR store operand");
                if (auto valid =
                        require_dominance(store->pointer, block.id, index);
                    !valid)
                    return valid;
                if (auto valid =
                        require_dominance(store->value, block.id, index);
                    !valid)
                    return valid;
                if (!pointer->is_pointer() || !pointer->pointer_mutable ||
                    *value != pointer->pointee_type() ||
                    instruction.type != Type::Void)
                    return std::unexpected("invalid MIR pointer store");
                continue;
            }
            if (const auto* cast =
                    std::get_if<PointerCastOperation>(&instruction.operation)) {
                const auto operand = value_type(cast->operand);
                if (!operand)
                    return std::unexpected(operand.error());
                if (auto valid =
                        require_dominance(cast->operand, block.id, index);
                    !valid)
                    return valid;
                const auto size_type = [](const Type& type) {
                    return type.kind == Type::Integer &&
                           type.integer_type.is_usize;
                };
                const auto& result = instruction.type;
                if (!((operand->is_pointer() && size_type(result)) ||
                      (size_type(*operand) && result.is_pointer()) ||
                      (operand->is_pointer() && result.is_pointer() &&
                       operand->pointee_type() == result.pointee_type() &&
                       (operand->pointer_mutable || !result.pointer_mutable))))
                    return std::unexpected("invalid MIR pointer cast");
                continue;
            }
            if (const auto* cast =
                    std::get_if<IntegerCastOperation>(&instruction.operation)) {
                const auto operand = value_type(cast->operand);
                if (!operand)
                    return std::unexpected(operand.error());
                if (auto valid =
                        require_dominance(cast->operand, block.id, index);
                    !valid)
                    return valid;
                if (operand->kind != Type::Integer ||
                    instruction.type.kind != Type::Integer) {
                    return std::unexpected("invalid MIR integer cast");
                }
                continue;
            }
            if (const auto* cast =
                    std::get_if<BoolToU32Operation>(&instruction.operation)) {
                const auto operand = value_type(cast->operand);
                if (!operand) {
                    return std::unexpected(operand.error());
                }
                if (auto valid =
                        require_dominance(cast->operand, block.id, index);
                    !valid) {
                    return valid;
                }
                if (*operand != Type::Bool || instruction.type != Type::U32) {
                    return std::unexpected("invalid MIR bool-to-u32 cast");
                }
                continue;
            }

            if (const auto* array =
                    std::get_if<ArrayOperation>(&instruction.operation)) {
                if (!instruction.type.is_array() ||
                    array->elements.size() != instruction.type.length) {
                    return std::unexpected(
                        "invalid MIR array length or result type");
                }
                for (const auto element : array->elements) {
                    const auto operand = value_type(element);
                    if (!operand) {
                        return std::unexpected(operand.error());
                    }
                    if (*operand != instruction.type.element_type()) {
                        return std::unexpected(
                            "MIR array element type mismatch");
                    }
                    if (auto valid =
                            require_dominance(element, block.id, index);
                        !valid) {
                        return valid;
                    }
                }
                continue;
            }
            if (const auto* access =
                    std::get_if<IndexOperation>(&instruction.operation)) {
                const auto array = value_type(access->array);
                const auto offset = value_type(access->index);
                if (!array) {
                    return std::unexpected(array.error());
                }
                if (!offset) {
                    return std::unexpected(offset.error());
                }
                if (!array->is_array() || *offset != Type::U32 ||
                    instruction.type != array->element_type()) {
                    return std::unexpected("invalid MIR checked index types");
                }
                if (auto valid =
                        require_dominance(access->array, block.id, index);
                    !valid) {
                    return valid;
                }
                if (auto valid =
                        require_dominance(access->index, block.id, index);
                    !valid) {
                    return valid;
                }
                continue;
            }

            if (const auto* record =
                    std::get_if<RecordOperation>(&instruction.operation)) {
                if (!instruction.type.is_record() ||
                    record->fields.size() != instruction.type.fields.size()) {
                    return std::unexpected("invalid MIR record construction");
                }
                for (std::size_t field = 0; field < record->fields.size();
                     ++field) {
                    const auto operand = value_type(record->fields[field]);
                    if (!operand) {
                        return std::unexpected(operand.error());
                    }
                    if (*operand != instruction.type.fields[field]) {
                        return std::unexpected(
                            "MIR record field type mismatch");
                    }
                    if (auto valid = require_dominance(record->fields[field],
                                                       block.id, index);
                        !valid) {
                        return valid;
                    }
                }
                continue;
            }
            if (const auto* field =
                    std::get_if<FieldOperation>(&instruction.operation)) {
                const auto record = value_type(field->record);
                if (!record) {
                    return std::unexpected(record.error());
                }
                if (!record->is_record() ||
                    field->index >= record->fields.size() ||
                    instruction.type != record->fields[field->index]) {
                    return std::unexpected("invalid MIR field access");
                }
                if (auto valid =
                        require_dominance(field->record, block.id, index);
                    !valid) {
                    return valid;
                }
                continue;
            }

            const auto& phi = std::get<PhiOperation>(instruction.operation);
            if (phi.incoming.empty()) {
                return std::unexpected("MIR phi has no incoming values");
            }
            std::unordered_set<BlockId> incoming_blocks;
            for (const auto& incoming : phi.incoming) {
                const auto operand = value_type(incoming.value);
                if (!operand) {
                    return std::unexpected(operand.error());
                }
                if (*operand != instruction.type) {
                    return std::unexpected("MIR phi operand type mismatch");
                }
                if (!predecessors[block.id].contains(incoming.block)) {
                    return std::unexpected(
                        "MIR phi references non-predecessor `" +
                        block_name(incoming.block) + "`");
                }
                if (!incoming_blocks.insert(incoming.block).second) {
                    return std::unexpected(
                        "MIR phi has duplicate predecessor `" +
                        block_name(incoming.block) + "`");
                }
                const auto definition = definitions.at(incoming.value);
                if (definition.block != incoming.block &&
                    !dominators.at(incoming.block).contains(definition.block)) {
                    return std::unexpected(
                        "MIR phi value does not dominate predecessor `" +
                        block_name(incoming.block) + "`");
                }
            }
            if (incoming_blocks != predecessors[block.id]) {
                return std::unexpected(
                    "MIR phi must cover every predecessor exactly once");
            }
        }

        const auto terminator_index = block.instructions.size();
        if (const auto* branch =
                std::get_if<ConditionalBranch>(&block.terminator)) {
            if (auto valid = require_dominance(branch->condition, block.id,
                                               terminator_index);
                !valid) {
                return valid;
            }
        } else if (const auto* return_operation =
                       std::get_if<Return>(&block.terminator)) {
            if (auto valid = require_dominance(return_operation->value,
                                               block.id, terminator_index);
                !valid) {
                return valid;
            }
        }
    }
    return {};
}

void print_instruction(const Instruction& instruction, std::ostream& output) {
    output << "  " << value_name(instruction.result) << ": "
           << type_name(instruction.type) << " = ";
    if (const auto* constant =
            std::get_if<ConstantOperation>(&instruction.operation)) {
        output << "const ";
        print_constant(constant->value, output);
    } else if (const auto* parameter =
                   std::get_if<ParameterOperation>(&instruction.operation)) {
        output << "parameter " << parameter->index;
    } else if (const auto* call =
                   std::get_if<CallOperation>(&instruction.operation)) {
        output << "call @" << call->callee << '(';
        for (std::size_t index = 0; index < call->arguments.size(); ++index) {
            if (index != 0)
                output << ", ";
            output << value_name(call->arguments[index]);
        }
        output << ')';
    } else if (std::holds_alternative<ExternalInputOperation>(
                   instruction.operation)) {
        output << "external_input";
    } else if (const auto* unary =
                   std::get_if<UnaryOperation>(&instruction.operation)) {
        output << (unary->operation == UnaryOperator::Not      ? "not "
                   : unary->operation == UnaryOperator::BitNot ? "bit.not "
                                                               : "negate ")
               << value_name(unary->operand);
    } else if (const auto* binary =
                   std::get_if<BinaryOperation>(&instruction.operation)) {
        output << binary_name(binary->operation) << ' '
               << value_name(binary->left) << ", " << value_name(binary->right);
    } else if (const auto* integer_cast =
                   std::get_if<IntegerCastOperation>(&instruction.operation)) {
        output << "integer_cast " << value_name(integer_cast->operand);
    } else if (const auto* pointer_cast =
                   std::get_if<PointerCastOperation>(&instruction.operation)) {
        output << "pointer_cast " << value_name(pointer_cast->operand);
    } else if (const auto* load =
                   std::get_if<LoadOperation>(&instruction.operation)) {
        output << "load " << value_name(load->pointer);
    } else if (const auto* store =
                   std::get_if<StoreOperation>(&instruction.operation)) {
        output << "store " << value_name(store->pointer) << ", "
               << value_name(store->value);
    } else if (const auto* cast =
                   std::get_if<BoolToU32Operation>(&instruction.operation)) {
        output << "bool_to_u32 " << value_name(cast->operand);
    } else if (const auto* array =
                   std::get_if<ArrayOperation>(&instruction.operation)) {
        output << "array ";
        for (std::size_t index = 0; index < array->elements.size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << value_name(array->elements[index]);
        }
    } else if (const auto* record =
                   std::get_if<RecordOperation>(&instruction.operation)) {
        output << "record ";
        for (std::size_t index = 0; index < record->fields.size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << value_name(record->fields[index]);
        }
    } else if (const auto* field =
                   std::get_if<FieldOperation>(&instruction.operation)) {
        output << "field " << value_name(field->record) << ", " << field->index;
    } else if (const auto* access =
                   std::get_if<IndexOperation>(&instruction.operation)) {
        output << "index.checked " << value_name(access->array) << ", "
               << value_name(access->index);
    } else {
        const auto& phi = std::get<PhiOperation>(instruction.operation);
        output << "phi ";
        for (std::size_t index = 0; index < phi.incoming.size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << '[' << block_name(phi.incoming[index].block) << ": "
                   << value_name(phi.incoming[index].value) << ']';
        }
    }
    output << '\n';
}

} // namespace

std::string type_name(const Type& type) {
    if (type == Type::Void)
        return "void";
    if (type.is_pointer())
        return std::string(type.pointer_mutable ? "*mut " : "*const ") +
               type_name(type.pointee_type());
    if (type.is_record()) {
        return type.record_name;
    }
    if (type.is_array()) {
        return "[" + type_name(type.element_type()) + "; " +
               std::to_string(type.length) + "]";
    }
    if (type.kind == Type::Integer) {
        if (type.integer_type.is_usize)
            return "usize" + std::to_string(type.integer_type.width);
        return std::string(type.integer_type.is_signed ? "i" : "u") +
               std::to_string(type.integer_type.width);
    }
    return type == Type::Bool ? "bool" : "<invalid>";
}

std::string_view binary_name(BinaryOperator operation) noexcept {
    using enum BinaryOperator;
    switch (operation) {
    case Add:
        return "add";
    case Subtract:
        return "sub";
    case Multiply:
        return "mul";
    case DivideChecked:
        return "udiv.checked";
    case RemainderChecked:
        return "urem.checked";
    case BitAnd:
        return "bit.and";
    case BitOr:
        return "bit.or";
    case BitXor:
        return "bit.xor";
    case ShiftLeftChecked:
        return "shl.checked";
    case ShiftRightChecked:
        return "lshr.checked";
    case Equal:
        return "eq";
    case NotEqual:
        return "ne";
    case Less:
        return "lt";
    case LessEqual:
        return "le";
    case Greater:
        return "gt";
    case GreaterEqual:
        return "ge";
    }
    return "unknown";
}

std::expected<void, std::string> verify(const Module& module) {
    if (module.pointer_bits != 0 && module.pointer_bits != 32 &&
        module.pointer_bits != 64)
        return std::unexpected("invalid MIR target pointer width");
    if (module.functions.empty()) {
        return std::unexpected("MIR module has no functions");
    }
    Functions functions;
    for (const auto& function : module.functions) {
        if (function.name.empty() || function.name == "pagos_external_input")
            return std::unexpected("empty or reserved MIR function name");
        if (!functions.emplace(function.name, &function).second)
            return std::unexpected("duplicate MIR function `" + function.name +
                                   "`");
    }
    std::unordered_map<std::string, Type> records;
    const auto consistent = [&](const Type& type) {
        if (!type.is_record()) {
            return true;
        }
        const auto [entry, inserted] = records.emplace(type.record_name, type);
        return inserted || entry->second == type;
    };
    for (const auto& function : module.functions) {
        for (const auto& parameter : function.parameters) {
            if (!parameter.valid() ||
                !parameter.matches_pointer_width(module.pointer_bits))
                return std::unexpected(
                    "invalid MIR parameter type or target width");
            if (!consistent(parameter))
                return std::unexpected("inconsistent MIR record definition");
        }
        if (function.c_abi) {
            const auto supported = [](const Type& type) {
                return type.is_pointer() || (type.kind == Type::Integer &&
                                             type.integer_type.width == 32 &&
                                             !type.integer_type.is_usize);
            };
            if (function.internal || !supported(function.result_type) ||
                !std::ranges::all_of(function.parameters, supported))
                return std::unexpected(
                    "C ABI MIR signatures require external "
                    "linkage and i32/u32 or integer pointer types");
        }
        if (!function.result_type.matches_pointer_width(module.pointer_bits))
            return std::unexpected(
                "MIR usize width does not match semantic target");
        if (!consistent(function.result_type)) {
            return std::unexpected("inconsistent MIR record definition");
        }
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (!instruction.type.matches_pointer_width(
                        module.pointer_bits))
                    return std::unexpected(
                        "MIR usize width does not match semantic target");
                if (!consistent(instruction.type)) {
                    return std::unexpected(
                        "inconsistent MIR record definition");
                }
            }
        }
        if (auto valid = verify_function(function, functions); !valid) {
            return valid;
        }
    }
    return {};
}

void print(const Module& module, std::ostream& output) {
    if (module.pointer_bits != 0)
        output << "target pointer_bits = " << module.pointer_bits << '\n';
    for (const auto& function : module.functions) {
        output << (function.declaration ? "extern C func @"
                   : function.c_abi     ? "export C func @"
                                        : "func @")
               << function.name << '(';
        for (std::size_t index = 0; index < function.parameters.size();
             ++index) {
            if (index != 0)
                output << ", ";
            output << type_name(function.parameters[index]);
        }
        output << ") -> " << type_name(function.result_type);
        if (function.declaration) {
            output << '\n';
            continue;
        }
        output << " {\n";
        for (const auto& block : function.blocks) {
            output << block_name(block.id) << ":\n";
            for (const auto& instruction : block.instructions) {
                print_instruction(instruction, output);
            }
            output << "  ";
            if (const auto* branch = std::get_if<Branch>(&block.terminator)) {
                output << "br " << block_name(branch->target);
            } else if (const auto* conditional_branch =
                           std::get_if<ConditionalBranch>(&block.terminator)) {
                output << "condbr " << value_name(conditional_branch->condition)
                       << ", " << block_name(conditional_branch->then_target)
                       << ", " << block_name(conditional_branch->else_target);
            } else if (const auto* return_operation =
                           std::get_if<Return>(&block.terminator)) {
                output << "return " << value_name(return_operation->value);
            } else {
                output << "<missing terminator>";
            }
            output << '\n';
        }
        output << "}\n";
    }
}

} // namespace pagos::mir
