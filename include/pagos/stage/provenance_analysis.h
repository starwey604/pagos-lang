#pragma once

#include "pagos/hir/hir.h"
#include "pagos/stage/provenance_graph.h"

#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace pagos::stage {

class ProvenanceRecorder;

struct ProvenanceSite {
    source::Span span;
    std::string label;
};

struct ProvenanceRoot {
    std::string name;
    source::Span span;
    hir::Stage stage;
    provenance::NodeId node;
    bool requires_comptime{};
};

// Published with the HIR module. Owns its graph and source metadata
// independently of the analyzer, AST, type table and HIR expression addresses.
class ProvenanceAnalysis {
  public:
    [[nodiscard]] const provenance::FrozenGraph& graph() const noexcept {
        return graph_;
    }
    [[nodiscard]] const ProvenanceSite& site(provenance::SiteId id) const;
    [[nodiscard]] std::span<const ProvenanceRoot> roots() const noexcept {
        return roots_;
    }

  private:
    friend class ProvenanceRecorder;
    ProvenanceAnalysis(provenance::FrozenGraph graph,
                       std::vector<ProvenanceSite> sites,
                       std::vector<ProvenanceRoot> roots);
    provenance::FrozenGraph graph_;
    std::vector<ProvenanceSite> sites_;
    std::vector<ProvenanceRoot> roots_;
};

void explain_provenance(const ProvenanceAnalysis& analysis,
                        std::ostream& output);

} // namespace pagos::stage
