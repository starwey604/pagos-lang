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
    std::size_t array_elements{65'536};
    std::size_t array_bytes{262'144};
    // Cumulative construction quotas shared by arrays and records, not RSS.
    std::size_t aggregate_members{65'536};
    std::size_t aggregate_bytes{262'144};
};

struct AnalysisStats {
    std::size_t fuel_consumed{};
    std::size_t specializations{};
    std::size_t cache_hits{};
    std::size_t maximum_recursion_depth{};
    std::size_t array_elements_reserved{};
    std::size_t array_bytes_reserved{};
    std::size_t aggregate_constructions{};
    std::size_t aggregate_members_reserved{};
    std::size_t aggregate_bytes_reserved{};
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
        std::vector<hir::ExprPtr> effects;
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
    BlockResult analyze_for(const syntax::ForStmt& loop_statement);
    hir::ExprPtr analyze_expression(const syntax::Expr& expression);
    hir::ExprPtr analyze_unary(const syntax::UnaryExpr& expression);
    hir::ExprPtr analyze_binary(const syntax::BinaryExpr& expression);
    hir::ExprPtr analyze_call(const syntax::CallExpr& expression);
    hir::ExprPtr analyze_if(const syntax::IfExpr& expression);
    hir::ExprPtr analyze_array(const syntax::ArrayExpr& expression);
    hir::ExprPtr
    analyze_array_generator(const syntax::ArrayGeneratorExpr& expression);
    hir::ExprPtr finish_array(std::vector<hir::ExprPtr> elements,
                              const syntax::Type& type, source::Span span);
    bool reserve_array(std::size_t count, const syntax::Type& type,
                       source::Span span);
    bool reserve_aggregate(std::size_t bool_members, std::size_t u32_members,
                           source::Span span);
    hir::ExprPtr analyze_index(const syntax::IndexExpr& expression);
    hir::ExprPtr analyze_record(const syntax::RecordExpr& expression);
    hir::ExprPtr analyze_field(const syntax::FieldExpr& expression);

    void define(const std::string& name, hir::ExprPtr value);
    [[nodiscard]] hir::ExprPtr lookup(const std::string& name) const;
    [[nodiscard]] hir::ExprPtr make_runtime(
        hir::Expr::Kind kind, syntax::Type type, source::Span span,
        std::vector<hir::ExprPtr> operands, hir::RuntimeTrace trace,
        std::optional<syntax::UnaryOperator> unary = std::nullopt,
        std::optional<syntax::BinaryOperator> binary = std::nullopt) const;
    [[nodiscard]] hir::ExprPtr make_sequence(std::vector<hir::ExprPtr> effects,
                                             hir::ExprPtr value,
                                             source::Span span) const;
    [[nodiscard]] hir::ExprPtr resolve_return(const hir::ExprPtr& value) const;
    void report_static_failure(const syntax::BindingStmt& binding,
                               const hir::RuntimeTrace& trace);
    bool check_resolved_type(const syntax::Type& expected,
                             const hir::ExprPtr& value, source::Span span,
                             const std::string& context);
    [[nodiscard]] syntax::Type type_of(const syntax::Expr& expression) const;
    [[nodiscard]] bool consume_fuel(source::Span span);
    [[nodiscard]] std::optional<SpecializationKey>
    specialization_key(const syntax::Function& function,
                       const std::vector<hir::ExprPtr>& arguments) const;

    source::DiagnosticEngine& diagnostics_;
    std::unordered_map<std::string, const syntax::Module::Record*> records_;
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
    bool construction_budget_exhausted_{};
};

} // namespace pagos::stage
