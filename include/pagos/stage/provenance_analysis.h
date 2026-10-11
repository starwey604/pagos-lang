#pragma once

#include "pagos/hir/hir.h"
#include "pagos/stage/provenance_graph.h"

#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pagos::stage {

class ProvenanceRecorder;

// Semantic rules supplied by StageAnalyzer, distinct from graph storage rules.
enum class StageRule {
    RuntimeBoundary,
    RuntimeSelection,
    OperandPropagation,
    MemberProjection,
    ConservativeFallback,
};

enum class StageReason {
    None,
    KnownMember,
    RuntimeMember,
    OpaqueAggregate,
    RuntimeIndex,
    RecursiveCompletion,
};

struct StageDecision {
    provenance::NodeId node;
    hir::Stage result;
    StageRule rule;
    source::Span span;
    StageReason reason{StageReason::None};
    std::optional<std::uint32_t> member;
};

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
    [[nodiscard]] std::span<const StageDecision> decisions() const noexcept {
        return decisions_;
    }
    // Unmigrated computations and structural wrappers have no decision entry.
    [[nodiscard]] const StageDecision*
    decision(provenance::NodeId node) const noexcept;

  private:
    friend class ProvenanceRecorder;
    ProvenanceAnalysis(provenance::FrozenGraph graph,
                       std::vector<ProvenanceSite> sites,
                       std::vector<ProvenanceRoot> roots,
                       std::vector<StageDecision> decisions);
    provenance::FrozenGraph graph_;
    std::vector<ProvenanceSite> sites_;
    std::vector<ProvenanceRoot> roots_;
    std::vector<StageDecision> decisions_;
};

void explain_provenance(const ProvenanceAnalysis& analysis,
                        std::ostream& output);

} // namespace pagos::stage
