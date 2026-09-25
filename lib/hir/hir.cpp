#include "pagos/hir/hir.h"

#include <ostream>
#include <string_view>
#include <type_traits>

namespace pagos::hir {
namespace {

std::string_view binary_name(syntax::BinaryOperator operation) {
    using enum syntax::BinaryOperator;
    switch (operation) {
    case Add:
        return "add";
    case Subtract:
        return "sub";
    case Multiply:
        return "mul";
    case Divide:
        return "div";
    case Remainder:
        return "rem";
    case BitAnd:
        return "bit.and";
    case BitOr:
        return "bit.or";
    case BitXor:
        return "bit.xor";
    case ShiftLeft:
        return "shl";
    case ShiftRight:
        return "lshr";
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
    case LogicalAnd:
        return "and";
    case LogicalOr:
        return "or";
    }
    return "unknown";
}

void print_expression(const ExprPtr& expression, std::ostream& output) {
    if (!expression) {
        output << "<none>";
        return;
    }
    switch (expression->kind) {
    case Expr::Kind::Constant: {
        const auto& constant = expression->constant.value();
        if (std::holds_alternative<std::uint32_t>(constant)) {
            output << std::get<std::uint32_t>(constant);
        } else if (std::holds_alternative<bool>(constant)) {
            output << (std::get<bool>(constant) ? "true" : "false");
        } else {
            std::visit(
                [&](const auto& values) {
                    if constexpr (requires { values.size(); }) {
                        output << '[';
                        for (std::size_t index = 0; index < values.size();
                             ++index) {
                            if (index != 0) {
                                output << ", ";
                            }
                            if constexpr (std::is_same_v<
                                              typename std::decay_t<
                                                  decltype(values)>::value_type,
                                              bool>) {
                                output << (values[index] ? "true" : "false");
                            } else {
                                output << values[index];
                            }
                        }
                        output << ']';
                    }
                },
                constant);
        }
        break;
    }
    case Expr::Kind::Reference:
        print_expression(expression->operands.at(0), output);
        break;
    case Expr::Kind::Return:
    case Expr::Kind::ReturnScope:
        output << (expression->kind == Expr::Kind::Return ? "return("
                                                          : "call_scope(");
        print_expression(expression->operands.at(0), output);
        output << ')';
        break;
    case Expr::Kind::RuntimeBoundary:
        output << "runtime(";
        print_expression(expression->operands.at(0), output);
        output << ')';
        break;
    case Expr::Kind::ExternalInput:
        output << "external_input()";
        break;
    case Expr::Kind::Unary:
        output << (expression->unary_operation == syntax::UnaryOperator::Not
                       ? "not("
                       : "bit.not(");
        print_expression(expression->operands.at(0), output);
        output << ')';
        break;
    case Expr::Kind::Binary:
        output << binary_name(expression->binary_operation.value()) << '(';
        print_expression(expression->operands.at(0), output);
        output << ", ";
        print_expression(expression->operands.at(1), output);
        output << ')';
        break;
    case Expr::Kind::If:
        output << "if(";
        print_expression(expression->operands.at(0), output);
        output << ", ";
        print_expression(expression->operands.at(1), output);
        output << ", ";
        print_expression(expression->operands.at(2), output);
        output << ')';
        break;
    case Expr::Kind::Sequence:
    case Expr::Kind::Array:
    case Expr::Kind::Index:
        output << (expression->kind == Expr::Kind::Sequence ? "sequence("
                   : expression->kind == Expr::Kind::Array  ? "array("
                                                            : "index(");
        for (std::size_t index = 0; index < expression->operands.size();
             ++index) {
            if (index != 0) {
                output << ", ";
            }
            print_expression(expression->operands[index], output);
        }
        output << ')';
        break;
    case Expr::Kind::LoopIndex:
        output << expression->variable_name.value_or("index");
        break;
    case Expr::Kind::RangeLoop:
        output << "for(" << expression->variable_name.value_or("index")
               << " in ";
        print_expression(expression->operands.at(0), output);
        output << "..";
        print_expression(expression->operands.at(1), output);
        output << ") {";
        for (std::size_t index = 3; index < expression->operands.size();
             ++index) {
            if (index != 3) {
                output << ", ";
            }
            print_expression(expression->operands[index], output);
        }
        output << '}';
        break;
    }
}

} // namespace

std::string_view stage_name(Stage stage) noexcept {
    return stage == Stage::Static ? "Static" : "Runtime";
}

ExprPtr make_constant(Constant value, syntax::Type type, source::Span span) {
    auto expression = std::make_shared<Expr>();
    expression->kind = Expr::Kind::Constant;
    expression->type = type;
    expression->stage = Stage::Static;
    expression->span = span;
    expression->constant = std::move(value);
    return expression;
}

ExprPtr with_trace_step(const ExprPtr& expression, std::string step) {
    if (!expression || expression->stage == Stage::Static ||
        !expression->trace) {
        return expression;
    }
    auto copy = std::make_shared<Expr>(*expression);
    // A trace describes a use, not a new evaluation of the computation.
    copy->kind = Expr::Kind::Reference;
    copy->operands = {expression};
    copy->trace.value().path.push_back(std::move(step));
    return copy;
}

void print(const Module& module, std::ostream& output) {
    for (const auto& function : module.functions) {
        output << "fn " << function.name << '(';
        for (std::size_t index = 0; index < function.parameters.size();
             ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << syntax::type_name(function.parameters[index]);
        }
        output << ") -> " << syntax::type_name(function.result) << '\n';
    }
    for (const auto& binding : module.bindings) {
        output << "binding " << binding.name << ": "
               << syntax::type_name(binding.type) << " ["
               << stage_name(binding.stage) << "] = ";
        print_expression(binding.value, output);
        output << '\n';
    }
    if (module.result && module.result->kind == Expr::Kind::Sequence) {
        output << "residual = ";
        print_expression(module.result, output);
        output << '\n';
    }
}

void explain_stages(const Module& module, std::ostream& output) {
    for (const auto& binding : module.bindings) {
        output << binding.name << ": " << stage_name(binding.stage);
        if (binding.trace) {
            const auto& trace = binding.trace.value();
            output << " (";
            for (std::size_t index = 0; index < trace.path.size(); ++index) {
                if (index != 0) {
                    output << " -> ";
                }
                output << trace.path[index];
            }
            output << ')';
        }
        output << '\n';
    }
}

} // namespace pagos::hir
