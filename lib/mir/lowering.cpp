#include "pagos/mir/lowering.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pagos::mir {
namespace {

struct LoweringContext {
    Module output;
    std::unordered_map<std::string, Type> records;
    std::unordered_map<const hir::ResidualFunction*, std::string> callees;
};

class Lowerer {
  public:
    explicit Lowerer(LoweringContext& context) : context_(context) {
        function_.name = "pagos_main";
        function_.result_type = Type::U32;
        function_.entry = create_block("entry", std::nullopt);
        current_block_ = function_.entry;
    }

    std::expected<Module, std::string> run(const hir::Module& hir_module) {
        for (const auto& record : hir_module.records) {
            Type type{Type::Record};
            type.record_name = record.name;
            for (const auto& field : record.fields) {
                type.fields.push_back(type_of(field));
            }
            context_.records.emplace(record.name, std::move(type));
        }
        ValueId result{};
        if (hir_module.result) {
            auto lowered = lower_expression(hir_module.result);
            if (!lowered) {
                return std::unexpected(lowered.error());
            }
            result = *lowered;
            if (hir_module.result->type == sema::TypeKind::Bool) {
                result = emit(Type::U32, BoolToU32Operation{.operand = result},
                              hir_module.result->span);
            } else if (hir_module.result->type.kind ==
                           sema::TypeKind::Integer &&
                       hir_module.result->type != sema::TypeKind::Integer) {
                result =
                    emit(Type::U32, IntegerCastOperation{.operand = result},
                         hir_module.result->span);
            } else if (hir_module.result->type.is_array() ||
                       hir_module.result->type.is_record()) {
                result = emit(Type::U32,
                              ConstantOperation{.value = std::uint32_t{0}},
                              hir_module.result->span);
            }
        } else {
            result = emit(Type::U32,
                          ConstantOperation{.value = std::uint32_t{0}}, {});
        }
        block(current_block_).terminator = Return{.value = result};

        context_.output.pointer_bits = hir_module.pointer_bits;
        context_.output.functions.insert(context_.output.functions.begin(),
                                         std::move(function_));
        if (auto valid = verify(context_.output); !valid) {
            return std::unexpected("invalid lowered MIR: " + valid.error());
        }
        return std::move(context_.output);
    }

  private:
    struct CachedValue {
        BlockId block;
        ValueId value;
    };

    struct ReturnTarget {
        BlockId exit;
        std::vector<PhiIncoming> incoming;
    };

    BlockId create_block(std::string name, std::optional<BlockId> parent) {
        const auto id = static_cast<BlockId>(function_.blocks.size());
        function_.blocks.push_back({.id = id,
                                    .name = std::move(name),
                                    .instructions = {},
                                    .terminator = std::monostate{}});
        parents_.push_back(parent);
        return id;
    }

    BasicBlock& block(BlockId id) { return function_.blocks.at(id); }

    bool dominates(BlockId definition, BlockId use) const {
        auto cursor = std::optional<BlockId>{use};
        while (cursor) {
            if (*cursor == definition) {
                return true;
            }
            cursor = parents_.at(*cursor);
        }
        return false;
    }

    std::optional<ValueId> find_cached(const hir::Expr* expression) const {
        const auto iterator = cache_.find(expression);
        if (iterator == cache_.end()) {
            return std::nullopt;
        }
        for (auto cached = iterator->second.rbegin();
             cached != iterator->second.rend(); ++cached) {
            if (dominates(cached->block, current_block_)) {
                return cached->value;
            }
        }
        return std::nullopt;
    }

    void cache(const hir::Expr* expression, ValueId value) {
        cache_[expression].push_back({.block = current_block_, .value = value});
    }

    ValueId emit(Type type, Operation operation, source::Span span) {
        const auto result = next_value_++;
        block(current_block_)
            .instructions.push_back({.result = result,
                                     .type = std::move(type),
                                     .operation = std::move(operation),
                                     .span = span});
        return result;
    }

    std::expected<ValueId, std::string>
    lower_expression(const hir::ExprPtr& expression) {
        if (!expression) {
            return std::unexpected("cannot lower an empty HIR expression");
        }
        if (const auto cached = find_cached(expression.get())) {
            return *cached;
        }

        std::expected<ValueId, std::string> result =
            std::unexpected("unknown HIR expression");
        switch (expression->kind) {
        case hir::Expr::Kind::Constant:
            if (!expression->constant) {
                return std::unexpected("HIR constant has no value");
            }
            result = emit(type_of(expression->type),
                          ConstantOperation{.value = to_constant(
                                                expression->constant.value())},
                          expression->span);
            break;
        case hir::Expr::Kind::Reference:
        case hir::Expr::Kind::RuntimeBoundary:
            if (expression->operands.size() != 1) {
                return std::unexpected(
                    "HIR reference or runtime boundary requires one operand");
            }
            result = lower_expression(expression->operands.front());
            break;
        case hir::Expr::Kind::ExternalInput:
            result =
                emit(Type::U32, ExternalInputOperation{}, expression->span);
            break;
        case hir::Expr::Kind::Cast: {
            const auto operand = lower_expression(expression->operands.at(0));
            if (!operand)
                return std::unexpected(operand.error());
            if (!live_)
                return *operand;
            result = emit(type_of(expression->type),
                          IntegerCastOperation{.operand = *operand},
                          expression->span);
            break;
        }
        case hir::Expr::Kind::Unary:
            result = lower_unary(expression);
            break;
        case hir::Expr::Kind::Binary:
            result = lower_binary(expression);
            break;
        case hir::Expr::Kind::If:
            result = lower_if(expression);
            break;
        case hir::Expr::Kind::Sequence:
            result = lower_sequence(expression);
            break;
        case hir::Expr::Kind::Parameter:
            return std::unexpected("unbound residual HIR parameter");
        case hir::Expr::Kind::LoopIndex:
            return std::unexpected("HIR loop index used outside its loop");
        case hir::Expr::Kind::RangeLoop:
            result = lower_range_loop(expression);
            break;
        case hir::Expr::Kind::Return:
            result = lower_return(expression);
            break;
        case hir::Expr::Kind::ReturnScope:
            result = lower_return_scope(expression);
            break;
        case hir::Expr::Kind::Call:
            result = lower_call(expression);
            break;
        case hir::Expr::Kind::Array:
        case hir::Expr::Kind::Index:
            result = lower_array_operation(expression);
            break;
        case hir::Expr::Kind::Record:
        case hir::Expr::Kind::Field:
            result = lower_record_operation(expression);
            break;
        }
        if (result && live_) {
            cache(expression.get(), *result);
        }
        return result;
    }

    std::expected<ValueId, std::string>
    lower_unary(const hir::ExprPtr& expression) {
        if (expression->operands.size() != 1 || !expression->unary_operation) {
            return std::unexpected("malformed unary HIR expression");
        }
        auto operand = lower_expression(expression->operands.front());
        if (!operand || !live_) {
            return operand;
        }
        return emit(
            type_of(expression->type),
            UnaryOperation{.operation = *expression->unary_operation ==
                                                syntax::UnaryOperator::Not
                                            ? UnaryOperator::Not
                                        : *expression->unary_operation ==
                                                syntax::UnaryOperator::BitNot
                                            ? UnaryOperator::BitNot
                                            : UnaryOperator::Negate,
                           .operand = *operand},
            expression->span);
    }

    std::expected<ValueId, std::string>
    lower_binary(const hir::ExprPtr& expression) {
        if (expression->operands.size() != 2 || !expression->binary_operation) {
            return std::unexpected("malformed binary HIR expression");
        }
        auto left = lower_expression(expression->operands[0]);
        if (!left || !live_) {
            return left;
        }
        auto right = lower_expression(expression->operands[1]);
        if (!right || !live_) {
            return right;
        }
        const auto operation =
            binary_operation(expression->binary_operation.value());
        if (!operation) {
            return std::unexpected(
                "logical HIR operation must be residualized as control flow");
        }
        return emit(type_of(expression->type),
                    BinaryOperation{.operation = *operation,
                                    .left = *left,
                                    .right = *right},
                    expression->span);
    }

    std::expected<ValueId, std::string>
    lower_if(const hir::ExprPtr& expression) {
        if (expression->operands.size() != 3) {
            return std::unexpected("malformed conditional HIR expression");
        }
        auto condition = lower_expression(expression->operands[0]);
        if (!condition || !live_) {
            return condition;
        }

        const auto branch_block = current_block_;
        const auto then_block = create_block("if.then", branch_block);
        const auto else_block = create_block("if.else", branch_block);
        block(branch_block).terminator =
            ConditionalBranch{.condition = *condition,
                              .then_target = then_block,
                              .else_target = else_block};

        current_block_ = then_block;
        auto then_value = lower_expression(expression->operands[1]);
        if (!then_value) {
            return then_value;
        }
        const auto then_exit = current_block_;
        const auto then_live = live_;

        current_block_ = else_block;
        live_ = true;
        auto else_value = lower_expression(expression->operands[2]);
        if (!else_value) {
            return else_value;
        }
        const auto else_exit = current_block_;
        const auto else_live = live_;

        if (!then_live && !else_live) {
            return ValueId{};
        }
        live_ = true;
        if (!then_live) {
            return *else_value;
        }
        if (!else_live) {
            current_block_ = then_exit;
            return *then_value;
        }

        const auto merge_block = create_block("if.merge", branch_block);
        block(then_exit).terminator = Branch{.target = merge_block};
        block(else_exit).terminator = Branch{.target = merge_block};
        current_block_ = merge_block;
        return emit(
            type_of(expression->type),
            PhiOperation{.incoming =
                             {
                                 {.block = then_exit, .value = *then_value},
                                 {.block = else_exit, .value = *else_value},
                             }},
            expression->span);
    }

    std::expected<ValueId, std::string>
    lower_sequence(const hir::ExprPtr& expression) {
        if (expression->operands.empty()) {
            return std::unexpected("HIR sequence has no operands");
        }
        std::expected<ValueId, std::string> result =
            std::unexpected("HIR sequence has no result");
        for (const auto& operand : expression->operands) {
            result = lower_expression(operand);
            if (!result || !live_) {
                return result;
            }
        }
        return result;
    }

    std::expected<ValueId, std::string>
    lower_range_loop(const hir::ExprPtr& expression) {
        if (expression->operands.size() < 3 ||
            expression->operands[2]->kind != hir::Expr::Kind::LoopIndex) {
            return std::unexpected("malformed range-loop HIR expression");
        }
        auto begin = lower_expression(expression->operands[0]);
        if (!begin || !live_) {
            return begin;
        }
        auto end = lower_expression(expression->operands[1]);
        if (!end || !live_) {
            return end;
        }

        const auto preheader = current_block_;
        const auto header = create_block("loop.header", preheader);
        const auto body = create_block("loop.body", header);
        const auto exit = create_block("loop.exit", header);
        block(preheader).terminator = Branch{.target = header};

        current_block_ = header;
        const auto index = emit(
            Type::U32,
            PhiOperation{.incoming = {{.block = preheader, .value = *begin}}},
            expression->operands[2]->span);
        cache(expression->operands[2].get(), index);
        const auto condition =
            emit(Type::Bool,
                 BinaryOperation{.operation = BinaryOperator::Less,
                                 .left = index,
                                 .right = *end},
                 expression->span);
        block(header).terminator = ConditionalBranch{
            .condition = condition, .then_target = body, .else_target = exit};

        current_block_ = body;
        for (std::size_t operand = 3; operand < expression->operands.size();
             ++operand) {
            if (auto lowered = lower_expression(expression->operands[operand]);
                !lowered) {
                return lowered;
            }
            if (!live_) {
                break;
            }
        }
        if (live_) {
            const auto one =
                emit(Type::U32, ConstantOperation{.value = std::uint32_t{1}},
                     expression->span);
            const auto next =
                emit(Type::U32,
                     BinaryOperation{.operation = BinaryOperator::Add,
                                     .left = index,
                                     .right = one},
                     expression->span);
            const auto body_exit = current_block_;
            block(body_exit).terminator = Branch{.target = header};
            auto& phi = std::get<PhiOperation>(
                block(header).instructions.front().operation);
            phi.incoming.push_back({.block = body_exit, .value = next});
        }

        current_block_ = exit;
        live_ = true;
        return *begin;
    }

    std::expected<ValueId, std::string>
    lower_return(const hir::ExprPtr& expression) {
        if (expression->operands.size() != 1 || return_targets_.empty()) {
            return std::unexpected(
                "HIR return requires a value and call scope");
        }
        auto value = lower_expression(expression->operands[0]);
        if (!value || !live_) {
            return value;
        }
        auto& target = return_targets_.back();
        target.incoming.push_back({.block = current_block_, .value = *value});
        block(current_block_).terminator = Branch{.target = target.exit};
        live_ = false;
        return *value;
    }

    std::expected<ValueId, std::string>
    lower_return_scope(const hir::ExprPtr& expression) {
        if (expression->operands.size() != 1) {
            return std::unexpected("HIR call scope requires one body");
        }
        const auto exit = create_block("call.exit", current_block_);
        return_targets_.push_back({.exit = exit, .incoming = {}});
        auto value = lower_expression(expression->operands[0]);
        if (!value) {
            return value;
        }
        if (live_) {
            return_targets_.back().incoming.push_back(
                {.block = current_block_, .value = *value});
            block(current_block_).terminator = Branch{.target = exit};
        }
        auto incoming = std::move(return_targets_.back().incoming);
        return_targets_.pop_back();
        if (incoming.empty()) {
            return std::unexpected("HIR call scope has no result path");
        }
        current_block_ = exit;
        live_ = true;
        return emit(type_of(expression->type),
                    PhiOperation{.incoming = std::move(incoming)},
                    expression->span);
    }

    std::expected<ValueId, std::string>
    lower_array_operation(const hir::ExprPtr& expression) {
        std::vector<ValueId> values;
        values.reserve(expression->operands.size());
        for (const auto& operand : expression->operands) {
            auto value = lower_expression(operand);
            if (!value || !live_) {
                return value;
            }
            values.push_back(*value);
        }
        if (expression->kind == hir::Expr::Kind::Array) {
            return emit(type_of(expression->type),
                        ArrayOperation{.elements = std::move(values)},
                        expression->span);
        }
        if (values.size() != 2) {
            return std::unexpected("HIR index requires array and index");
        }
        return emit(type_of(expression->type),
                    IndexOperation{.array = values[0], .index = values[1]},
                    expression->span);
    }

    std::expected<ValueId, std::string>
    lower_record_operation(const hir::ExprPtr& expression) {
        std::vector<ValueId> fields;
        for (const auto& operand : expression->operands) {
            auto value = lower_expression(operand);
            if (!value || !live_) {
                return value;
            }
            fields.push_back(*value);
        }
        if (expression->kind == hir::Expr::Kind::Record) {
            return emit(type_of(expression->type),
                        RecordOperation{.fields = std::move(fields)},
                        expression->span);
        }
        if (fields.size() != 1) {
            return std::unexpected("HIR field access requires one record");
        }
        return emit(type_of(expression->type),
                    FieldOperation{.record = fields[0],
                                   .index = expression->field_index},
                    expression->span);
    }

    std::expected<ValueId, std::string>
    lower_call(const hir::ExprPtr& expression) {
        if (!expression->callee || !expression->callee->body)
            return std::unexpected("HIR call requires a residual definition");
        const auto& definition = *expression->callee;
        if (expression->operands.size() != definition.parameters.size())
            return std::unexpected("HIR call argument count mismatch");
        std::vector<ValueId> arguments;
        arguments.reserve(expression->operands.size());
        for (const auto& operand : expression->operands) {
            auto value = lower_expression(operand);
            if (!value || !live_)
                return value;
            arguments.push_back(*value);
        }
        if (const auto found = context_.callees.find(&definition);
            found != context_.callees.end()) {
            return emit(type_of(expression->type),
                        CallOperation{found->second, std::move(arguments)},
                        expression->span);
        }
        // Reserve a unique symbol before lowering nested calls. Separate
        // lowerers keep SSA IDs, dominance caches and return scopes local.
        const auto slot = context_.output.functions.size();
        const auto name =
            "pagos." + definition.name + "." + std::to_string(slot);
        context_.output.functions.emplace_back();
        context_.callees.emplace(&definition, name);
        Lowerer callee(context_);
        callee.function_.name = name;
        callee.function_.internal = true;
        callee.function_.result_type = type_of(expression->type);
        callee.function_.parameters.reserve(definition.parameters.size());
        for (const auto& operand : definition.parameters) {
            const auto type = type_of(operand->type);
            const auto index =
                static_cast<std::uint32_t>(callee.function_.parameters.size());
            callee.function_.parameters.push_back(type);
            const auto parameter =
                callee.emit(type, ParameterOperation{index}, operand->span);
            callee.cache(operand.get(), parameter);
        }
        auto value = callee.lower_expression(definition.body);
        if (!value)
            return std::unexpected(value.error());
        if (!callee.live_)
            return std::unexpected("HIR call body has no continuation result");
        callee.block(callee.current_block_).terminator = Return{*value};
        context_.output.functions[slot] = std::move(callee.function_);
        return emit(type_of(expression->type),
                    CallOperation{name, std::move(arguments)},
                    expression->span);
    }

    Type type_of(const sema::Type& type) const {
        if (type.is_record()) {
            const auto found = context_.records.find(type.record_name);
            return found == context_.records.end() ? Type{Type::Invalid}
                                                   : found->second;
        }
        if (type.is_array()) {
            return Type::array(type_of(type.element_type()), type.length);
        }
        if (type.kind == sema::TypeKind::Integer) {
            return Type::integer(type.integer_type.width,
                                 type.integer_type.is_signed,
                                 type.integer_type.is_usize);
        }
        return type == sema::TypeKind::Bool ? Type::Bool : Type::Invalid;
    }

    static Constant to_constant(const hir::Constant& constant) {
        return constant;
    }

    static std::optional<BinaryOperator>
    binary_operation(syntax::BinaryOperator operation) {
        using enum syntax::BinaryOperator;
        switch (operation) {
        case Add:
            return BinaryOperator::Add;
        case Subtract:
            return BinaryOperator::Subtract;
        case Multiply:
            return BinaryOperator::Multiply;
        case Divide:
            return BinaryOperator::DivideChecked;
        case Remainder:
            return BinaryOperator::RemainderChecked;
        case BitAnd:
            return BinaryOperator::BitAnd;
        case BitOr:
            return BinaryOperator::BitOr;
        case BitXor:
            return BinaryOperator::BitXor;
        case ShiftLeft:
            return BinaryOperator::ShiftLeftChecked;
        case ShiftRight:
            return BinaryOperator::ShiftRightChecked;
        case Equal:
            return BinaryOperator::Equal;
        case NotEqual:
            return BinaryOperator::NotEqual;
        case Less:
            return BinaryOperator::Less;
        case LessEqual:
            return BinaryOperator::LessEqual;
        case Greater:
            return BinaryOperator::Greater;
        case GreaterEqual:
            return BinaryOperator::GreaterEqual;
        case LogicalAnd:
        case LogicalOr:
            return std::nullopt;
        }
        return std::nullopt;
    }

    Function function_;
    LoweringContext& context_;
    BlockId current_block_{};
    // Returned paths have no continuation value; callers must stop lowering
    // that path while this is false, even if lower_expression succeeded.
    bool live_{true};
    std::vector<ReturnTarget> return_targets_;
    ValueId next_value_{};
    std::vector<std::optional<BlockId>> parents_;
    std::unordered_map<const hir::Expr*, std::vector<CachedValue>> cache_;
};

} // namespace

std::expected<Module, std::string> lower(const hir::Module& module) {
    LoweringContext context;
    return Lowerer(context).run(module);
}

} // namespace pagos::mir
