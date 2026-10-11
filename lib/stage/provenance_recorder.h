#pragma once

#include "pagos/stage/provenance_analysis.h"

#include <memory>
#include <unordered_map>

namespace pagos::stage {

// Private single-task adapter. It records the decisions already made by
// staging; graph queries never feed back into Stage inference or
// residualization.
class ProvenanceRecorder {
  public:
    ProvenanceRecorder();
    ~ProvenanceRecorder();
    void binding(const syntax::BindingStmt& binding, const hir::ExprPtr& value,
                 bool top_level);
    void storage(const syntax::Module::Global& global,
                 const hir::ExprPtr& initializer);
    void enter_call(
        const std::shared_ptr<const hir::ResidualFunction>& callee,
        const std::unordered_map<const hir::Expr*, hir::ExprPtr>& arguments);
    void leave_call();
    void projection(const hir::ExprPtr& value, const hir::ExprPtr& aggregate,
                    std::uint32_t member, const hir::ExprPtr& index = {});
    void loop_index(const hir::ExprPtr& index, const hir::ExprPtr& begin,
                    const hir::ExprPtr& end);
    [[nodiscard]] std::shared_ptr<const ProvenanceAnalysis>
    finish(const hir::Module& module);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pagos::stage
