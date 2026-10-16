#pragma once

#include "pagos/stage/cache_identity.h"
#include "pagos/stage/provenance_analysis.h"

#include <memory>
#include <unordered_map>

namespace pagos::stage {

// Private single-task adapter. It records the decisions already made by
// staging; graph queries never feed back into Stage inference or
// residualization.
class ProvenanceRecorder {
  public:
    explicit ProvenanceRecorder(provenance::EvidenceBuildConfig config = {});
    ~ProvenanceRecorder();
    void identity(const syntax::Module& module, const sema::TypeTable& types,
                  const cache::QueryContext& context);
    void binding(const syntax::BindingStmt& binding, const hir::ExprPtr& value,
                 bool top_level);
    void storage(const syntax::Module::Global& global,
                 const hir::ExprPtr& initializer);
    void enter_call(
        const std::shared_ptr<const hir::ResidualFunction>& callee,
        const std::unordered_map<const hir::Expr*, hir::ExprPtr>& arguments);
    void leave_call();
    void
    specialization(const std::shared_ptr<const hir::ResidualFunction>& callee,
                   const syntax::Function& declaration,
                   std::span<const hir::ExprPtr> arguments);
    void runtime_boundary(const hir::ExprPtr& value,
                          const hir::ExprPtr& initializer);
    void selection(const hir::ExprPtr& value, StageRule rule,
                   const hir::ExprPtr& condition,
                   const hir::ExprPtr& then_value,
                   const hir::ExprPtr& else_value);
    void projection(const hir::ExprPtr& value, const hir::ExprPtr& aggregate,
                    StageReason reason, std::optional<std::uint32_t> member,
                    const hir::ExprPtr& selected = {},
                    const hir::ExprPtr& index = {});
    void conservative_completion(
        const std::shared_ptr<const hir::ResidualFunction>& callee);
    void instantiate_inline(
        const hir::ExprPtr& body,
        const std::unordered_map<const hir::Expr*, hir::ExprPtr>& arguments,
        const std::unordered_map<const hir::Expr*, hir::ExprPtr>& instances);
    void loop_index(const hir::ExprPtr& index, const hir::ExprPtr& begin,
                    const hir::ExprPtr& end);
    [[nodiscard]] std::shared_ptr<const ProvenanceAnalysis>
    finish(const hir::Module& module);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pagos::stage
