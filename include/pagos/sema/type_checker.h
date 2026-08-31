#pragma once

#include "pagos/source/diagnostic.h"
#include "pagos/syntax/ast.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace pagos::sema {

using TypeTable = std::unordered_map<const syntax::Expr*, syntax::TypeKind>;

class TypeChecker {
  public:
    explicit TypeChecker(source::DiagnosticEngine& diagnostics)
        : diagnostics_(diagnostics) {}

    [[nodiscard]] bool check(syntax::Module& module);
    [[nodiscard]] const TypeTable& types() const noexcept { return types_; }

  private:
    using Scope = std::unordered_map<std::string, syntax::TypeKind>;

    void collect_functions(syntax::Module& module);
    void check_function(syntax::Function& function);
    syntax::TypeKind check_block(syntax::Block& block,
                                 syntax::TypeKind expected_return,
                                 bool& saw_return);
    void check_statement(syntax::Stmt& statement,
                         syntax::TypeKind expected_return, bool& saw_return);
    syntax::TypeKind check_expression(syntax::Expr& expression);
    syntax::TypeKind check_binary(syntax::BinaryExpr& expression);
    syntax::TypeKind check_call(syntax::CallExpr& expression);
    syntax::TypeKind check_if(syntax::IfExpr& expression);

    bool define(std::string name, syntax::TypeKind type,
                source::Span name_span);
    [[nodiscard]] syntax::TypeKind lookup(const std::string& name,
                                          source::Span span);
    void type_mismatch(source::Span span, syntax::TypeKind expected,
                       syntax::TypeKind actual, std::string context);

    source::DiagnosticEngine& diagnostics_;
    std::unordered_map<std::string, syntax::Function*> functions_;
    std::vector<Scope> scopes_;
    TypeTable types_;
    syntax::TypeKind current_return_type_{syntax::TypeKind::Void};
};

} // namespace pagos::sema
