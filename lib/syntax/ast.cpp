#include "pagos/syntax/ast.h"

#include <string_view>

namespace pagos::syntax {

IfExpr::IfExpr(std::unique_ptr<Expr> condition,
               std::unique_ptr<Block> then_block,
               std::unique_ptr<Block> else_block, source::Span span)
    : Expr(Kind::If, span), condition(std::move(condition)),
      then_block(std::move(then_block)), else_block(std::move(else_block)) {}

IfExpr::~IfExpr() = default;

ForStmt::ForStmt(std::string variable, source::Span variable_span,
                 std::unique_ptr<Expr> begin, std::unique_ptr<Expr> end,
                 std::unique_ptr<Block> body, source::Span span)
    : Stmt(Kind::For, span), variable(std::move(variable)),
      variable_span(variable_span), begin(std::move(begin)),
      end(std::move(end)), body(std::move(body)) {}

ForStmt::~ForStmt() = default;

std::string_view type_name(TypeKind type) noexcept {
    switch (type) {
    case TypeKind::Unknown:
        return "<unknown>";
    case TypeKind::Void:
        return "void";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::U32:
        return "u32";
    case TypeKind::Error:
        return "<error>";
    }
    return "<error>";
}

} // namespace pagos::syntax
