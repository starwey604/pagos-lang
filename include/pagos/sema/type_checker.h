#pragma once

#include "pagos/source/diagnostic.h"
#include "pagos/syntax/ast.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace pagos::sema {

using TypeTable = std::unordered_map<const syntax::Expr*, syntax::Type>;

class TypeChecker {
  public:
    explicit TypeChecker(source::DiagnosticEngine& diagnostics)
        : diagnostics_(diagnostics) {}

    [[nodiscard]] bool check(syntax::Module& module);
    [[nodiscard]] const TypeTable& types() const noexcept { return types_; }

  private:
    using Scope = std::unordered_map<std::string, syntax::Type>;

    void collect_functions(syntax::Module& module);
    void check_function(syntax::Function& function);
    syntax::Type check_block(syntax::Block& block, syntax::Type expected_return,
                             bool& saw_return);
    void check_statement(syntax::Stmt& statement, syntax::Type expected_return,
                         bool& saw_return);
    syntax::Type check_expression(syntax::Expr& expression);
    syntax::Type check_binary(syntax::BinaryExpr& expression);
    syntax::Type check_call(syntax::CallExpr& expression);
    syntax::Type check_if(syntax::IfExpr& expression);

    bool define(std::string name, syntax::Type type, source::Span name_span);
    [[nodiscard]] syntax::Type lookup(const std::string& name,
                                      source::Span span);
    void type_mismatch(source::Span span, syntax::Type expected,
                       syntax::Type actual, std::string context);

    source::DiagnosticEngine& diagnostics_;
    std::unordered_map<std::string, syntax::Function*> functions_;
    std::vector<Scope> scopes_;
    TypeTable types_;
    syntax::Type current_return_type_{syntax::TypeKind::Void};
};

} // namespace pagos::sema
