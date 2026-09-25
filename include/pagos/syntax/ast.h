#pragma once

#include "pagos/source/span.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pagos::syntax {

enum class TypeKind {
    Unknown,
    Void,
    Bool,
    U32,
    Never,
    Error,
    ArrayBool,
    ArrayU32
};

struct Type {
    TypeKind kind{TypeKind::Error};
    std::uint32_t length{};

    Type() = default;
    Type(TypeKind kind, std::uint32_t length = 0)
        : kind(kind), length(length) {}
    bool operator==(const Type&) const = default;
    [[nodiscard]] bool is_array() const noexcept {
        return kind == TypeKind::ArrayBool || kind == TypeKind::ArrayU32;
    }
    [[nodiscard]] Type element_type() const noexcept {
        return kind == TypeKind::ArrayBool ? TypeKind::Bool : TypeKind::U32;
    }
    [[nodiscard]] static Type array(Type element, std::uint32_t length) {
        return {element == TypeKind::Bool ? TypeKind::ArrayBool
                                          : TypeKind::ArrayU32,
                length};
    }
};
enum class BindingKind { Inferred, Static, Runtime };
enum class UnaryOperator { Not, BitNot };
enum class BinaryOperator {
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    BitAnd,
    BitOr,
    BitXor,
    ShiftLeft,
    ShiftRight,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LogicalAnd,
    LogicalOr,
};

struct Block;

struct Expr {
    enum class Kind {
        Integer,
        Boolean,
        Name,
        Unary,
        Binary,
        Call,
        If,
        Array,
        ArrayGenerator,
        Index
    };

    Expr(Kind kind, source::Span span) : kind(kind), span(span) {}
    virtual ~Expr() = default;

    Kind kind;
    source::Span span;
};

struct IntegerExpr final : Expr {
    IntegerExpr(std::string spelling, source::Span span)
        : Expr(Kind::Integer, span), spelling(std::move(spelling)) {}
    std::string spelling;
};

struct BooleanExpr final : Expr {
    BooleanExpr(bool value, source::Span span)
        : Expr(Kind::Boolean, span), value(value) {}
    bool value;
};

struct NameExpr final : Expr {
    NameExpr(std::string name, source::Span span)
        : Expr(Kind::Name, span), name(std::move(name)) {}
    std::string name;
};

struct UnaryExpr final : Expr {
    UnaryExpr(UnaryOperator operation, std::unique_ptr<Expr> operand,
              source::Span span)
        : Expr(Kind::Unary, span), operation(operation),
          operand(std::move(operand)) {}
    UnaryOperator operation;
    std::unique_ptr<Expr> operand;
};

struct BinaryExpr final : Expr {
    BinaryExpr(BinaryOperator operation, std::unique_ptr<Expr> left,
               std::unique_ptr<Expr> right, source::Span span)
        : Expr(Kind::Binary, span), operation(operation), left(std::move(left)),
          right(std::move(right)) {}
    BinaryOperator operation;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

struct CallExpr final : Expr {
    CallExpr(std::string callee, source::Span callee_span,
             std::vector<std::unique_ptr<Expr>> arguments, source::Span span)
        : Expr(Kind::Call, span), callee(std::move(callee)),
          callee_span(callee_span), arguments(std::move(arguments)) {}
    std::string callee;
    source::Span callee_span;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct IfExpr final : Expr {
    IfExpr(std::unique_ptr<Expr> condition, std::unique_ptr<Block> then_block,
           std::unique_ptr<Block> else_block, source::Span span);
    ~IfExpr() override;

    std::unique_ptr<Expr> condition;
    std::unique_ptr<Block> then_block;
    std::unique_ptr<Block> else_block;
};

struct ArrayExpr final : Expr {
    ArrayExpr(std::vector<std::unique_ptr<Expr>> elements, source::Span span)
        : Expr(Kind::Array, span), elements(std::move(elements)) {}
    std::vector<std::unique_ptr<Expr>> elements;
};

struct IndexExpr final : Expr {
    IndexExpr(std::unique_ptr<Expr> array, std::unique_ptr<Expr> index,
              source::Span span)
        : Expr(Kind::Index, span), array(std::move(array)),
          index(std::move(index)) {}
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> index;
};

struct ArrayGeneratorExpr final : Expr {
    ArrayGeneratorExpr(std::string variable, source::Span variable_span,
                       std::uint32_t begin, std::uint32_t end,
                       std::unique_ptr<Block> body, source::Span span);
    ~ArrayGeneratorExpr() override;

    std::string variable;
    source::Span variable_span;
    std::uint32_t begin;
    std::uint32_t end;
    std::unique_ptr<Block> body;
};

struct Stmt {
    enum class Kind { Binding, Return, Expression, For };

    Stmt(Kind kind, source::Span span) : kind(kind), span(span) {}
    virtual ~Stmt() = default;

    Kind kind;
    source::Span span;
};

struct BindingStmt final : Stmt {
    BindingStmt(BindingKind binding_kind, std::string name,
                source::Span name_span, std::optional<Type> annotation,
                std::unique_ptr<Expr> initializer, source::Span span)
        : Stmt(Kind::Binding, span), binding_kind(binding_kind),
          name(std::move(name)), name_span(name_span), annotation(annotation),
          initializer(std::move(initializer)) {}

    BindingKind binding_kind;
    std::string name;
    source::Span name_span;
    std::optional<Type> annotation;
    std::unique_ptr<Expr> initializer;
};

struct ReturnStmt final : Stmt {
    ReturnStmt(std::unique_ptr<Expr> value, source::Span span)
        : Stmt(Kind::Return, span), value(std::move(value)) {}
    std::unique_ptr<Expr> value;
};

struct ExpressionStmt final : Stmt {
    ExpressionStmt(std::unique_ptr<Expr> expression, source::Span span)
        : Stmt(Kind::Expression, span), expression(std::move(expression)) {}
    std::unique_ptr<Expr> expression;
};

struct ForStmt final : Stmt {
    ForStmt(std::string variable, source::Span variable_span,
            std::unique_ptr<Expr> begin, std::unique_ptr<Expr> end,
            std::unique_ptr<Block> body, source::Span span);
    ~ForStmt() override;

    std::string variable;
    source::Span variable_span;
    std::unique_ptr<Expr> begin;
    std::unique_ptr<Expr> end;
    std::unique_ptr<Block> body;
};

struct Block {
    std::vector<std::unique_ptr<Stmt>> statements;
    std::unique_ptr<Expr> tail;
    source::Span span;
};

struct Parameter {
    std::string name;
    source::Span name_span;
    Type type{TypeKind::Error};
};

struct Function {
    std::string name;
    source::Span name_span;
    std::vector<Parameter> parameters;
    Type result{TypeKind::Error};
    std::unique_ptr<Block> body;
    source::Span span;
};

struct Module {
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<std::unique_ptr<Stmt>> statements;
};

[[nodiscard]] std::string type_name(Type type);

} // namespace pagos::syntax
