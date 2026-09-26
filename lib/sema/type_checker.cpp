#include "pagos/sema/type_checker.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_set>

namespace pagos::sema {
namespace {

bool compatible(const sema::Type& expected, const sema::Type& actual) {
    return expected == sema::TypeKind::Error ||
           actual == sema::TypeKind::Never || actual == sema::TypeKind::Error ||
           expected == actual ||
           (expected.is_array() && actual.is_array() &&
            expected.element_type() == actual.element_type() &&
            (expected.length == 0 || actual.length == 0));
}

} // namespace

bool TypeChecker::check(syntax::Module& module) {
    types_.expressions.clear();
    functions_.clear();
    records_.clear();
    collect_records(module);
    collect_functions(module);
    for (auto& function : module.functions) {
        check_function(*function);
    }

    scopes_.emplace_back();
    bool saw_return = false;
    for (auto& statement : module.statements) {
        check_statement(*statement, sema::TypeKind::Void, saw_return);
    }
    scopes_.pop_back();
    return !diagnostics_.has_error();
}

void TypeChecker::collect_records(const syntax::Module& module) {
    for (const auto& record : module.records) {
        if (!records_.emplace(record.name, &record).second ||
            record.name == "external_input") {
            diagnostics_.error("E1004",
                               "duplicate or reserved record name `" +
                                   record.name + "`",
                               record.name_span);
        }
        if (record.fields.empty()) {
            diagnostics_.error("E1008", "empty records are not supported",
                               record.name_span);
        }
        std::unordered_set<std::string> names;
        for (const auto& field : record.fields) {
            if (!names.insert(field.name).second) {
                diagnostics_.error("E1004",
                                   "duplicate field `" + field.name + "`",
                                   field.name_span);
            }
            if (types_.resolve(field.type).kind != sema::TypeKind::Integer &&
                types_.resolve(field.type) != sema::TypeKind::Bool) {
                diagnostics_.error("E1008",
                                   "record fields must be `bool` or an integer",
                                   field.name_span);
            }
        }
    }
}

void TypeChecker::validate_type(const sema::Type& type, source::Span span) {
    if (type.kind == TypeKind::Error) {
        diagnostics_.error("E1005", "`usize` requires a target pointer width",
                           span);
    }
    if (type.is_record() && !records_.contains(type.record_name)) {
        diagnostics_.error(
            "E1003", "unknown record type `" + type.record_name + "`", span);
    }
}

void TypeChecker::collect_functions(syntax::Module& module) {
    for (auto& function : module.functions) {
        if (records_.contains(function->name)) {
            diagnostics_.error("E1004",
                               "function and record share name `" +
                                   function->name + "`",
                               function->name_span);
        }
        validate_type(types_.resolve(function->result), function->name_span);
        for (const auto& parameter : function->parameters) {
            validate_type(types_.resolve(parameter.type), parameter.name_span);
        }
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
        define(parameter.name, types_.resolve(parameter.type),
               parameter.name_span);
    }
    const auto previous_return_type = current_return_type_;
    const auto result_type = types_.resolve(function.result);
    current_return_type_ = result_type;
    bool saw_return = false;
    const auto tail_type = check_block(*function.body, result_type, saw_return);
    if (tail_type != sema::TypeKind::Void &&
        !compatible(result_type, tail_type)) {
        type_mismatch(function.body->tail->span, result_type, tail_type,
                      "function tail expression");
    }
    if (!saw_return && tail_type == sema::TypeKind::Void) {
        diagnostics_.error(
            "E1007", "function `" + function.name + "` has no result",
            function.name_span, "add `return` or a block tail expression");
    }
    current_return_type_ = previous_return_type;
    scopes_.pop_back();
}

sema::Type TypeChecker::check_block(syntax::Block& block,
                                    const sema::Type& expected_return,
                                    bool& saw_return) {
    scopes_.emplace_back();
    for (auto& statement : block.statements) {
        check_statement(*statement, expected_return, saw_return);
    }
    sema::Type type = sema::TypeKind::Void;
    if (block.tail) {
        type = check_expression(*block.tail);
    }
    saw_return = saw_return || type == sema::TypeKind::Never;
    if (saw_return) {
        type = sema::TypeKind::Never;
    }
    scopes_.pop_back();
    return type;
}

void TypeChecker::check_statement(syntax::Stmt& statement,
                                  const sema::Type& expected_return,
                                  bool& saw_return) {
    switch (statement.kind) {
    case syntax::Stmt::Kind::Binding: {
        auto& binding = static_cast<syntax::BindingStmt&>(statement);
        if (binding.annotation) {
            validate_type(types_.resolve(*binding.annotation),
                          binding.name_span);
        }
        const auto initializer_type = check_expression(*binding.initializer);
        saw_return = saw_return || initializer_type == sema::TypeKind::Never;
        const auto binding_type = binding.annotation
                                      ? types_.resolve(*binding.annotation)
                                      : initializer_type;
        if (binding.annotation && !compatible(binding_type, initializer_type)) {
            type_mismatch(binding.initializer->span, binding_type,
                          initializer_type,
                          "initializer for `" + binding.name + "`");
        }
        define(binding.name, binding_type, binding.name_span);
        break;
    }
    case syntax::Stmt::Kind::Return: {
        auto& return_statement = static_cast<syntax::ReturnStmt&>(statement);
        const auto actual = check_expression(*return_statement.value);
        if (expected_return == sema::TypeKind::Void) {
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
        const auto type = check_expression(*expression_statement.expression);
        saw_return = saw_return || type == sema::TypeKind::Never;
        break;
    }
    case syntax::Stmt::Kind::For: {
        auto& for_statement = static_cast<syntax::ForStmt&>(statement);
        const auto begin_type = check_expression(*for_statement.begin);
        const auto end_type = check_expression(*for_statement.end);
        if (!compatible(sema::TypeKind::Integer, begin_type)) {
            type_mismatch(for_statement.begin->span, sema::TypeKind::Integer,
                          begin_type, "range start");
        }
        if (!compatible(sema::TypeKind::Integer, end_type)) {
            type_mismatch(for_statement.end->span, sema::TypeKind::Integer,
                          end_type, "range end");
        }
        scopes_.emplace_back();
        define(for_statement.variable, sema::TypeKind::Integer,
               for_statement.variable_span);
        bool body_returns = false;
        (void)check_block(*for_statement.body, expected_return, body_returns);
        // The body may never execute. Only evaluating a bound can guarantee
        // a return independently of the trip count.
        saw_return = saw_return || begin_type == sema::TypeKind::Never ||
                     end_type == sema::TypeKind::Never;
        scopes_.pop_back();
        break;
    }
    }
}

sema::Type TypeChecker::check_expression(syntax::Expr& expression) {
    sema::Type type = sema::TypeKind::Error;
    switch (expression.kind) {
    case syntax::Expr::Kind::Record:
        type = check_record(static_cast<syntax::RecordExpr&>(expression));
        break;
    case syntax::Expr::Kind::Field:
        type = check_field(static_cast<syntax::FieldExpr&>(expression));
        break;
    case syntax::Expr::Kind::Integer: {
        const auto& integer = static_cast<syntax::IntegerExpr&>(expression);
        const auto literal_type =
            integer.is_usize ? Type::usize(types_.pointer_bits)
                             : Type::integer(integer.width, integer.is_signed);
        if (literal_type.kind == TypeKind::Error) {
            diagnostics_.error("E1005",
                               "`usize` requires a target pointer width",
                               integer.span);
            break;
        }
        if (!IntegerValue::parse_decimal(literal_type.integer_type,
                                         integer.spelling)) {
            diagnostics_.error("E1005",
                               "integer literal does not fit `" +
                                   type_name(literal_type) + "`",
                               integer.span, "no truncation is performed");
        } else {
            type = literal_type;
        }
        break;
    }
    case syntax::Expr::Kind::Boolean:
        type = sema::TypeKind::Bool;
        break;
    case syntax::Expr::Kind::Name: {
        const auto& name = static_cast<syntax::NameExpr&>(expression);
        type = lookup(name.name, name.span);
        break;
    }
    case syntax::Expr::Kind::Unary: {
        auto& unary = static_cast<syntax::UnaryExpr&>(expression);
        const auto operand_type = check_expression(*unary.operand);
        if (operand_type == sema::TypeKind::Never) {
            type = sema::TypeKind::Never;
            break;
        }
        if (unary.operation == syntax::UnaryOperator::Negate) {
            if (operand_type.kind == TypeKind::Integer &&
                operand_type.integer_type.is_signed) {
                type = operand_type;
            } else if (operand_type.kind != TypeKind::Error) {
                diagnostics_.error(
                    "E1008", "unary `-` requires a signed integer", unary.span,
                    "use a signed literal suffix or an explicit integer cast");
            }
            break;
        }
        const auto expected = unary.operation == syntax::UnaryOperator::Not
                                  ? Type(sema::TypeKind::Bool)
                              : operand_type.kind == TypeKind::Integer
                                  ? operand_type
                                  : Type(TypeKind::Integer);
        if (!compatible(expected, operand_type)) {
            type_mismatch(unary.operand->span, expected, operand_type,
                          unary.operation == syntax::UnaryOperator::Not
                              ? "operand of `!`"
                              : "operand of `~`");
        } else {
            type = expected;
        }
        break;
    }
    case syntax::Expr::Kind::Cast: {
        auto& cast = static_cast<syntax::CastExpr&>(expression);
        const auto operand = check_expression(*cast.operand);
        const auto destination = types_.resolve(cast.destination);
        if (destination.kind != TypeKind::Integer ||
            (operand.kind != TypeKind::Integer &&
             operand.kind != TypeKind::Never &&
             operand.kind != TypeKind::Error)) {
            diagnostics_.error(
                "E1007", "`as` requires integer source and destination types",
                expression.span);
        } else {
            type = operand.kind == TypeKind::Never ? operand : destination;
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
    case syntax::Expr::Kind::Array: {
        auto& array = static_cast<syntax::ArrayExpr&>(expression);
        sema::Type element_type = sema::TypeKind::Unknown;
        bool returns = false;
        if (array.elements.empty()) {
            diagnostics_.error("E1008", "empty arrays are not supported",
                               expression.span);
        }
        if (array.elements.size() > std::numeric_limits<std::uint32_t>::max()) {
            diagnostics_.error("E1008", "array length does not fit `u32`",
                               expression.span);
        }
        for (auto& element : array.elements) {
            const auto actual = check_expression(*element);
            returns = returns || actual == sema::TypeKind::Never;
            if (actual == sema::TypeKind::Never ||
                actual == sema::TypeKind::Error) {
                continue;
            }
            if (actual != sema::TypeKind::Bool &&
                actual.kind != sema::TypeKind::Integer) {
                diagnostics_.error(
                    "E1008", "array elements must be `bool` or an integer",
                    element->span);
                continue;
            }
            if (element_type == sema::TypeKind::Unknown) {
                element_type = actual;
            } else if (element_type != actual) {
                type_mismatch(element->span, element_type, actual,
                              "array element");
            }
        }
        if (returns) {
            type = sema::TypeKind::Never;
        } else if (element_type != sema::TypeKind::Unknown) {
            type = sema::Type::array(element_type, static_cast<std::uint32_t>(
                                                       array.elements.size()));
        }
        break;
    }
    case syntax::Expr::Kind::ArrayGenerator: {
        auto& generator = static_cast<syntax::ArrayGeneratorExpr&>(expression);
        const auto begin = check_expression(*generator.begin);
        const auto end = check_expression(*generator.end);
        if (!compatible(sema::TypeKind::Integer, begin)) {
            type_mismatch(generator.begin->span, sema::TypeKind::Integer, begin,
                          "array generator start");
        }
        if (!compatible(sema::TypeKind::Integer, end)) {
            type_mismatch(generator.end->span, sema::TypeKind::Integer, end,
                          "array generator end");
        }
        scopes_.emplace_back();
        define(generator.variable, sema::TypeKind::Integer,
               generator.variable_span);
        bool returns = false;
        const auto element =
            check_block(*generator.body, current_return_type_, returns);
        scopes_.pop_back();
        if (element == sema::TypeKind::Never) {
            type = sema::TypeKind::Never;
        } else if (element.kind == sema::TypeKind::Integer ||
                   element == sema::TypeKind::Bool) {
            if (begin == sema::TypeKind::Never ||
                end == sema::TypeKind::Never) {
                type = sema::TypeKind::Never;
                break;
            }
            // Expressions are evaluated by staging, not by a second evaluator
            // in the type checker. Preserve the known literal shape where
            // possible; all other lengths are checked at specialization time.
            std::uint32_t length = 0;
            if (generator.begin->kind == syntax::Expr::Kind::Integer &&
                generator.end->kind == syntax::Expr::Kind::Integer &&
                begin == sema::TypeKind::Integer &&
                end == sema::TypeKind::Integer) {
                const auto literal = [](const syntax::Expr& bound) {
                    auto digits =
                        static_cast<const syntax::IntegerExpr&>(bound).spelling;
                    std::erase(digits, '_');
                    std::uint32_t value{};
                    std::from_chars(digits.data(),
                                    digits.data() + digits.size(), value);
                    return value;
                };
                const auto first = literal(*generator.begin);
                const auto last = literal(*generator.end);
                if (last > first) {
                    length = last - first;
                }
            }
            type = sema::Type::array(element, length);
        } else if (element != sema::TypeKind::Error) {
            diagnostics_.error("E1008",
                               "array generator body must produce `bool` or an "
                               "integer",
                               generator.body->span);
        }
        break;
    }
    case syntax::Expr::Kind::Index: {
        auto& index = static_cast<syntax::IndexExpr&>(expression);
        const auto array_type = check_expression(*index.array);
        const auto index_type = check_expression(*index.index);
        if (!compatible(sema::TypeKind::Integer, index_type)) {
            type_mismatch(index.index->span, sema::TypeKind::Integer,
                          index_type, "array index");
        }
        if (array_type == sema::TypeKind::Never ||
            index_type == sema::TypeKind::Never) {
            type = sema::TypeKind::Never;
        } else if (array_type.is_array()) {
            type = array_type.element_type();
        } else if (array_type != sema::TypeKind::Error) {
            diagnostics_.error("E1008", "indexing requires an array",
                               index.array->span);
        }
        break;
    }
    }
    types_.expressions.insert_or_assign(&expression, type);
    return type;
}

sema::Type TypeChecker::check_record(syntax::RecordExpr& expression) {
    const auto found = records_.find(expression.name);
    if (found == records_.end()) {
        diagnostics_.error("E1003",
                           "unknown record type `" + expression.name + "`",
                           expression.name_span);
        for (auto& field : expression.fields) {
            (void)check_expression(*field.value);
        }
        return sema::TypeKind::Error;
    }
    const auto& fields = found->second->fields;
    std::unordered_set<std::string> initialized;
    bool returns = false;
    for (auto& field : expression.fields) {
        const auto actual = check_expression(*field.value);
        returns = returns || actual == sema::TypeKind::Never;
        if (!initialized.insert(field.name).second) {
            diagnostics_.error(
                "E1004", "duplicate initializer for field `" + field.name + "`",
                field.name_span);
        }
        const auto declared =
            std::ranges::find(fields, field.name, &syntax::Parameter::name);
        if (declared == fields.end()) {
            diagnostics_.error("E1008",
                               "unknown field `" + field.name + "` in `" +
                                   expression.name + "`",
                               field.name_span);
        } else if (!compatible(types_.resolve(declared->type), actual)) {
            type_mismatch(field.value->span, types_.resolve(declared->type),
                          actual, "field `" + field.name + "`");
        }
    }
    for (const auto& field : fields) {
        if (!initialized.contains(field.name)) {
            diagnostics_.error("E1008",
                               "missing field `" + field.name + "` in `" +
                                   expression.name + "`",
                               expression.span);
        }
    }
    return returns ? sema::Type{sema::TypeKind::Never}
                   : sema::Type::record(expression.name);
}

sema::Type TypeChecker::check_field(syntax::FieldExpr& expression) {
    auto type = check_expression(*expression.record);
    if (type == sema::TypeKind::Never || type == sema::TypeKind::Error) {
        return type;
    }
    if (!type.is_record()) {
        diagnostics_.error("E1008", "field access requires a record",
                           expression.record->span);
        return sema::TypeKind::Error;
    }
    const auto record = records_.find(type.record_name);
    if (record == records_.end()) {
        return sema::TypeKind::Error;
    }
    const auto& fields = record->second->fields;
    const auto field =
        std::ranges::find(fields, expression.name, &syntax::Parameter::name);
    if (field == fields.end()) {
        diagnostics_.error("E1008",
                           "unknown field `" + expression.name + "` in `" +
                               type.record_name + "`",
                           expression.name_span);
        return sema::TypeKind::Error;
    }
    return types_.resolve(field->type);
}

sema::Type TypeChecker::check_binary(syntax::BinaryExpr& expression) {
    auto left = check_expression(*expression.left);
    const auto right = check_expression(*expression.right);
    using enum syntax::BinaryOperator;
    const auto operation = expression.operation;
    if (left == sema::TypeKind::Never ||
        (right == sema::TypeKind::Never && operation != LogicalAnd &&
         operation != LogicalOr)) {
        return sema::TypeKind::Never;
    }
    if (operation == LogicalAnd || operation == LogicalOr) {
        if (!compatible(sema::TypeKind::Bool, left)) {
            type_mismatch(expression.left->span, sema::TypeKind::Bool, left,
                          "left logical operand");
        }
        if (!compatible(sema::TypeKind::Bool, right)) {
            type_mismatch(expression.right->span, sema::TypeKind::Bool, right,
                          "right logical operand");
        }
        return sema::TypeKind::Bool;
    }
    if (operation == Equal || operation == NotEqual) {
        if (left.is_record() || right.is_record()) {
            diagnostics_.error("E1008", "record equality is not supported",
                               expression.span);
            return sema::TypeKind::Error;
        }
        if (left.is_array() || right.is_array()) {
            diagnostics_.error("E1008", "array equality is not supported",
                               expression.span);
            return sema::TypeKind::Error;
        }
        if (!compatible(left, right)) {
            type_mismatch(expression.right->span, left, right,
                          "equality operand");
        }
        return sema::TypeKind::Bool;
    }
    if (left.kind != TypeKind::Integer &&
        !compatible(sema::TypeKind::Integer, left)) {
        type_mismatch(expression.left->span, sema::TypeKind::Integer, left,
                      "left arithmetic operand");
    }
    if (!compatible(left, right)) {
        type_mismatch(expression.right->span, left, right,
                      "right arithmetic operand");
    }
    if (operation == Less || operation == LessEqual || operation == Greater ||
        operation == GreaterEqual) {
        return sema::TypeKind::Bool;
    }
    return left;
}

sema::Type TypeChecker::check_call(syntax::CallExpr& expression) {
    if (records_.contains(expression.callee)) {
        diagnostics_.error("E1008", "record construction requires named fields",
                           expression.span);
        for (auto& argument : expression.arguments) {
            (void)check_expression(*argument);
        }
        return sema::TypeKind::Error;
    }
    if (expression.callee == "external_input") {
        if (!expression.arguments.empty()) {
            diagnostics_.error("E1006", "`external_input` takes no arguments",
                               expression.span);
        }
        return sema::TypeKind::Integer;
    }

    const auto iterator = functions_.find(expression.callee);
    if (iterator == functions_.end()) {
        diagnostics_.error("E1003",
                           "unknown function `" + expression.callee + "`",
                           expression.callee_span);
        for (auto& argument : expression.arguments) {
            (void)check_expression(*argument);
        }
        return sema::TypeKind::Error;
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
    bool argument_returns = false;
    for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
        const auto actual = check_expression(*expression.arguments[index]);
        argument_returns = argument_returns || actual == sema::TypeKind::Never;
        if (index < count &&
            !compatible(types_.resolve(function.parameters[index].type),
                        actual)) {
            type_mismatch(
                expression.arguments[index]->span,
                types_.resolve(function.parameters[index].type), actual,
                "argument for `" + function.parameters[index].name + "`");
        }
    }
    return argument_returns ? sema::Type(sema::TypeKind::Never)
                            : types_.resolve(function.result);
}

sema::Type TypeChecker::check_if(syntax::IfExpr& expression) {
    const auto condition = check_expression(*expression.condition);
    if (!compatible(sema::TypeKind::Bool, condition)) {
        type_mismatch(expression.condition->span, sema::TypeKind::Bool,
                      condition, "`if` condition");
    }
    bool then_return = false;
    bool else_return = false;
    auto then_type =
        check_block(*expression.then_block, current_return_type_, then_return);
    auto else_type =
        check_block(*expression.else_block, current_return_type_, else_return);
    if (condition == sema::TypeKind::Never) {
        return sema::TypeKind::Never;
    }
    if (then_type == sema::TypeKind::Never &&
        else_type != sema::TypeKind::Void) {
        return else_type;
    }
    if (else_type == sema::TypeKind::Never &&
        then_type != sema::TypeKind::Void) {
        return then_type;
    }
    if (!compatible(then_type, else_type)) {
        type_mismatch(expression.else_block->span, then_type, else_type,
                      "`if` arm");
        return sema::TypeKind::Error;
    }
    if (then_type == sema::TypeKind::Void) {
        diagnostics_.error("E1007", "`if` expression arms require values",
                           expression.span,
                           "add a tail expression to both blocks");
        return sema::TypeKind::Error;
    }
    if (then_type.is_array() && else_type.is_array() &&
        (then_type.length == 0 || else_type.length == 0)) {
        return sema::Type::array(then_type.element_type(), 0);
    }
    return then_type;
}

bool TypeChecker::define(std::string name, const sema::Type& type,
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

sema::Type TypeChecker::lookup(const std::string& name, source::Span span) {
    for (auto iterator = scopes_.rbegin(); iterator != scopes_.rend();
         ++iterator) {
        if (const auto found = iterator->find(name); found != iterator->end()) {
            return found->second;
        }
    }
    diagnostics_.error("E1003", "unknown name `" + name + "`", span);
    return sema::TypeKind::Error;
}

void TypeChecker::type_mismatch(source::Span span, const sema::Type& expected,
                                const sema::Type& actual, std::string context) {
    diagnostics_.error("E1008", "type mismatch in " + std::move(context), span,
                       "expected `" + std::string(sema::type_name(expected)) +
                           "`, found `" + std::string(sema::type_name(actual)) +
                           "`");
}

} // namespace pagos::sema
