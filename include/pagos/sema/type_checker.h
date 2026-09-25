#pragma once

#include "pagos/sema/type.h"

#include "pagos/source/diagnostic.h"
#include "pagos/syntax/ast.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace pagos::sema {

using TypeTable = std::unordered_map<const syntax::Expr*, sema::Type>;

class TypeChecker {
  public:
    explicit TypeChecker(source::DiagnosticEngine& diagnostics)
        : diagnostics_(diagnostics) {}

    [[nodiscard]] bool check(syntax::Module& module);
    [[nodiscard]] const TypeTable& types() const noexcept { return types_; }

  private:
    using Scope = std::unordered_map<std::string, sema::Type>;

    void collect_functions(syntax::Module& module);
    void collect_records(const syntax::Module& module);
    void validate_type(const sema::Type& type, source::Span span);
    sema::Type check_record(syntax::RecordExpr& expression);
    sema::Type check_field(syntax::FieldExpr& expression);
    void check_function(syntax::Function& function);
    sema::Type check_block(syntax::Block& block,
                           const sema::Type& expected_return, bool& saw_return);
    void check_statement(syntax::Stmt& statement,
                         const sema::Type& expected_return, bool& saw_return);
    sema::Type check_expression(syntax::Expr& expression);
    sema::Type check_binary(syntax::BinaryExpr& expression);
    sema::Type check_call(syntax::CallExpr& expression);
    sema::Type check_if(syntax::IfExpr& expression);

    bool define(std::string name, const sema::Type& type,
                source::Span name_span);
    [[nodiscard]] sema::Type lookup(const std::string& name, source::Span span);
    void type_mismatch(source::Span span, const sema::Type& expected,
                       const sema::Type& actual, std::string context);

    source::DiagnosticEngine& diagnostics_;
    std::unordered_map<std::string, syntax::Function*> functions_;
    std::unordered_map<std::string, const syntax::Module::Record*> records_;
    std::vector<Scope> scopes_;
    TypeTable types_;
    sema::Type current_return_type_{sema::TypeKind::Void};
};

} // namespace pagos::sema
