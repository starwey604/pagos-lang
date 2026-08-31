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

bool has_residual_control(const hir::ExprPtr& expression) {
    if (!expression) {
        return false;
    }
    if (expression->kind == hir::Expr::Kind::RangeLoop) {
        return true;
    }
    return std::ranges::any_of(expression->operands, has_residual_control);
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
    return {.value = std::move(value),
            .returned = false,
            .effects = std::move(effects)};
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
        return analyze_for(static_cast<const syntax::ForStmt&>(statement));
    }
    return {};
}

StageAnalyzer::BlockResult
StageAnalyzer::analyze_for(const syntax::ForStmt& loop_statement) {
    const auto begin = analyze_expression(*loop_statement.begin);
    const auto end = analyze_expression(*loop_statement.end);
    if (!begin || !end) {
        return {};
    }
    if (begin->stage == hir::Stage::Static &&
        end->stage == hir::Stage::Static) {
        const auto begin_value =
            std::get<std::uint32_t>(begin->constant.value());
        const auto end_value = std::get<std::uint32_t>(end->constant.value());
        std::vector<hir::ExprPtr> effects;
        if (has_residual_control(begin)) {
            effects.push_back(begin);
        }
        if (has_residual_control(end)) {
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
    if (body_result.returned) {
        diagnostics_.report({
            .severity = source::Severity::Error,
            .code = "E5001",
            .message = "return from a Runtime loop is not implemented yet",
            .primary = {.span = loop_statement.span,
                        .message = "the loop trip count is Runtime"},
            .help = "move the return after the loop or make both bounds "
                    "Static",
        });
        return {};
    }

    auto loop = std::make_shared<hir::Expr>();
    loop->kind = hir::Expr::Kind::RangeLoop;
    loop->type = syntax::TypeKind::Void;
    loop->stage = hir::Stage::Runtime;
    loop->span = loop_statement.span;
    loop->trace = std::move(trace);
    loop->variable_name = loop_statement.variable;
    loop->operands = {begin, end, std::move(index)};
    std::ranges::move(body_result.effects, std::back_inserter(loop->operands));
    return {.effects = {std::move(loop)}};
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
        auto value =
            hir::make_constant(*result, type_of(expression), expression.span);
        if (has_residual_control(operand)) {
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
            auto value = hir::make_constant(left_value, syntax::TypeKind::Bool,
                                            expression.span);
            if (has_residual_control(left)) {
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
        if (has_residual_control(left)) {
            effects.push_back(std::move(left));
        }
        if (has_residual_control(right)) {
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
    auto block_result = analyze_block(*function.body);
    auto result =
        make_sequence(std::move(block_result.effects),
                      std::move(block_result.value), function.body->span);
    call_stack_.pop_back();
    scopes_.pop_back();
    if (key) {
        active_specializations_.erase(*key);
        if (result && result->stage == hir::Stage::Static &&
            !has_residual_control(result)) {
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
        auto selected_result = analyze_block(*selected);
        if (has_residual_control(condition)) {
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
    sequence->operands = std::move(effects);
    sequence->operands.push_back(std::move(value));
    return sequence;
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
