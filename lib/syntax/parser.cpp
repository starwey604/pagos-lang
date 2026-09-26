#include "pagos/syntax/parser.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>

namespace pagos::syntax {

const Token& Parser::current() const noexcept { return tokens_[index_]; }

const Token& Parser::previous() const noexcept { return tokens_[index_ - 1]; }

bool Parser::at_end() const noexcept { return check(TokenKind::End); }

bool Parser::check(TokenKind kind) const noexcept {
    return current().kind == kind;
}

bool Parser::match(TokenKind kind) noexcept {
    if (!check(kind)) {
        return false;
    }
    ++index_;
    return true;
}

const Token* Parser::consume(TokenKind kind, std::string message) {
    if (check(kind)) {
        ++index_;
        return &previous();
    }
    diagnostics_.error("E1002", std::move(message), current().span,
                       "found " + std::string(token_kind_name(current().kind)));
    return nullptr;
}

void Parser::synchronize() {
    while (!at_end()) {
        if (index_ > 0 && previous().kind == TokenKind::Semicolon) {
            return;
        }
        switch (current().kind) {
        case TokenKind::KwFn:
        case TokenKind::KwRecord:
        case TokenKind::KwLet:
        case TokenKind::KwStatic:
        case TokenKind::KwRuntime:
        case TokenKind::KwReturn:
        case TokenKind::KwFor:
        case TokenKind::RightBrace:
            return;
        default:
            ++index_;
            break;
        }
    }
}

std::unique_ptr<Module> Parser::parse_module() {
    auto module = std::make_unique<Module>();
    while (!at_end()) {
        const auto before = index_;
        if (check(TokenKind::KwRecord)) {
            if (auto record = parse_record()) {
                module->records.push_back(std::move(*record));
            }
        } else if (check(TokenKind::KwFn)) {
            if (auto function = parse_function()) {
                module->functions.push_back(std::move(function));
            }
        } else if (auto statement = parse_statement()) {
            module->statements.push_back(std::move(statement));
        }

        if (index_ == before) {
            ++index_;
        }
        if (diagnostics_.has_error()) {
            synchronize();
        }
    }
    return module;
}

std::optional<Module::Record> Parser::parse_record() {
    consume(TokenKind::KwRecord, "expected `record`");
    const auto* name = consume(TokenKind::Identifier, "expected record name");
    if (!name ||
        !consume(TokenKind::LeftBrace, "expected `{` after record name")) {
        return std::nullopt;
    }
    Module::Record record{.name = std::string(name->lexeme),
                          .name_span = name->span};
    if (!check(TokenKind::RightBrace)) {
        do {
            const auto* field =
                consume(TokenKind::Identifier, "expected field name");
            if (!field ||
                !consume(TokenKind::Colon, "expected `:` after field name")) {
                return std::nullopt;
            }
            const auto type = parse_type();
            if (!type) {
                return std::nullopt;
            }
            record.fields.push_back({.name = std::string(field->lexeme),
                                     .name_span = field->span,
                                     .type = *type});
        } while (match(TokenKind::Comma) && !check(TokenKind::RightBrace));
    }
    if (!consume(TokenKind::RightBrace, "expected `}` after record fields")) {
        return std::nullopt;
    }
    return record;
}

std::optional<Type> Parser::parse_type() {
    if (match(TokenKind::Identifier)) {
        return Type::record(std::string(previous().lexeme));
    }
    if (match(TokenKind::LeftBracket)) {
        const auto element_span = current().span;
        if (!check(TokenKind::KwBool) && !check(TokenKind::KwU32) &&
            !check(TokenKind::KwU8) && !check(TokenKind::KwU16) &&
            !check(TokenKind::KwU64) && !check(TokenKind::KwI8) &&
            !check(TokenKind::KwI16) && !check(TokenKind::KwI32) &&
            !check(TokenKind::KwI64)) {
            diagnostics_.error("E1002",
                               "array elements must be `bool` or an integer",
                               element_span);
            return std::nullopt;
        }
        const auto element = parse_type();
        if (!consume(TokenKind::Semicolon,
                     "expected `;` before array length")) {
            return std::nullopt;
        }
        const auto* size =
            consume(TokenKind::Integer, "expected literal array length");
        if (!size) {
            return std::nullopt;
        }
        std::string digits(size->lexeme);
        std::erase(digits, '_');
        std::uint32_t length{};
        const auto parsed = std::from_chars(
            digits.data(), digits.data() + digits.size(), length);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != digits.data() + digits.size() || length == 0) {
            diagnostics_.error("E1005",
                               "array length must be a positive `u32` literal",
                               size->span);
            return std::nullopt;
        }
        if (!consume(TokenKind::RightBracket,
                     "expected `]` after array type")) {
            return std::nullopt;
        }
        return Type::array(*element, length);
    }
    if (match(TokenKind::KwBool)) {
        return TypeKind::Bool;
    }
    if (match(TokenKind::KwU32)) {
        return TypeKind::Integer;
    }
    if (match(TokenKind::KwU8))
        return Type::integer(8);
    if (match(TokenKind::KwU16))
        return Type::integer(16);
    if (match(TokenKind::KwU64))
        return Type::integer(64);
    if (match(TokenKind::KwI8))
        return Type::integer(8, true);
    if (match(TokenKind::KwI16))
        return Type::integer(16, true);
    if (match(TokenKind::KwI32))
        return Type::integer(32, true);
    if (match(TokenKind::KwI64))
        return Type::integer(64, true);
    diagnostics_.error("E1002", "expected type", current().span,
                       "expected `bool`, an integer, a record name, or a "
                       "fixed-length scalar array");
    return std::nullopt;
}

std::unique_ptr<Function> Parser::parse_function() {
    const auto* start = consume(TokenKind::KwFn, "expected `fn`");
    const auto* name =
        consume(TokenKind::Identifier, "expected function name after `fn`");
    if (!start || !name ||
        !consume(TokenKind::LeftParen, "expected `(` after function name")) {
        return nullptr;
    }

    std::vector<Parameter> parameters;
    if (!check(TokenKind::RightParen)) {
        do {
            const auto* parameter_name =
                consume(TokenKind::Identifier, "expected parameter name");
            if (!parameter_name ||
                !consume(TokenKind::Colon,
                         "expected `:` after parameter name")) {
                return nullptr;
            }
            const auto type = parse_type();
            if (!type) {
                return nullptr;
            }
            parameters.push_back({.name = std::string(parameter_name->lexeme),
                                  .name_span = parameter_name->span,
                                  .type = *type});
        } while (match(TokenKind::Comma));
    }

    if (!consume(TokenKind::RightParen, "expected `)` after parameters") ||
        !consume(TokenKind::Arrow, "expected `->` before function result")) {
        return nullptr;
    }
    const auto result = parse_type();
    if (!result) {
        return nullptr;
    }
    auto body = parse_block();
    if (!body) {
        return nullptr;
    }
    const auto span =
        source::Span{.begin = start->span.begin, .end = body->span.end};
    return std::make_unique<Function>(
        Function{.name = std::string(name->lexeme),
                 .name_span = name->span,
                 .parameters = std::move(parameters),
                 .result = *result,
                 .body = std::move(body),
                 .span = span});
}

std::unique_ptr<Block> Parser::parse_block() {
    const auto* left_brace =
        consume(TokenKind::LeftBrace, "expected `{` to start block");
    if (!left_brace) {
        return nullptr;
    }

    auto block = std::make_unique<Block>();
    while (!check(TokenKind::RightBrace) && !at_end()) {
        if (check(TokenKind::KwLet) || check(TokenKind::KwStatic) ||
            check(TokenKind::KwRuntime) || check(TokenKind::KwReturn) ||
            check(TokenKind::KwFor)) {
            if (auto statement = parse_statement()) {
                block->statements.push_back(std::move(statement));
            } else {
                synchronize();
            }
            continue;
        }

        auto expression = parse_expression();
        if (!expression) {
            synchronize();
            continue;
        }
        if (match(TokenKind::Semicolon)) {
            const auto span = source::Span{.begin = expression->span.begin,
                                           .end = previous().span.end};
            block->statements.push_back(
                std::make_unique<ExpressionStmt>(std::move(expression), span));
            continue;
        }
        block->tail = std::move(expression);
        break;
    }

    const auto* right_brace =
        consume(TokenKind::RightBrace, "expected `}` after block");
    if (!right_brace) {
        return nullptr;
    }
    block->span = {.begin = left_brace->span.begin,
                   .end = right_brace->span.end};
    return block;
}

std::unique_ptr<Stmt> Parser::parse_statement() {
    if (match(TokenKind::KwStatic)) {
        const auto start = previous().span;
        if (!consume(TokenKind::KwLet, "expected `let` after `static`")) {
            return nullptr;
        }
        return parse_binding(BindingKind::Static, start);
    }
    if (match(TokenKind::KwRuntime)) {
        const auto start = previous().span;
        if (!consume(TokenKind::KwLet, "expected `let` after `runtime`")) {
            return nullptr;
        }
        return parse_binding(BindingKind::Runtime, start);
    }
    if (match(TokenKind::KwLet)) {
        return parse_binding(BindingKind::Inferred, previous().span);
    }
    if (match(TokenKind::KwReturn)) {
        return parse_return();
    }
    if (match(TokenKind::KwFor)) {
        return parse_for();
    }

    auto expression = parse_expression();
    if (!expression) {
        return nullptr;
    }
    const auto* semicolon =
        consume(TokenKind::Semicolon, "expected `;` after expression");
    if (!semicolon) {
        return nullptr;
    }
    const auto span = source::Span{.begin = expression->span.begin,
                                   .end = semicolon->span.end};
    return std::make_unique<ExpressionStmt>(std::move(expression), span);
}

std::unique_ptr<Stmt> Parser::parse_binding(BindingKind kind,
                                            source::Span start_span) {
    const auto* name = consume(TokenKind::Identifier, "expected binding name");
    if (!name) {
        return nullptr;
    }
    std::optional<Type> annotation;
    if (match(TokenKind::Colon)) {
        annotation = parse_type();
        if (!annotation) {
            return nullptr;
        }
    }
    if (!consume(TokenKind::Equal, "expected `=` in binding")) {
        return nullptr;
    }
    auto initializer = parse_expression();
    if (!initializer) {
        return nullptr;
    }
    const auto* semicolon =
        consume(TokenKind::Semicolon, "expected `;` after binding");
    if (!semicolon) {
        return nullptr;
    }
    const auto span =
        source::Span{.begin = start_span.begin, .end = semicolon->span.end};
    return std::make_unique<BindingStmt>(kind, std::string(name->lexeme),
                                         name->span, annotation,
                                         std::move(initializer), span);
}

std::unique_ptr<Stmt> Parser::parse_return() {
    const auto start = previous().span;
    auto value = parse_expression();
    if (!value) {
        return nullptr;
    }
    const auto* semicolon =
        consume(TokenKind::Semicolon, "expected `;` after return value");
    if (!semicolon) {
        return nullptr;
    }
    return std::make_unique<ReturnStmt>(
        std::move(value),
        source::Span{.begin = start.begin, .end = semicolon->span.end});
}

std::unique_ptr<Stmt> Parser::parse_for() {
    const auto start = previous().span;
    const auto* variable =
        consume(TokenKind::Identifier, "expected loop variable");
    if (!variable || !consume(TokenKind::KwIn, "expected `in` in range loop")) {
        return nullptr;
    }
    auto begin = parse_expression();
    if (!begin || !consume(TokenKind::Range, "expected `..` in range loop")) {
        return nullptr;
    }
    auto end = parse_expression();
    auto body = parse_block();
    if (!end || !body) {
        return nullptr;
    }
    const auto span = source::Span{.begin = start.begin, .end = body->span.end};
    return std::make_unique<ForStmt>(std::string(variable->lexeme),
                                     variable->span, std::move(begin),
                                     std::move(end), std::move(body), span);
}

std::unique_ptr<Expr> Parser::parse_expression() {
    if (check(TokenKind::KwIf)) {
        return parse_if();
    }
    return parse_logical_or();
}

std::unique_ptr<Expr> Parser::parse_if() {
    const auto* keyword = consume(TokenKind::KwIf, "expected `if`");
    auto condition = parse_expression();
    auto then_block = parse_block();
    if (!keyword || !condition || !then_block ||
        !consume(TokenKind::KwElse, "an `if` expression requires `else`")) {
        return nullptr;
    }

    std::unique_ptr<Block> else_block;
    if (check(TokenKind::KwIf)) {
        auto nested = parse_if();
        if (!nested) {
            return nullptr;
        }
        else_block = std::make_unique<Block>();
        else_block->span = nested->span;
        else_block->tail = std::move(nested);
    } else {
        else_block = parse_block();
    }
    if (!else_block) {
        return nullptr;
    }

    const auto span =
        source::Span{.begin = keyword->span.begin, .end = else_block->span.end};
    return std::make_unique<IfExpr>(std::move(condition), std::move(then_block),
                                    std::move(else_block), span);
}

std::unique_ptr<Expr> Parser::parse_binary(std::unique_ptr<Expr> left,
                                           BinaryOperator operation,
                                           std::unique_ptr<Expr> right) {
    if (!left || !right) {
        return nullptr;
    }
    const auto span = source::merge(left->span, right->span);
    return std::make_unique<BinaryExpr>(operation, std::move(left),
                                        std::move(right), span);
}

std::unique_ptr<Expr> Parser::parse_logical_or() {
    auto expression = parse_logical_and();
    while (match(TokenKind::OrOr)) {
        expression =
            parse_binary(std::move(expression), BinaryOperator::LogicalOr,
                         parse_logical_and());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_logical_and() {
    auto expression = parse_bitwise_or();
    while (match(TokenKind::AndAnd)) {
        expression =
            parse_binary(std::move(expression), BinaryOperator::LogicalAnd,
                         parse_bitwise_or());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_bitwise_or() {
    auto expression = parse_bitwise_xor();
    while (match(TokenKind::Pipe)) {
        expression = parse_binary(std::move(expression), BinaryOperator::BitOr,
                                  parse_bitwise_xor());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_bitwise_xor() {
    auto expression = parse_bitwise_and();
    while (match(TokenKind::Caret)) {
        expression = parse_binary(std::move(expression), BinaryOperator::BitXor,
                                  parse_bitwise_and());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_bitwise_and() {
    auto expression = parse_equality();
    while (match(TokenKind::Ampersand)) {
        expression = parse_binary(std::move(expression), BinaryOperator::BitAnd,
                                  parse_equality());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_equality() {
    auto expression = parse_comparison();
    while (check(TokenKind::EqualEqual) || check(TokenKind::BangEqual)) {
        const auto kind = current().kind;
        ++index_;
        const auto operation = kind == TokenKind::EqualEqual
                                   ? BinaryOperator::Equal
                                   : BinaryOperator::NotEqual;
        expression =
            parse_binary(std::move(expression), operation, parse_comparison());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_comparison() {
    auto expression = parse_shift();
    while (check(TokenKind::Less) || check(TokenKind::LessEqual) ||
           check(TokenKind::Greater) || check(TokenKind::GreaterEqual)) {
        const auto kind = current().kind;
        ++index_;
        BinaryOperator operation = BinaryOperator::Less;
        if (kind == TokenKind::LessEqual) {
            operation = BinaryOperator::LessEqual;
        } else if (kind == TokenKind::Greater) {
            operation = BinaryOperator::Greater;
        } else if (kind == TokenKind::GreaterEqual) {
            operation = BinaryOperator::GreaterEqual;
        }
        expression =
            parse_binary(std::move(expression), operation, parse_shift());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_shift() {
    auto expression = parse_additive();
    while (check(TokenKind::ShiftLeft) || check(TokenKind::ShiftRight)) {
        const auto kind = current().kind;
        ++index_;
        expression = parse_binary(std::move(expression),
                                  kind == TokenKind::ShiftLeft
                                      ? BinaryOperator::ShiftLeft
                                      : BinaryOperator::ShiftRight,
                                  parse_additive());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_additive() {
    auto expression = parse_multiplicative();
    while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        const auto kind = current().kind;
        ++index_;
        expression =
            parse_binary(std::move(expression),
                         kind == TokenKind::Plus ? BinaryOperator::Add
                                                 : BinaryOperator::Subtract,
                         parse_multiplicative());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_multiplicative() {
    auto expression = parse_cast();
    while (check(TokenKind::Star) || check(TokenKind::Slash) ||
           check(TokenKind::Percent)) {
        const auto kind = current().kind;
        ++index_;
        BinaryOperator operation = BinaryOperator::Multiply;
        if (kind == TokenKind::Slash) {
            operation = BinaryOperator::Divide;
        } else if (kind == TokenKind::Percent) {
            operation = BinaryOperator::Remainder;
        }
        expression =
            parse_binary(std::move(expression), operation, parse_cast());
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_cast() {
    auto expression = parse_unary();
    while (expression && match(TokenKind::KwAs)) {
        const auto destination = parse_type();
        if (!destination)
            return nullptr;
        const auto span = source::Span{.begin = expression->span.begin,
                                       .end = previous().span.end};
        expression = std::make_unique<CastExpr>(std::move(expression),
                                                *destination, span);
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_unary() {
    if (match(TokenKind::Bang) || match(TokenKind::Tilde) ||
        match(TokenKind::Minus)) {
        const auto operation =
            previous().kind == TokenKind::Bang    ? UnaryOperator::Not
            : previous().kind == TokenKind::Tilde ? UnaryOperator::BitNot
                                                  : UnaryOperator::Negate;
        const auto start = previous().span;
        auto operand = parse_unary();
        if (!operand) {
            return nullptr;
        }
        const auto span =
            source::Span{.begin = start.begin, .end = operand->span.end};
        // Keep a signed literal's sign as source syntax so its minimum value
        // need not first fit as a positive value. A second minus is an
        // operator.
        if (operation == UnaryOperator::Negate &&
            operand->kind == Expr::Kind::Integer) {
            auto& literal = static_cast<IntegerExpr&>(*operand);
            if (literal.is_signed && !literal.spelling.starts_with('-')) {
                literal.spelling.insert(0, "-");
                literal.span = span;
                return operand;
            }
        }
        return std::make_unique<UnaryExpr>(operation, std::move(operand), span);
    }
    return parse_call();
}

std::unique_ptr<Expr> Parser::parse_call() {
    auto expression = parse_primary();
    while (expression) {
        if (match(TokenKind::Dot)) {
            const auto* field =
                consume(TokenKind::Identifier, "expected field name after `.`");
            if (!field) {
                return nullptr;
            }
            const auto span = source::Span{.begin = expression->span.begin,
                                           .end = field->span.end};
            expression = std::make_unique<FieldExpr>(std::move(expression),
                                                     std::string(field->lexeme),
                                                     field->span, span);
            continue;
        }
        if (match(TokenKind::LeftBracket)) {
            auto index = parse_expression();
            if (!index) {
                return nullptr;
            }
            const auto* right =
                consume(TokenKind::RightBracket, "expected `]` after index");
            if (!right) {
                return nullptr;
            }
            const auto span = source::Span{.begin = expression->span.begin,
                                           .end = right->span.end};
            expression = std::make_unique<IndexExpr>(std::move(expression),
                                                     std::move(index), span);
            continue;
        }
        if (!match(TokenKind::LeftParen)) {
            break;
        }
        auto* name = dynamic_cast<NameExpr*>(expression.get());
        if (!name) {
            diagnostics_.error("E1002", "only named functions can be called",
                               expression->span);
            return nullptr;
        }
        if (check(TokenKind::Identifier) && index_ + 1 < tokens_.size() &&
            tokens_[index_ + 1].kind == TokenKind::Colon) {
            std::vector<FieldInitializer> fields;
            do {
                const auto* field = consume(TokenKind::Identifier,
                                            "expected initializer field name");
                if (!field || !consume(TokenKind::Colon,
                                       "expected `:` after field name")) {
                    return nullptr;
                }
                auto value = parse_expression();
                if (!value) {
                    return nullptr;
                }
                fields.push_back({.name = std::string(field->lexeme),
                                  .name_span = field->span,
                                  .value = std::move(value)});
            } while (match(TokenKind::Comma) && !check(TokenKind::RightParen));
            const auto* end = consume(TokenKind::RightParen,
                                      "expected `)` after record fields");
            if (!end) {
                return nullptr;
            }
            const auto span = source::Span{.begin = expression->span.begin,
                                           .end = end->span.end};
            expression = std::make_unique<RecordExpr>(name->name, name->span,
                                                      std::move(fields), span);
            continue;
        }
        std::vector<std::unique_ptr<Expr>> arguments;
        if (!check(TokenKind::RightParen)) {
            do {
                auto argument = parse_expression();
                if (!argument) {
                    return nullptr;
                }
                arguments.push_back(std::move(argument));
            } while (match(TokenKind::Comma));
        }
        const auto* right =
            consume(TokenKind::RightParen, "expected `)` after arguments");
        if (!right) {
            return nullptr;
        }
        const auto span = source::Span{.begin = expression->span.begin,
                                       .end = right->span.end};
        auto callee = name->name;
        const auto callee_span = name->span;
        expression = std::make_unique<CallExpr>(std::move(callee), callee_span,
                                                std::move(arguments), span);
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_primary() {
    if (match(TokenKind::LeftBracket)) {
        const auto start_span = previous().span;
        if (match(TokenKind::KwFor)) {
            return parse_array_generator(start_span);
        }
        const auto start = previous().span.begin;
        std::vector<std::unique_ptr<Expr>> elements;
        if (!check(TokenKind::RightBracket)) {
            do {
                auto element = parse_expression();
                if (!element) {
                    return nullptr;
                }
                elements.push_back(std::move(element));
            } while (match(TokenKind::Comma) &&
                     !check(TokenKind::RightBracket));
        }
        const auto* right = consume(TokenKind::RightBracket,
                                    "expected `]` after array literal");
        if (!right) {
            return nullptr;
        }
        return std::make_unique<ArrayExpr>(
            std::move(elements),
            source::Span{.begin = start, .end = right->span.end});
    }
    if (match(TokenKind::Integer)) {
        const auto token = previous();
        const auto suffix_start = token.lexeme.find_first_not_of("0123456789_");
        auto literal = std::make_unique<IntegerExpr>(
            std::string(token.lexeme.substr(0, suffix_start)), token.span);
        if (suffix_start != std::string_view::npos) {
            const auto suffix = token.lexeme.substr(suffix_start);
            if (suffix == "u8" || suffix == "i8")
                literal->width = 8;
            else if (suffix == "u16" || suffix == "i16")
                literal->width = 16;
            else if (suffix == "u32" || suffix == "i32")
                literal->width = 32;
            else if (suffix == "u64" || suffix == "i64")
                literal->width = 64;
            else {
                diagnostics_.error("E1005", "invalid integer literal suffix",
                                   token.span,
                                   "expected u8/u16/u32/u64 or i8/i16/i32/i64");
                return nullptr;
            }
            literal->is_signed = suffix.front() == 'i';
        }
        return literal;
    }
    if (match(TokenKind::KwTrue)) {
        return std::make_unique<BooleanExpr>(true, previous().span);
    }
    if (match(TokenKind::KwFalse)) {
        return std::make_unique<BooleanExpr>(false, previous().span);
    }
    if (match(TokenKind::Identifier)) {
        return std::make_unique<NameExpr>(std::string(previous().lexeme),
                                          previous().span);
    }
    if (match(TokenKind::LeftParen)) {
        auto expression = parse_expression();
        if (!consume(TokenKind::RightParen,
                     "expected `)` after parenthesized expression")) {
            return nullptr;
        }
        return expression;
    }

    diagnostics_.error("E1002", "expected expression", current().span,
                       "found " + std::string(token_kind_name(current().kind)));
    return nullptr;
}

std::unique_ptr<Expr> Parser::parse_array_generator(source::Span start) {
    const auto* variable =
        consume(TokenKind::Identifier, "expected array generator index name");
    if (!variable ||
        !consume(TokenKind::KwIn, "expected `in` after generator index")) {
        return nullptr;
    }
    auto begin = parse_expression();
    if (!begin ||
        !consume(TokenKind::Range, "expected `..` between generator bounds")) {
        return nullptr;
    }
    auto end = parse_expression();
    if (!end) {
        return nullptr;
    }
    auto body = parse_block();
    if (!body) {
        return nullptr;
    }
    const auto* right =
        consume(TokenKind::RightBracket, "expected `]` after array generator");
    if (!right) {
        return nullptr;
    }
    return std::make_unique<ArrayGeneratorExpr>(
        std::string(variable->lexeme), variable->span, std::move(begin),
        std::move(end), std::move(body),
        source::Span{.begin = start.begin, .end = right->span.end});
}

} // namespace pagos::syntax
