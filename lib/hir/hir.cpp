#include "pagos/hir/hir.h"

#include <algorithm>
#include <ostream>
#include <string_view>

namespace pagos::hir {
bool has_residual_work(const ExprPtr& expression) {
    if (!expression)
        return false;
    if (expression->stage == Stage::Runtime || expression->may_return)
        return true;
    return std::ranges::any_of(expression->operands, has_residual_work);
}

bool is_cacheable_result(const ExprPtr& expression) {
    return expression && expression->stage == Stage::Comptime &&
           expression->constant.has_value() && expression->falls_through &&
           !has_residual_work(expression);
}

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
    case Expr::Kind::GlobalAddress:
        output << "global_address(" << *expression->variable_name << ')';
        break;
    case Expr::Kind::FieldAddress:
        output << "field_address(";
        print_expression(expression->operands.at(0), output);
        output << ", " << expression->field_index << ')';
        break;
    case Expr::Kind::GlobalElementAddress:
        output << "global_element_address(" << *expression->variable_name
               << ", ";
        print_expression(expression->operands.at(0), output);
        output << ')';
        break;
    case Expr::Kind::Constant: {
        print_constant(expression->constant.value(), output);
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
    case Expr::Kind::Parameter:
        output << "parameter(" << expression->variable_name.value() << ')';
        break;
    case Expr::Kind::Call:
        output << "call " << expression->variable_name.value() << '(';
        for (std::size_t index = 0; index < expression->operands.size();
             ++index) {
            if (index != 0)
                output << ", ";
            print_expression(expression->operands[index], output);
        }
        output << ") {";
        if (const auto callee = expression->callee.lock()) {
            // Recursive definitions are printed separately below, never
            // recursively expanded through call edges.
            if (callee->c_abi)
                output << (callee->declaration ? "extern C" : "export C");
            else if (callee->recursive)
                output << "recursive";
            else
                print_expression(callee->body, output);
        } else {
            output << "<missing definition>";
        }
        output << '}';
        break;
    case Expr::Kind::Cast:
        output << "cast<" << sema::type_name(expression->type) << ">(";
        print_expression(expression->operands.front(), output);
        output << ')';
        break;
    case Expr::Kind::Load:
    case Expr::Kind::Store:
    case Expr::Kind::VolatileLoad:
    case Expr::Kind::VolatileStore:
        if (expression->kind == Expr::Kind::VolatileLoad ||
            expression->kind == Expr::Kind::VolatileStore)
            output << "volatile_";
        output << (expression->kind == Expr::Kind::Load ||
                           expression->kind == Expr::Kind::VolatileLoad
                       ? "load("
                       : "store(");
        print_expression(expression->operands.at(0), output);
        if (expression->kind == Expr::Kind::Store ||
            expression->kind == Expr::Kind::VolatileStore) {
            output << ", ";
            print_expression(expression->operands.at(1), output);
        }
        output << ')';
        break;
    case Expr::Kind::Unary:
        output << (expression->unary_operation == syntax::UnaryOperator::Not
                       ? "not("
                   : expression->unary_operation ==
                           syntax::UnaryOperator::BitNot
                       ? "bit.not("
                       : "negate(");
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
    case Expr::Kind::Field:
        output << "field(";
        print_expression(expression->operands.at(0), output);
        output << ", " << expression->variable_name.value() << ')';
        break;
    case Expr::Kind::Record:
        output << sema::type_name(expression->type);
        [[fallthrough]];
    case Expr::Kind::Sequence:
    case Expr::Kind::Array:
    case Expr::Kind::Index:
        output << (expression->kind == Expr::Kind::Sequence ? "sequence("
                   : expression->kind == Expr::Kind::Array  ? "array("
                   : expression->kind == Expr::Kind::Record ? "("
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
    return stage == Stage::Comptime ? "Comptime" : "Runtime";
}

ExprPtr make_constant(Constant value, sema::Type type, source::Span span) {
    auto expression = std::make_shared<Expr>();
    expression->kind = Expr::Kind::Constant;
    expression->type = std::move(type);
    expression->stage = Stage::Comptime;
    expression->span = span;
    expression->constant = std::move(value);
    return expression;
}

ExprPtr with_trace_step(const ExprPtr& expression, std::string step) {
    if (!expression || expression->stage == Stage::Comptime ||
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
    for (const auto& global : module.globals) {
        output << "static " << (global.writable ? "mut " : "const ")
               << global.name << ": " << sema::type_name(global.type);
        if (!global.storage.section.empty())
            output << " section(\"" << global.storage.section << "\")";
        if (global.storage.alignment != 0)
            output << " align(" << global.storage.alignment << ')';
        output << " = ";
        print_constant(global.initializer, output);
        output << '\n';
    }
    for (const auto& function : module.residual_functions) {
        if (function->c_abi) {
            output << (function->declaration ? "extern C fn " : "export C fn ")
                   << function->name << " -> "
                   << sema::type_name(function->result_type);
            if (!function->declaration) {
                output << " = ";
                print_expression(function->body, output);
            }
            output << '\n';
            continue;
        }
        if (!function->recursive)
            continue;
        output << "recursive fn " << function->name << " -> "
               << sema::type_name(function->result_type) << " = ";
        print_expression(function->body, output);
        output << '\n';
    }
    for (const auto& record : module.records) {
        output << "record " << record.name << " {";
        for (std::size_t index = 0; index < record.fields.size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << record.names[index] << ": "
                   << sema::type_name(record.fields[index]);
        }
        output << "}\n";
    }
    for (const auto& function : module.functions) {
        output << "fn " << function.name << '(';
        for (std::size_t index = 0; index < function.parameters.size();
             ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << sema::type_name(function.parameters[index]);
        }
        output << ") -> " << sema::type_name(function.result) << '\n';
    }
    for (const auto& binding : module.bindings) {
        output << "binding " << binding.name << ": "
               << sema::type_name(binding.type) << " ["
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
            const auto path = trace.path.to_vector();
            for (std::size_t index = 0; index < path.size(); ++index) {
                if (index != 0) {
                    output << " -> ";
                }
                output << path[index];
            }
            output << ')';
        }
        output << '\n';
    }
}

} // namespace pagos::hir
