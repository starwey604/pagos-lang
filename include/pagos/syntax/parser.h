#pragma once

#include "pagos/source/diagnostic.h"
#include "pagos/syntax/ast.h"
#include "pagos/syntax/token.h"

#include <memory>
#include <vector>

namespace pagos::syntax {

class Parser {
  public:
    Parser(const std::vector<Token>& tokens,
           source::DiagnosticEngine& diagnostics)
        : tokens_(tokens), diagnostics_(diagnostics) {}

    [[nodiscard]] std::unique_ptr<Module> parse_module();

  private:
    [[nodiscard]] const Token& current() const noexcept;
    [[nodiscard]] const Token& previous() const noexcept;
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] bool check(TokenKind kind) const noexcept;
    bool match(TokenKind kind) noexcept;
    const Token* consume(TokenKind kind, std::string message);
    void synchronize();

    std::unique_ptr<Function> parse_function();
    std::unique_ptr<Block> parse_block();
    std::unique_ptr<Stmt> parse_statement();
    std::unique_ptr<Stmt> parse_binding(BindingKind kind,
                                        source::Span start_span);
    std::unique_ptr<Stmt> parse_return();
    std::unique_ptr<Stmt> parse_for();

    std::unique_ptr<Expr> parse_expression();
    std::unique_ptr<Expr> parse_if();
    std::unique_ptr<Expr> parse_logical_or();
    std::unique_ptr<Expr> parse_logical_and();
    std::unique_ptr<Expr> parse_equality();
    std::unique_ptr<Expr> parse_comparison();
    std::unique_ptr<Expr> parse_additive();
    std::unique_ptr<Expr> parse_multiplicative();
    std::unique_ptr<Expr> parse_unary();
    std::unique_ptr<Expr> parse_call();
    std::unique_ptr<Expr> parse_primary();
    std::unique_ptr<Expr> parse_binary(std::unique_ptr<Expr> left,
                                       BinaryOperator operation,
                                       std::unique_ptr<Expr> right);

    std::optional<TypeKind> parse_type();

    const std::vector<Token>& tokens_;
    source::DiagnosticEngine& diagnostics_;
    std::size_t index_{};
};

} // namespace pagos::syntax
