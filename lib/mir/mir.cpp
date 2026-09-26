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

std::expected<void, std::string> verify_function(const Function& function) {
    if (!function.result_type.valid()) {
        return std::unexpected("invalid MIR result type");
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
            if (!instruction.type.valid()) {
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
            if (const auto* constant =
                    std::get_if<ConstantOperation>(&instruction.operation)) {
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
                                        scalar.type().is_signed);
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
    if (type.is_record()) {
        return type.record_name;
    }
    if (type.is_array()) {
        return "[" + type_name(type.element_type()) + "; " +
               std::to_string(type.length) + "]";
    }
    if (type.kind == Type::Integer) {
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
    if (module.functions.empty()) {
        return std::unexpected("MIR module has no functions");
    }
    std::unordered_set<std::string> names;
    std::unordered_map<std::string, Type> records;
    const auto consistent = [&](const Type& type) {
        if (!type.is_record()) {
            return true;
        }
        const auto [entry, inserted] = records.emplace(type.record_name, type);
        return inserted || entry->second == type;
    };
    for (const auto& function : module.functions) {
        if (!consistent(function.result_type)) {
            return std::unexpected("inconsistent MIR record definition");
        }
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (!consistent(instruction.type)) {
                    return std::unexpected(
                        "inconsistent MIR record definition");
                }
            }
        }
        if (!names.insert(function.name).second) {
            return std::unexpected("duplicate MIR function `" + function.name +
                                   "`");
        }
        if (auto valid = verify_function(function); !valid) {
            return valid;
        }
    }
    return {};
}

void print(const Module& module, std::ostream& output) {
    for (const auto& function : module.functions) {
        output << "func @" << function.name << "() -> "
               << type_name(function.result_type) << " {\n";
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
