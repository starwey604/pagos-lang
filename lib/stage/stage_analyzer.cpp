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

} // namespace

std::size_t StageAnalyzer::SpecializationKeyHash::operator()(
    const SpecializationKey& key) const noexcept {
    auto hash = std::hash<const syntax::Function*>{}(key.function);
    for (const auto& argument : key.arguments) {
        const auto argument_hash = std::hash<hir::Constant>{}(argument);
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
    for (const auto& statement : module.statements) {
        const auto statement_result =
            analyze_statement(*statement, result.get());
        if (statement_result.value) {
            result->result = statement_result.value;
        }
    }
    scopes_.pop_back();
    return result;
}

StageAnalyzer::BlockResult
StageAnalyzer::analyze_block(const syntax::Block& block) {
    scopes_.emplace_back();
    for (const auto& statement : block.statements) {
        auto result = analyze_statement(*statement, nullptr);
        if (result.returned) {
            scopes_.pop_back();
            return result;
        }
    }
    auto value = block.tail ? analyze_expression(*block.tail) : nullptr;
    scopes_.pop_back();
    return {.value = std::move(value), .returned = false};
}

StageAnalyzer::BlockResult
StageAnalyzer::analyze_statement(const syntax::Stmt& statement,
                                 hir::Module* output_module) {
    switch (statement.kind) {
    case syntax::Stmt::Kind::Binding:
        return {.value = analyze_binding(
                    static_cast<const syntax::BindingStmt&>(statement),
                    output_module),
                .returned = false};
    case syntax::Stmt::Kind::Return: {
        const auto& return_statement =
            static_cast<const syntax::ReturnStmt&>(statement);
        return {.value = analyze_expression(*return_statement.value),
                .returned = true};
    }
    case syntax::Stmt::Kind::Expression: {
        const auto& expression_statement =
            static_cast<const syntax::ExpressionStmt&>(statement);
        return {.value = analyze_expression(*expression_statement.expression),
                .returned = false};
    }
    case syntax::Stmt::Kind::For:
        diagnostics_.error(
            "E5001", "range-loop residualization is not implemented yet",
            statement.span, "range loops are scheduled after Milestone 1");
        return {};
    }
    return {};
}

hir::ExprPtr StageAnalyzer::analyze_binding(const syntax::BindingStmt& binding,
                                            hir::Module* output_module) {
    auto value = analyze_expression(*binding.initializer);
    if (!value) {
        return nullptr;
    }

    if (binding.binding_kind == syntax::BindingKind::Runtime) {
        auto runtime = std::make_shared<hir::Expr>();
        runtime->kind = hir::Expr::Kind::RuntimeBoundary;
        runtime->type = type_of(*binding.initializer);
        runtime->stage = hir::Stage::Runtime;
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
    }
    return nullptr;
}

hir::ExprPtr StageAnalyzer::analyze_unary(const syntax::UnaryExpr& expression) {
    auto operand = analyze_expression(*expression.operand);
    if (!operand) {
        return nullptr;
    }
    if (operand->stage == hir::Stage::Static) {
        const auto result = vm::Evaluator::unary(expression.operation,
                                                 operand->constant.value());
        if (!result) {
            diagnostics_.error(result.error().code, result.error().message,
                               expression.span);
            return nullptr;
        }
        return hir::make_constant(*result, type_of(expression),
                                  expression.span);
    }
    return make_runtime(hir::Expr::Kind::Unary, type_of(expression),
                        expression.span, {operand}, operand->trace.value(),
                        expression.operation);
}

hir::ExprPtr
StageAnalyzer::analyze_binary(const syntax::BinaryExpr& expression) {
    auto left = analyze_expression(*expression.left);
    if (!left) {
        return nullptr;
    }

    using enum syntax::BinaryOperator;
    if (left->stage == hir::Stage::Static &&
        (expression.operation == LogicalAnd ||
         expression.operation == LogicalOr)) {
        const auto left_value = std::get<bool>(left->constant.value());
        if ((expression.operation == LogicalAnd && !left_value) ||
            (expression.operation == LogicalOr && left_value)) {
            return hir::make_constant(left_value, syntax::TypeKind::Bool,
                                      expression.span);
        }
    }

    auto right = analyze_expression(*expression.right);
    if (!right) {
        return nullptr;
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
        return hir::make_constant(*result, type_of(expression),
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
        arguments.push_back(analyze_expression(*argument));
    }
    if (std::ranges::any_of(arguments,
                            [](const hir::ExprPtr& value) { return !value; })) {
        return nullptr;
    }

    const auto& function = *function_iterator->second;
    auto key = specialization_key(function, arguments);
    if (key) {
        if (const auto cached = specialization_cache_.find(*key);
            cached != specialization_cache_.end()) {
            ++stats_.cache_hits;
            return cached->second;
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
    auto result = analyze_block(*function.body).value;
    call_stack_.pop_back();
    scopes_.pop_back();
    if (key) {
        active_specializations_.erase(*key);
        if (result && result->stage == hir::Stage::Static) {
            specialization_cache_.emplace(std::move(*key), result);
        }
    }
    return result;
}

hir::ExprPtr StageAnalyzer::analyze_if(const syntax::IfExpr& expression) {
    auto condition = analyze_expression(*expression.condition);
    if (!condition) {
        return nullptr;
    }
    if (condition->stage == hir::Stage::Static) {
        const auto selected = std::get<bool>(condition->constant.value())
                                  ? expression.then_block.get()
                                  : expression.else_block.get();
        return analyze_block(*selected).value;
    }

    auto then_value = analyze_block(*expression.then_block).value;
    auto else_value = analyze_block(*expression.else_block).value;
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
    hir::Expr::Kind kind, syntax::TypeKind type, source::Span span,
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
    return expression;
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

syntax::TypeKind StageAnalyzer::type_of(const syntax::Expr& expression) const {
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
