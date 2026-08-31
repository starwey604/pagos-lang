#include "pagos/sema/type_checker.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>

namespace pagos::sema {
namespace {

bool compatible(syntax::TypeKind expected, syntax::TypeKind actual) {
    return expected == syntax::TypeKind::Error ||
           actual == syntax::TypeKind::Error || expected == actual;
}

bool valid_u32_literal(const std::string& spelling) {
    std::string digits;
    digits.reserve(spelling.size());
    std::ranges::copy_if(spelling, std::back_inserter(digits),
                         [](char character) { return character != '_'; });
    std::uint64_t value{};
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
    return result.ec == std::errc{} &&
           result.ptr == digits.data() + digits.size() &&
           value <= std::numeric_limits<std::uint32_t>::max();
}

} // namespace

bool TypeChecker::check(syntax::Module& module) {
    collect_functions(module);
    for (auto& function : module.functions) {
        check_function(*function);
    }

    scopes_.emplace_back();
    bool saw_return = false;
    for (auto& statement : module.statements) {
        check_statement(*statement, syntax::TypeKind::Void, saw_return);
    }
    scopes_.pop_back();
    return !diagnostics_.has_error();
}

void TypeChecker::collect_functions(syntax::Module& module) {
    for (auto& function : module.functions) {
        if (function->name == "external_input") {
            diagnostics_.error(
                "E1004", "reserved function name `external_input`",
                function->name_span, "this name is a runtime input intrinsic");
            continue;
        }
        const auto [iterator, inserted] =
            functions_.emplace(function->name, function.get());
        if (!inserted) {
            diagnostics_.error("E1004",
                               "duplicate function `" + function->name + "`",
                               function->name_span, "previously declared");
        }
        (void)iterator;
    }
}

void TypeChecker::check_function(syntax::Function& function) {
    scopes_.emplace_back();
    for (const auto& parameter : function.parameters) {
        define(parameter.name, parameter.type, parameter.name_span);
    }
    const auto previous_return_type = current_return_type_;
    current_return_type_ = function.result;
    bool saw_return = false;
    const auto tail_type =
        check_block(*function.body, function.result, saw_return);
    if (tail_type != syntax::TypeKind::Void &&
        !compatible(function.result, tail_type)) {
        type_mismatch(function.body->tail->span, function.result, tail_type,
                      "function tail expression");
    }
    if (!saw_return && tail_type == syntax::TypeKind::Void) {
        diagnostics_.error(
            "E1007", "function `" + function.name + "` has no result",
            function.name_span, "add `return` or a block tail expression");
    }
    current_return_type_ = previous_return_type;
    scopes_.pop_back();
}

syntax::TypeKind TypeChecker::check_block(syntax::Block& block,
                                          syntax::TypeKind expected_return,
                                          bool& saw_return) {
    scopes_.emplace_back();
    for (auto& statement : block.statements) {
        check_statement(*statement, expected_return, saw_return);
    }
    auto type = syntax::TypeKind::Void;
    if (block.tail) {
        type = check_expression(*block.tail);
    } else if (saw_return) {
        type = expected_return;
    }
    scopes_.pop_back();
    return type;
}

void TypeChecker::check_statement(syntax::Stmt& statement,
                                  syntax::TypeKind expected_return,
                                  bool& saw_return) {
    switch (statement.kind) {
    case syntax::Stmt::Kind::Binding: {
        auto& binding = static_cast<syntax::BindingStmt&>(statement);
        const auto initializer_type = check_expression(*binding.initializer);
        const auto binding_type = binding.annotation.value_or(initializer_type);
        if (binding.annotation &&
            !compatible(*binding.annotation, initializer_type)) {
            type_mismatch(binding.initializer->span, *binding.annotation,
                          initializer_type,
                          "initializer for `" + binding.name + "`");
        }
        define(binding.name, binding_type, binding.name_span);
        break;
    }
    case syntax::Stmt::Kind::Return: {
        auto& return_statement = static_cast<syntax::ReturnStmt&>(statement);
        const auto actual = check_expression(*return_statement.value);
        if (expected_return == syntax::TypeKind::Void) {
            diagnostics_.error("E1007", "`return` outside a function",
                               return_statement.span);
        } else if (!compatible(expected_return, actual)) {
            type_mismatch(return_statement.value->span, expected_return, actual,
                          "return expression");
        }
        saw_return = true;
        break;
    }
    case syntax::Stmt::Kind::Expression: {
        auto& expression_statement =
            static_cast<syntax::ExpressionStmt&>(statement);
        (void)check_expression(*expression_statement.expression);
        break;
    }
    case syntax::Stmt::Kind::For: {
        auto& for_statement = static_cast<syntax::ForStmt&>(statement);
        const auto begin_type = check_expression(*for_statement.begin);
        const auto end_type = check_expression(*for_statement.end);
        if (!compatible(syntax::TypeKind::U32, begin_type)) {
            type_mismatch(for_statement.begin->span, syntax::TypeKind::U32,
                          begin_type, "range start");
        }
        if (!compatible(syntax::TypeKind::U32, end_type)) {
            type_mismatch(for_statement.end->span, syntax::TypeKind::U32,
                          end_type, "range end");
        }
        scopes_.emplace_back();
        define(for_statement.variable, syntax::TypeKind::U32,
               for_statement.variable_span);
        (void)check_block(*for_statement.body, expected_return, saw_return);
        scopes_.pop_back();
        break;
    }
    }
}

syntax::TypeKind TypeChecker::check_expression(syntax::Expr& expression) {
    syntax::TypeKind type = syntax::TypeKind::Error;
    switch (expression.kind) {
    case syntax::Expr::Kind::Integer: {
        const auto& integer = static_cast<syntax::IntegerExpr&>(expression);
        if (!valid_u32_literal(integer.spelling)) {
            diagnostics_.error("E1005", "integer literal does not fit `u32`",
                               integer.span, "no truncation is performed");
        } else {
            type = syntax::TypeKind::U32;
        }
        break;
    }
    case syntax::Expr::Kind::Boolean:
        type = syntax::TypeKind::Bool;
        break;
    case syntax::Expr::Kind::Name: {
        const auto& name = static_cast<syntax::NameExpr&>(expression);
        type = lookup(name.name, name.span);
        break;
    }
    case syntax::Expr::Kind::Unary: {
        auto& unary = static_cast<syntax::UnaryExpr&>(expression);
        const auto operand_type = check_expression(*unary.operand);
        if (!compatible(syntax::TypeKind::Bool, operand_type)) {
            type_mismatch(unary.operand->span, syntax::TypeKind::Bool,
                          operand_type, "operand of `!`");
        } else {
            type = syntax::TypeKind::Bool;
        }
        break;
    }
    case syntax::Expr::Kind::Binary:
        type = check_binary(static_cast<syntax::BinaryExpr&>(expression));
        break;
    case syntax::Expr::Kind::Call:
        type = check_call(static_cast<syntax::CallExpr&>(expression));
        break;
    case syntax::Expr::Kind::If:
        type = check_if(static_cast<syntax::IfExpr&>(expression));
        break;
    }
    types_.insert_or_assign(&expression, type);
    return type;
}

syntax::TypeKind TypeChecker::check_binary(syntax::BinaryExpr& expression) {
    const auto left = check_expression(*expression.left);
    const auto right = check_expression(*expression.right);
    using enum syntax::BinaryOperator;
    const auto operation = expression.operation;
    if (operation == LogicalAnd || operation == LogicalOr) {
        if (!compatible(syntax::TypeKind::Bool, left)) {
            type_mismatch(expression.left->span, syntax::TypeKind::Bool, left,
                          "left logical operand");
        }
        if (!compatible(syntax::TypeKind::Bool, right)) {
            type_mismatch(expression.right->span, syntax::TypeKind::Bool, right,
                          "right logical operand");
        }
        return syntax::TypeKind::Bool;
    }
    if (operation == Equal || operation == NotEqual) {
        if (!compatible(left, right)) {
            type_mismatch(expression.right->span, left, right,
                          "equality operand");
        }
        return syntax::TypeKind::Bool;
    }
    if (!compatible(syntax::TypeKind::U32, left)) {
        type_mismatch(expression.left->span, syntax::TypeKind::U32, left,
                      "left arithmetic operand");
    }
    if (!compatible(syntax::TypeKind::U32, right)) {
        type_mismatch(expression.right->span, syntax::TypeKind::U32, right,
                      "right arithmetic operand");
    }
    if (operation == Less || operation == LessEqual || operation == Greater ||
        operation == GreaterEqual) {
        return syntax::TypeKind::Bool;
    }
    return syntax::TypeKind::U32;
}

syntax::TypeKind TypeChecker::check_call(syntax::CallExpr& expression) {
    if (expression.callee == "external_input") {
        if (!expression.arguments.empty()) {
            diagnostics_.error("E1006", "`external_input` takes no arguments",
                               expression.span);
        }
        return syntax::TypeKind::U32;
    }

    const auto iterator = functions_.find(expression.callee);
    if (iterator == functions_.end()) {
        diagnostics_.error("E1003",
                           "unknown function `" + expression.callee + "`",
                           expression.callee_span);
        for (auto& argument : expression.arguments) {
            (void)check_expression(*argument);
        }
        return syntax::TypeKind::Error;
    }
    const auto& function = *iterator->second;
    if (expression.arguments.size() != function.parameters.size()) {
        diagnostics_.error(
            "E1006",
            "wrong number of arguments for `" + expression.callee + "`",
            expression.span,
            "expected " + std::to_string(function.parameters.size()) +
                ", found " + std::to_string(expression.arguments.size()));
    }
    const auto count =
        std::min(expression.arguments.size(), function.parameters.size());
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
        const auto actual = check_expression(*expression.arguments[index]);
        if (index < count &&
            !compatible(function.parameters[index].type, actual)) {
            type_mismatch(expression.arguments[index]->span,
                          function.parameters[index].type, actual,
                          "argument for `" + function.parameters[index].name +
                              "`");
        }
    }
    return function.result;
}

syntax::TypeKind TypeChecker::check_if(syntax::IfExpr& expression) {
    const auto condition = check_expression(*expression.condition);
    if (!compatible(syntax::TypeKind::Bool, condition)) {
        type_mismatch(expression.condition->span, syntax::TypeKind::Bool,
                      condition, "`if` condition");
    }
    bool then_return = false;
    bool else_return = false;
    const auto then_type =
        check_block(*expression.then_block, current_return_type_, then_return);
    const auto else_type =
        check_block(*expression.else_block, current_return_type_, else_return);
    if (!compatible(then_type, else_type)) {
        type_mismatch(expression.else_block->span, then_type, else_type,
                      "`if` arm");
        return syntax::TypeKind::Error;
    }
    if (then_type == syntax::TypeKind::Void) {
        diagnostics_.error("E1007", "`if` expression arms require values",
                           expression.span,
                           "add a tail expression to both blocks");
        return syntax::TypeKind::Error;
    }
    return then_type;
}

bool TypeChecker::define(std::string name, syntax::TypeKind type,
                         source::Span name_span) {
    auto& scope = scopes_.back();
    const auto [iterator, inserted] = scope.emplace(std::move(name), type);
    if (!inserted) {
        diagnostics_.error("E1004",
                           "duplicate binding `" + iterator->first + "`",
                           name_span, "names are unique within a scope");
    }
    return inserted;
}

syntax::TypeKind TypeChecker::lookup(const std::string& name,
                                     source::Span span) {
    for (auto iterator = scopes_.rbegin(); iterator != scopes_.rend();
         ++iterator) {
        if (const auto found = iterator->find(name); found != iterator->end()) {
            return found->second;
        }
    }
    diagnostics_.error("E1003", "unknown name `" + name + "`", span);
    return syntax::TypeKind::Error;
}

void TypeChecker::type_mismatch(source::Span span, syntax::TypeKind expected,
                                syntax::TypeKind actual, std::string context) {
    diagnostics_.error("E1008", "type mismatch in " + std::move(context), span,
                       "expected `" + std::string(syntax::type_name(expected)) +
                           "`, found `" +
                           std::string(syntax::type_name(actual)) + "`");
}

} // namespace pagos::sema
