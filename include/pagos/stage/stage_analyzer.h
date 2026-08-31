#pragma once

#include "pagos/hir/hir.h"
#include "pagos/sema/type_checker.h"
#include "pagos/source/diagnostic.h"
#include "pagos/syntax/ast.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pagos::stage {

class StageAnalyzer {
  public:
    StageAnalyzer(source::DiagnosticEngine& diagnostics,
                  const sema::TypeTable& types)
        : diagnostics_(diagnostics), types_(types) {}

    [[nodiscard]] std::unique_ptr<hir::Module>
    analyze(const syntax::Module& module);

  private:
    struct BlockResult {
        hir::ExprPtr value;
        bool returned{};
    };

    using Scope = std::unordered_map<std::string, hir::ExprPtr>;

    BlockResult analyze_block(const syntax::Block& block);
    BlockResult analyze_statement(const syntax::Stmt& statement,
                                  hir::Module* output_module);
    hir::ExprPtr analyze_binding(const syntax::BindingStmt& binding,
                                 hir::Module* output_module);
    hir::ExprPtr analyze_expression(const syntax::Expr& expression);
    hir::ExprPtr analyze_unary(const syntax::UnaryExpr& expression);
    hir::ExprPtr analyze_binary(const syntax::BinaryExpr& expression);
    hir::ExprPtr analyze_call(const syntax::CallExpr& expression);
    hir::ExprPtr analyze_if(const syntax::IfExpr& expression);

    void define(const std::string& name, hir::ExprPtr value);
    [[nodiscard]] hir::ExprPtr lookup(const std::string& name) const;
    [[nodiscard]] hir::ExprPtr make_runtime(
        hir::Expr::Kind kind, syntax::TypeKind type, source::Span span,
        std::vector<hir::ExprPtr> operands, hir::RuntimeTrace trace,
        std::optional<syntax::UnaryOperator> unary = std::nullopt,
        std::optional<syntax::BinaryOperator> binary = std::nullopt) const;
    void report_static_failure(const syntax::BindingStmt& binding,
                               const hir::RuntimeTrace& trace);
    [[nodiscard]] syntax::TypeKind
    type_of(const syntax::Expr& expression) const;

    source::DiagnosticEngine& diagnostics_;
    const sema::TypeTable& types_;
    std::unordered_map<std::string, const syntax::Function*> functions_;
    std::vector<Scope> scopes_;
    std::vector<std::string> call_stack_;
};

} // namespace pagos::stage
