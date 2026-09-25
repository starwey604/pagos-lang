#include "pagos/stage/stage_analyzer.h"

#include "pagos/vm/evaluator.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <utility>

namespace pagos::stage {
namespace {

hir::RuntimeTrace first_trace(const hir::ExprPtr& left,
                              const hir::ExprPtr& right) {
    if (left && left->trace) {
        return left->trace.value();
    }
    return right->trace.value();
}

std::uint32_t parse_u32(const std::string& spelling) {
    std::string digits;
    digits.reserve(spelling.size());
    std::ranges::copy_if(spelling, std::back_inserter(digits),
                         [](char character) { return character != '_'; });
    std::uint32_t value{};
    (void)std::from_chars(digits.data(), digits.data() + digits.size(), value);
    return value;
}

bool has_residual_work(const hir::ExprPtr& expression) {
    if (!expression) {
        return false;
    }
    if (expression->stage == hir::Stage::Runtime || expression->may_return) {
        return true;
    }
    return std::ranges::any_of(expression->operands, has_residual_work);
}

struct ArrayProjection {
    std::optional<hir::Constant> constant;
    std::optional<hir::RuntimeTrace> trace;
};

std::optional<ArrayProjection> project_array_element(hir::ExprPtr array,
                                                     std::uint32_t offset) {
    std::vector<std::string> uses;
    // Only diagnostic aliases and ordered evaluation wrappers are transparent.
    // RuntimeBoundary, If, and ReturnScope deliberately stop projection.
    while (array) {
        if (array->kind == hir::Expr::Kind::Reference) {
            uses.push_back(array->trace->path.back());
            array = array->operands.front();
        } else if (array->kind == hir::Expr::Kind::Sequence) {
            array = array->operands.back();
        } else if (array->kind == hir::Expr::Kind::Array) {
            const auto& element = array->operands.at(offset);
            if (element->stage == hir::Stage::Static) {
                return ArrayProjection{.constant = element->constant};
            }
            auto trace = element->trace;
            trace->path.push_back("array element " + std::to_string(offset));
            for (auto use = uses.rbegin(); use != uses.rend(); ++use) {
                trace->path.push_back(*use);
            }
            return ArrayProjection{.trace = std::move(trace)};
        } else {
            break;
        }
    }
    return std::nullopt;
}

std::optional<hir::RuntimeTrace> return_dependency(const hir::ExprPtr& value) {
    if (!value || !value->may_return) {
        return std::nullopt;
    }
    if ((value->kind == hir::Expr::Kind::If ||
         value->kind == hir::Expr::Kind::RangeLoop ||
         value->kind == hir::Expr::Kind::Return) &&
        value->trace) {
        auto trace = value->trace;
        if (value->kind != hir::Expr::Kind::Return) {
            trace->path.push_back("return control");
        }
        return trace;
    }
    for (const auto& operand : value->operands) {
        if (auto trace = return_dependency(operand)) {
            return trace;
        }
    }
    return std::nullopt;
}

} // namespace

std::size_t StageAnalyzer::SpecializationKeyHash::operator()(
    const SpecializationKey& key) const noexcept {
    auto hash = std::hash<const syntax::Function*>{}(key.function);
    for (const auto& argument : key.arguments) {
        auto argument_hash = argument.index();
        std::visit(
            [&](const auto& value) {
                const auto mix = [&](std::size_t part) {
                    argument_hash ^= part + 0x9e3779b9U +
                                     (argument_hash << 6U) +
                                     (argument_hash >> 2U);
                };
                if constexpr (requires { value.size(); }) {
                    mix(value.size());
                    for (auto element : value) {
                        mix(static_cast<std::size_t>(element));
                    }
                } else {
                    mix(static_cast<std::size_t>(value));
                }
            },
            argument);
        hash ^= argument_hash + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
    }
    return hash;
}

std::unique_ptr<hir::Module>
StageAnalyzer::analyze(const syntax::Module& module) {
    stats_ = {};
    functions_.clear();
    scopes_.clear();
    call_stack_.clear();
    specialization_cache_.clear();
    active_specializations_.clear();
    fuel_exhausted_ = false;

    auto result = std::make_unique<hir::Module>();
    for (const auto& function : module.functions) {
        functions_.emplace(function->name, function.get());
        hir::FunctionSummary summary{.name = function->name,
                                     .result = function->result};
        for (const auto& parameter : function->parameters) {
            summary.parameters.push_back(parameter.type);
        }
        result->functions.push_back(std::move(summary));
    }

    scopes_.emplace_back();
    std::vector<hir::ExprPtr> effects;
    for (const auto& statement : module.statements) {
        auto statement_result = analyze_statement(*statement, result.get());
        std::ranges::move(statement_result.effects,
                          std::back_inserter(effects));
        if (statement_result.value) {
            result->result = statement_result.value;
        }
    }
    scopes_.pop_back();
    if (!effects.empty()) {
        auto value = result->result;
        if (!value) {
            value =
                hir::make_constant(std::uint32_t{0}, syntax::TypeKind::U32, {});
        }
        const auto span = value->span;
        result->result =
            make_sequence(std::move(effects), std::move(value), span);
    }
    return result;
}

StageAnalyzer::BlockResult
StageAnalyzer::analyze_block(const syntax::Block& block) {
    scopes_.emplace_back();
    std::vector<hir::ExprPtr> effects;
    for (const auto& statement : block.statements) {
        auto result = analyze_statement(*statement, nullptr);
        std::ranges::move(result.effects, std::back_inserter(effects));
        if (result.returned) {
            scopes_.pop_back();
            result.effects = std::move(effects);
            return result;
        }
    }
    auto value = block.tail ? analyze_expression(*block.tail) : nullptr;
    scopes_.pop_back();
    return {.value = value,
            .returned = block.tail && value && !value->falls_through,
            .effects = std::move(effects)};
}

StageAnalyzer::BlockResult
StageAnalyzer::analyze_statement(const syntax::Stmt& statement,
                                 hir::Module* output_module) {
    switch (statement.kind) {
    case syntax::Stmt::Kind::Binding: {
        auto value = analyze_binding(
            static_cast<const syntax::BindingStmt&>(statement), output_module);
        return {.value = value,
                .returned = value && !value->falls_through,
                .effects = has_residual_work(value) && value->falls_through
                               ? std::vector<hir::ExprPtr>{value}
                               : std::vector<hir::ExprPtr>{}};
    }
    case syntax::Stmt::Kind::Return: {
        const auto& return_statement =
            static_cast<const syntax::ReturnStmt&>(statement);
        auto value = analyze_expression(*return_statement.value);
        if (!value || !value->falls_through) {
            return {.value = value, .returned = true};
        }
        auto result = std::make_shared<hir::Expr>();
        result->kind = hir::Expr::Kind::Return;
        result->type = value->type;
        result->stage = value->stage;
        result->span = statement.span;
        result->trace = value->trace;
        result->falls_through = false;
        result->may_return = true;
        result->operands = {value};
        return {.value = result, .returned = true};
    }
    case syntax::Stmt::Kind::Expression: {
        const auto& expression_statement =
            static_cast<const syntax::ExpressionStmt&>(statement);
        auto value = analyze_expression(*expression_statement.expression);
        return {.value = value,
                .returned = value && !value->falls_through,
                .effects = has_residual_work(value) && value->falls_through
                               ? std::vector<hir::ExprPtr>{value}
                               : std::vector<hir::ExprPtr>{}};
    }
    case syntax::Stmt::Kind::For:
        return analyze_for(static_cast<const syntax::ForStmt&>(statement));
    }
    return {};
}

StageAnalyzer::BlockResult
StageAnalyzer::analyze_for(const syntax::ForStmt& loop_statement) {
    const auto begin = analyze_expression(*loop_statement.begin);
    if (!begin || !begin->falls_through) {
        return {.value = begin, .returned = true};
    }
    const auto end = analyze_expression(*loop_statement.end);
    if (!end) {
        return {};
    }
    if (!end->falls_through) {
        return {.value = make_sequence({begin}, end, loop_statement.span),
                .returned = true};
    }
    if (begin->stage == hir::Stage::Static &&
        end->stage == hir::Stage::Static) {
        const auto begin_value =
            std::get<std::uint32_t>(begin->constant.value());
        const auto end_value = std::get<std::uint32_t>(end->constant.value());
        std::vector<hir::ExprPtr> effects;
        if (has_residual_work(begin)) {
            effects.push_back(begin);
        }
        if (has_residual_work(end)) {
            effects.push_back(end);
        }
        for (std::uint32_t index = begin_value; index < end_value; ++index) {
            if (!consume_fuel(loop_statement.span)) {
                return {};
            }
            scopes_.emplace_back();
            define(loop_statement.variable,
                   hir::make_constant(index, syntax::TypeKind::U32,
                                      loop_statement.variable_span));
            auto body_result = analyze_block(*loop_statement.body);
            scopes_.pop_back();
            std::ranges::move(body_result.effects, std::back_inserter(effects));
            if (body_result.returned) {
                body_result.effects = std::move(effects);
                return body_result;
            }
            if (has_residual_work(body_result.value)) {
                effects.push_back(body_result.value);
            }
        }
        return {.effects = std::move(effects)};
    }

    auto trace = begin->stage == hir::Stage::Runtime ? begin->trace.value()
                                                     : end->trace.value();
    trace.path.push_back(loop_statement.variable);
    auto index = std::make_shared<hir::Expr>();
    index->kind = hir::Expr::Kind::LoopIndex;
    index->type = syntax::TypeKind::U32;
    index->stage = hir::Stage::Runtime;
    index->span = loop_statement.variable_span;
    index->trace = trace;
    index->variable_name = loop_statement.variable;

    scopes_.emplace_back();
    define(loop_statement.variable, index);
    auto body_result = analyze_block(*loop_statement.body);
    scopes_.pop_back();

    auto loop = std::make_shared<hir::Expr>();
    loop->kind = hir::Expr::Kind::RangeLoop;
    loop->type = syntax::TypeKind::Void;
    loop->stage = hir::Stage::Runtime;
    loop->span = loop_statement.span;
    loop->trace = std::move(trace);
    loop->variable_name = loop_statement.variable;
    loop->operands = {begin, end, std::move(index)};
    std::ranges::move(body_result.effects, std::back_inserter(loop->operands));
    if (has_residual_work(body_result.value)) {
        loop->operands.push_back(body_result.value);
    }
    loop->may_return =
        std::ranges::any_of(loop->operands, [](const auto& operand) {
            return operand->may_return;
        });
    return {.effects = {std::move(loop)}};
}

hir::ExprPtr StageAnalyzer::analyze_binding(const syntax::BindingStmt& binding,
                                            hir::Module* output_module) {
    auto value = analyze_expression(*binding.initializer);
    if (!value || !value->falls_through) {
        return value;
    }

    if (binding.binding_kind == syntax::BindingKind::Runtime) {
        auto runtime = std::make_shared<hir::Expr>();
        runtime->kind = hir::Expr::Kind::RuntimeBoundary;
        runtime->type = type_of(*binding.initializer);
        runtime->stage = hir::Stage::Runtime;
        runtime->may_return = value->may_return;
        runtime->span = binding.span;
        runtime->operands.push_back(value);
        runtime->trace = hir::RuntimeTrace{
            .origin_span = binding.name_span,
            .origin_name = binding.name,
            .path = {binding.name},
        };
        value = runtime;
    } else if (value->stage == hir::Stage::Runtime) {
        value = hir::with_trace_step(value, binding.name);
    }

    if (binding.binding_kind == syntax::BindingKind::Static &&
        value->stage == hir::Stage::Runtime && value->trace) {
        report_static_failure(binding, *value->trace);
    }

    define(binding.name, value);
    if (output_module) {
        output_module->bindings.push_back(
            {.name = binding.name,
             .name_span = binding.name_span,
             .type = type_of(*binding.initializer),
             .stage = value->stage,
             .value = value,
             .trace = value->trace});
    }
    return value;
}

hir::ExprPtr StageAnalyzer::analyze_expression(const syntax::Expr& expression) {
    if (!consume_fuel(expression.span)) {
        return nullptr;
    }
    switch (expression.kind) {
    case syntax::Expr::Kind::Integer: {
        const auto& integer =
            static_cast<const syntax::IntegerExpr&>(expression);
        return hir::make_constant(parse_u32(integer.spelling),
                                  syntax::TypeKind::U32, expression.span);
    }
    case syntax::Expr::Kind::Boolean: {
        const auto& boolean =
            static_cast<const syntax::BooleanExpr&>(expression);
        return hir::make_constant(boolean.value, syntax::TypeKind::Bool,
                                  expression.span);
    }
    case syntax::Expr::Kind::Name: {
        const auto& name = static_cast<const syntax::NameExpr&>(expression);
        return lookup(name.name);
    }
    case syntax::Expr::Kind::Unary:
        return analyze_unary(static_cast<const syntax::UnaryExpr&>(expression));
    case syntax::Expr::Kind::Binary:
        return analyze_binary(
            static_cast<const syntax::BinaryExpr&>(expression));
    case syntax::Expr::Kind::Call:
        return analyze_call(static_cast<const syntax::CallExpr&>(expression));
    case syntax::Expr::Kind::If:
        return analyze_if(static_cast<const syntax::IfExpr&>(expression));
    case syntax::Expr::Kind::Array:
        return analyze_array(static_cast<const syntax::ArrayExpr&>(expression));
    case syntax::Expr::Kind::Index:
        return analyze_index(static_cast<const syntax::IndexExpr&>(expression));
    }
    return nullptr;
}

hir::ExprPtr StageAnalyzer::analyze_array(const syntax::ArrayExpr& expression) {
    std::vector<hir::ExprPtr> elements;
    elements.reserve(expression.elements.size());
    std::optional<hir::RuntimeTrace> trace;
    for (const auto& element : expression.elements) {
        auto value = analyze_expression(*element);
        if (!value) {
            return nullptr;
        }
        if (!value->falls_through) {
            return make_sequence(std::move(elements), value, expression.span);
        }
        if (value->stage == hir::Stage::Runtime && !trace) {
            trace = value->trace;
        }
        elements.push_back(std::move(value));
    }
    const auto type = type_of(expression);
    if (trace) {
        trace->path.push_back("array element");
        return make_runtime(hir::Expr::Kind::Array, type, expression.span,
                            std::move(elements), std::move(*trace));
    }
    hir::Constant constant;
    if (type.element_type() == syntax::TypeKind::Bool) {
        std::vector<bool> values;
        values.reserve(elements.size());
        for (const auto& element : elements) {
            values.push_back(std::get<bool>(*element->constant));
        }
        constant = std::move(values);
    } else {
        std::vector<std::uint32_t> values;
        values.reserve(elements.size());
        for (const auto& element : elements) {
            values.push_back(std::get<std::uint32_t>(*element->constant));
        }
        constant = std::move(values);
    }
    std::erase_if(elements, [](const auto& element) {
        return !has_residual_work(element);
    });
    return make_sequence(
        std::move(elements),
        hir::make_constant(std::move(constant), type, expression.span),
        expression.span);
}

hir::ExprPtr StageAnalyzer::analyze_index(const syntax::IndexExpr& expression) {
    auto array = analyze_expression(*expression.array);
    if (!array || !array->falls_through) {
        return array;
    }
    auto index = analyze_expression(*expression.index);
    if (!index) {
        return nullptr;
    }
    if (!index->falls_through) {
        return make_sequence({array}, index, expression.span);
    }
    std::optional<hir::RuntimeTrace> selected_trace;
    if (index->stage == hir::Stage::Static) {
        const auto offset = std::get<std::uint32_t>(*index->constant);
        if (offset >= array->type.length) {
            diagnostics_.error("E4007", "array index out of bounds",
                               expression.index->span,
                               "index " + std::to_string(offset) + ", length " +
                                   std::to_string(array->type.length));
            return nullptr;
        }
        std::optional<hir::Constant> value;
        if (array->stage == hir::Stage::Static) {
            if (array->type.element_type() == syntax::TypeKind::Bool) {
                value = static_cast<bool>(
                    std::get<std::vector<bool>>(*array->constant)[offset]);
            } else {
                value = std::get<std::vector<std::uint32_t>>(
                    *array->constant)[offset];
            }
        } else if (auto selected = project_array_element(array, offset)) {
            value = std::move(selected->constant);
            selected_trace = std::move(selected->trace);
        }
        if (value) {
            std::vector<hir::ExprPtr> effects;
            if (has_residual_work(array)) {
                effects.push_back(array);
            }
            if (has_residual_work(index)) {
                effects.push_back(index);
            }
            return make_sequence(std::move(effects),
                                 hir::make_constant(std::move(*value),
                                                    type_of(expression),
                                                    expression.span),
                                 expression.span);
        }
    }
    auto trace =
        selected_trace ? std::move(*selected_trace) : first_trace(array, index);
    trace.path.push_back("array index");
    return make_runtime(hir::Expr::Kind::Index, type_of(expression),
                        expression.span, {array, index}, std::move(trace));
}

hir::ExprPtr StageAnalyzer::analyze_unary(const syntax::UnaryExpr& expression) {
    auto operand = analyze_expression(*expression.operand);
    if (!operand || !operand->falls_through) {
        return operand;
    }
    if (operand->stage == hir::Stage::Static) {
        const auto result = vm::Evaluator::unary(expression.operation,
                                                 operand->constant.value());
        if (!result) {
            diagnostics_.error(result.error().code, result.error().message,
                               expression.span);
            return nullptr;
        }
        auto value =
            hir::make_constant(*result, type_of(expression), expression.span);
        if (has_residual_work(operand)) {
            return make_sequence({std::move(operand)}, std::move(value),
                                 expression.span);
        }
        return value;
    }
    return make_runtime(hir::Expr::Kind::Unary, type_of(expression),
                        expression.span, {operand}, operand->trace.value(),
                        expression.operation);
}

hir::ExprPtr
StageAnalyzer::analyze_binary(const syntax::BinaryExpr& expression) {
    auto left = analyze_expression(*expression.left);
    if (!left || !left->falls_through) {
        return left;
    }

    using enum syntax::BinaryOperator;
    if (left->stage == hir::Stage::Static &&
        (expression.operation == LogicalAnd ||
         expression.operation == LogicalOr)) {
        const auto left_value = std::get<bool>(left->constant.value());
        if ((expression.operation == LogicalAnd && !left_value) ||
            (expression.operation == LogicalOr && left_value)) {
            auto value = hir::make_constant(left_value, syntax::TypeKind::Bool,
                                            expression.span);
            if (has_residual_work(left)) {
                return make_sequence({std::move(left)}, std::move(value),
                                     expression.span);
            }
            return value;
        }
    }

    auto right = analyze_expression(*expression.right);
    if (!right) {
        return nullptr;
    }
    if (!right->falls_through && expression.operation != LogicalAnd &&
        expression.operation != LogicalOr) {
        return make_sequence({left}, right, expression.span);
    }
    if (!right->falls_through && left->stage == hir::Stage::Static) {
        return make_sequence({left}, right, expression.span);
    }
    if (left->stage == hir::Stage::Static &&
        right->stage == hir::Stage::Static) {
        const auto result =
            vm::Evaluator::binary(expression.operation, left->constant.value(),
                                  right->constant.value());
        if (!result) {
            diagnostics_.error(result.error().code, result.error().message,
                               expression.span);
            return nullptr;
        }
        auto value =
            hir::make_constant(*result, type_of(expression), expression.span);
        std::vector<hir::ExprPtr> effects;
        if (has_residual_work(left)) {
            effects.push_back(std::move(left));
        }
        if (has_residual_work(right)) {
            effects.push_back(std::move(right));
        }
        return make_sequence(std::move(effects), std::move(value),
                             expression.span);
    }

    if (expression.operation == LogicalAnd ||
        expression.operation == LogicalOr) {
        const auto fallback =
            hir::make_constant(expression.operation == LogicalOr,
                               syntax::TypeKind::Bool, expression.span);
        const auto then_value =
            expression.operation == LogicalAnd ? right : fallback;
        const auto else_value =
            expression.operation == LogicalAnd ? fallback : right;
        const auto trace =
            left->trace ? left->trace.value() : right->trace.value();
        return make_runtime(hir::Expr::Kind::If, syntax::TypeKind::Bool,
                            expression.span, {left, then_value, else_value},
                            trace);
    }

    return make_runtime(hir::Expr::Kind::Binary, type_of(expression),
                        expression.span, {left, right},
                        first_trace(left, right), std::nullopt,
                        expression.operation);
}

hir::ExprPtr StageAnalyzer::analyze_call(const syntax::CallExpr& expression) {
    if (expression.callee == "external_input") {
        return make_runtime(hir::Expr::Kind::ExternalInput,
                            syntax::TypeKind::U32, expression.span, {},
                            {.origin_span = expression.callee_span,
                             .origin_name = "external_input",
                             .path = {"external_input"}});
    }

    const auto function_iterator = functions_.find(expression.callee);
    if (function_iterator == functions_.end()) {
        return nullptr;
    }
    std::vector<hir::ExprPtr> arguments;
    arguments.reserve(expression.arguments.size());
    for (const auto& argument : expression.arguments) {
        auto value = analyze_expression(*argument);
        if (!value) {
            return nullptr;
        }
        if (!value->falls_through) {
            return make_sequence(std::move(arguments), value, expression.span);
        }
        arguments.push_back(std::move(value));
    }

    const auto& function = *function_iterator->second;
    // Arguments execute left-to-right, including on a specialization hit and
    // when the callee does not use their values.
    std::vector<hir::ExprPtr> argument_work;
    for (const auto& argument : arguments) {
        if (has_residual_work(argument)) {
            argument_work.push_back(argument);
        }
    }
    auto key = specialization_key(function, arguments);
    if (key) {
        if (const auto cached = specialization_cache_.find(*key);
            cached != specialization_cache_.end()) {
            ++stats_.cache_hits;
            return make_sequence(std::move(argument_work), cached->second,
                                 expression.span);
        }
        if (active_specializations_.contains(*key)) {
            diagnostics_.report({
                .severity = source::Severity::Error,
                .code = "E4003",
                .message = "recursive specialization cycle in `" +
                           expression.callee + "`",
                .primary = {.span = expression.span,
                            .message = "the same Static arguments recur"},
                .help = "change the recursive arguments or add a terminating "
                        "Static branch",
            });
            return nullptr;
        }
        if (stats_.specializations >= limits_.specializations) {
            diagnostics_.report({
                .severity = source::Severity::Error,
                .code = "E4006",
                .message = "compile-time specialization limit of " +
                           std::to_string(limits_.specializations) +
                           " exceeded",
                .primary = {.span = expression.span,
                            .message = std::to_string(stats_.specializations) +
                                       " specializations already created"},
                .help = "increase --max-specializations or reduce distinct "
                        "Static call arguments",
            });
            return nullptr;
        }
    } else if (std::ranges::find(call_stack_, expression.callee) !=
               call_stack_.end()) {
        diagnostics_.report({
            .severity = source::Severity::Error,
            .code = "E4003",
            .message = "Runtime-recursive call cannot be specialized",
            .primary = {.span = expression.span,
                        .message = "recursive arguments are not all Static"},
            .help = "make recursive control and arguments Static",
        });
        return nullptr;
    }
    if (call_stack_.size() >= limits_.recursion_depth) {
        diagnostics_.report({
            .severity = source::Severity::Error,
            .code = "E4005",
            .message = "compile-time recursion depth limit of " +
                       std::to_string(limits_.recursion_depth) + " exceeded",
            .primary = {.span = expression.span,
                        .message = "depth " +
                                   std::to_string(call_stack_.size()) +
                                   " cannot enter `" + expression.callee + "`"},
            .help = "increase --max-recursion-depth or reduce recursive depth",
        });
        return nullptr;
    }

    if (key) {
        active_specializations_.insert(*key);
        ++stats_.specializations;
    }
    scopes_.emplace_back();
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
        auto value = arguments[index];
        if (value->stage == hir::Stage::Runtime) {
            value = hir::with_trace_step(
                value, function.name + "." + function.parameters[index].name);
        }
        define(function.parameters[index].name, std::move(value));
    }
    call_stack_.push_back(function.name);
    stats_.maximum_recursion_depth =
        std::max(stats_.maximum_recursion_depth, call_stack_.size());
    auto block_result = analyze_block(*function.body);
    auto result =
        make_sequence(std::move(block_result.effects),
                      std::move(block_result.value), function.body->span);
    if (result && result->may_return) {
        if (auto resolved = resolve_return(result)) {
            result = std::move(resolved);
        } else {
            auto region = std::make_shared<hir::Expr>();
            region->kind = hir::Expr::Kind::ReturnScope;
            region->type = function.result;
            region->stage = hir::Stage::Runtime;
            region->span = expression.span;
            region->trace = return_dependency(result);
            region->operands = {result};
            result = region;
        }
    }
    call_stack_.pop_back();
    scopes_.pop_back();
    if (key) {
        active_specializations_.erase(*key);
        if (result && result->stage == hir::Stage::Static &&
            !has_residual_work(result)) {
            specialization_cache_.emplace(std::move(*key), result);
        }
    }
    return make_sequence(std::move(argument_work), std::move(result),
                         expression.span);
}

hir::ExprPtr StageAnalyzer::analyze_if(const syntax::IfExpr& expression) {
    auto condition = analyze_expression(*expression.condition);
    if (!condition || !condition->falls_through) {
        return condition;
    }
    if (condition->stage == hir::Stage::Static) {
        const auto selected = std::get<bool>(condition->constant.value())
                                  ? expression.then_block.get()
                                  : expression.else_block.get();
        auto selected_result = analyze_block(*selected);
        if (has_residual_work(condition)) {
            selected_result.effects.insert(selected_result.effects.begin(),
                                           std::move(condition));
        }
        return make_sequence(std::move(selected_result.effects),
                             std::move(selected_result.value), expression.span);
    }

    auto then_result = analyze_block(*expression.then_block);
    auto else_result = analyze_block(*expression.else_block);
    auto then_value = make_sequence(std::move(then_result.effects),
                                    std::move(then_result.value),
                                    expression.then_block->span);
    auto else_value = make_sequence(std::move(else_result.effects),
                                    std::move(else_result.value),
                                    expression.else_block->span);
    if (!then_value || !else_value) {
        return nullptr;
    }
    return make_runtime(hir::Expr::Kind::If, type_of(expression),
                        expression.span, {condition, then_value, else_value},
                        condition->trace.value());
}

void StageAnalyzer::define(const std::string& name, hir::ExprPtr value) {
    scopes_.back().insert_or_assign(name, std::move(value));
}

hir::ExprPtr StageAnalyzer::lookup(const std::string& name) const {
    for (auto iterator = scopes_.rbegin(); iterator != scopes_.rend();
         ++iterator) {
        if (const auto found = iterator->find(name); found != iterator->end()) {
            return found->second;
        }
    }
    return nullptr;
}

hir::ExprPtr StageAnalyzer::make_runtime(
    hir::Expr::Kind kind, syntax::Type type, source::Span span,
    std::vector<hir::ExprPtr> operands, hir::RuntimeTrace trace,
    std::optional<syntax::UnaryOperator> unary,
    std::optional<syntax::BinaryOperator> binary) const {
    auto expression = std::make_shared<hir::Expr>();
    expression->kind = kind;
    expression->type = type;
    expression->stage = hir::Stage::Runtime;
    expression->span = span;
    expression->trace = std::move(trace);
    expression->unary_operation = unary;
    expression->binary_operation = binary;
    expression->operands = std::move(operands);
    expression->may_return =
        std::ranges::any_of(expression->operands, [](const auto& operand) {
            return operand->may_return;
        });
    if (kind == hir::Expr::Kind::If) {
        expression->falls_through = expression->operands[0]->falls_through &&
                                    (expression->operands[1]->falls_through ||
                                     expression->operands[2]->falls_through);
    } else {
        expression->falls_through =
            std::ranges::all_of(expression->operands, [](const auto& operand) {
                return operand->falls_through;
            });
    }
    return expression;
}

hir::ExprPtr StageAnalyzer::make_sequence(std::vector<hir::ExprPtr> effects,
                                          hir::ExprPtr value,
                                          source::Span span) const {
    if (effects.empty() || !value) {
        return value;
    }
    auto sequence = std::make_shared<hir::Expr>();
    sequence->kind = hir::Expr::Kind::Sequence;
    sequence->type = value->type;
    sequence->stage = value->stage;
    sequence->span = span;
    sequence->trace = value->trace;
    sequence->constant = value->constant;
    sequence->falls_through = value->falls_through;
    sequence->operands = std::move(effects);
    sequence->operands.push_back(std::move(value));
    sequence->may_return =
        std::ranges::any_of(sequence->operands, [](const auto& operand) {
            return operand->may_return;
        });
    return sequence;
}

hir::ExprPtr StageAnalyzer::resolve_return(const hir::ExprPtr& value) const {
    // Remove a terminal return only when no earlier operation can return.
    // Otherwise its value must join the other exits in a ReturnScope.
    if (!value->may_return) {
        return value;
    }
    if (value->kind == hir::Expr::Kind::Return &&
        !value->operands[0]->may_return) {
        return value->operands[0];
    }
    if (value->kind == hir::Expr::Kind::Sequence) {
        auto effects = value->operands;
        auto tail = effects.back();
        effects.pop_back();
        if (std::ranges::none_of(effects, [](const auto& operand) {
                return operand->may_return;
            })) {
            if (auto resolved = resolve_return(tail)) {
                return make_sequence(std::move(effects), resolved, value->span);
            }
        }
    }
    return nullptr;
}

void StageAnalyzer::report_static_failure(const syntax::BindingStmt& binding,
                                          const hir::RuntimeTrace& trace) {
    source::Diagnostic diagnostic{
        .severity = source::Severity::Error,
        .code = "E2001",
        .message = "static binding `" + binding.name + "` was thawed",
        .primary = {.span = binding.name_span,
                    .message = "requires a Static value"},
        .related = {{.span = trace.origin_span,
                     .message = "Runtime source `" + trace.origin_name + "`",
                     .heading = "runtime source introduced here"}},
        .help = "remove `static` or remove the Runtime dependency",
        .dependency_path = trace.path,
    };
    diagnostics_.report(std::move(diagnostic));
}

syntax::Type StageAnalyzer::type_of(const syntax::Expr& expression) const {
    if (const auto iterator = types_.find(&expression);
        iterator != types_.end()) {
        return iterator->second;
    }
    return syntax::TypeKind::Error;
}

bool StageAnalyzer::consume_fuel(source::Span span) {
    if (stats_.fuel_consumed < limits_.fuel) {
        ++stats_.fuel_consumed;
        return true;
    }
    if (!fuel_exhausted_) {
        diagnostics_.report({
            .severity = source::Severity::Error,
            .code = "E4004",
            .message = "compile-time evaluation fuel exhausted after " +
                       std::to_string(stats_.fuel_consumed) + " steps",
            .primary = {.span = span,
                        .message = "configured limit is " +
                                   std::to_string(limits_.fuel)},
            .help = "increase --max-fuel or simplify compile-time evaluation",
        });
        fuel_exhausted_ = true;
    }
    return false;
}

std::optional<StageAnalyzer::SpecializationKey>
StageAnalyzer::specialization_key(
    const syntax::Function& function,
    const std::vector<hir::ExprPtr>& arguments) const {
    SpecializationKey key{.function = &function};
    key.arguments.reserve(arguments.size());
    for (const auto& argument : arguments) {
        if (!argument || argument->stage != hir::Stage::Static ||
            !argument->constant) {
            return std::nullopt;
        }
        key.arguments.push_back(argument->constant.value());
    }
    return key;
}

} // namespace pagos::stage
