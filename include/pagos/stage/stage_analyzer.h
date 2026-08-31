#pragma once

#include "pagos/hir/hir.h"
#include "pagos/sema/type_checker.h"
#include "pagos/source/diagnostic.h"
#include "pagos/syntax/ast.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pagos::stage {

struct AnalysisLimits {
    std::size_t fuel{1'000'000};
    std::size_t recursion_depth{128};
    std::size_t specializations{4'096};
};

struct AnalysisStats {
    std::size_t fuel_consumed{};
    std::size_t specializations{};
    std::size_t cache_hits{};
    std::size_t maximum_recursion_depth{};
};

class StageAnalyzer {
  public:
    StageAnalyzer(source::DiagnosticEngine& diagnostics,
                  const sema::TypeTable& types, AnalysisLimits limits = {})
        : diagnostics_(diagnostics), types_(types), limits_(limits) {}

    [[nodiscard]] std::unique_ptr<hir::Module>
    analyze(const syntax::Module& module);
    [[nodiscard]] const AnalysisStats& stats() const noexcept { return stats_; }

  private:
    struct BlockResult {
        hir::ExprPtr value;
        bool returned{};
    };

    using Scope = std::unordered_map<std::string, hir::ExprPtr>;

    struct SpecializationKey {
        const syntax::Function* function;
        std::vector<hir::Constant> arguments;

        bool operator==(const SpecializationKey&) const = default;
    };

    struct SpecializationKeyHash {
        std::size_t operator()(const SpecializationKey& key) const noexcept;
    };

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
    [[nodiscard]] bool consume_fuel(source::Span span);
    [[nodiscard]] std::optional<SpecializationKey>
    specialization_key(const syntax::Function& function,
                       const std::vector<hir::ExprPtr>& arguments) const;

    source::DiagnosticEngine& diagnostics_;
    const sema::TypeTable& types_;
    AnalysisLimits limits_;
    AnalysisStats stats_;
    std::unordered_map<std::string, const syntax::Function*> functions_;
    std::vector<Scope> scopes_;
    std::vector<std::string> call_stack_;
    std::unordered_map<SpecializationKey, hir::ExprPtr, SpecializationKeyHash>
        specialization_cache_;
    std::unordered_set<SpecializationKey, SpecializationKeyHash>
        active_specializations_;
    bool fuel_exhausted_{};
};

} // namespace pagos::stage
