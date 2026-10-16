#pragma once

#include "pagos/hir/hir.h"
#include "pagos/source/source_manager.h"
#include "pagos/stage/evidence_builder.h"
#include "pagos/stage/memory_stats.h"
#include "pagos/stage/provenance_graph.h"
#include "pagos/stage/provenance_query.h"

#include <iosfwd>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace pagos::stage {

class ProvenanceRecorder;
class EvidenceUnavailable : public std::runtime_error {
  public:
    explicit EvidenceUnavailable(provenance::EvidenceError error)
        : std::runtime_error("provenance details unavailable"), error_(error) {}
    [[nodiscard]] const provenance::EvidenceError& error() const {
        return error_;
    }

  private:
    provenance::EvidenceError error_;
};

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
    provenance::SummaryId scope;
    bool instantiated{}; // Copied decision in call-site evidence.
};

using ProvenanceSite = provenance::EvidenceSite;

struct ProvenanceRoot {
    std::string name;
    source::Span span;
    hir::Stage stage;
    provenance::NodeId node;
    bool requires_comptime{};
    provenance::SummaryId scope;
};

struct ProvenanceScope {
    provenance::SummaryId id; // Zero is the module scope.
    std::string name;
    std::vector<provenance::NodeId> decisions;
    std::vector<std::size_t> contracts; // Indices into roots().
    struct Specialization {
        std::string definition_name; // Captured from the actual declaration.
        std::vector<cache::StageArgument> arguments;
        std::optional<cache::StableDefinitionId> definition{};
        cache::Digest definition_fingerprint{};
    };
    std::optional<Specialization> specialization{};
};

struct ProvenanceIdentity {
    cache::QueryContext context;
    cache::Digest module_fingerprint;
};

// Published with the HIR module. Owns its graph and source metadata
// independently of the analyzer, AST, type table and HIR expression addresses.
class ProvenanceAnalysis {
  public:
    // Compatibility interface: a spill analysis materializes once on demand.
    // Returned references remain valid for this analysis's lifetime.
    [[nodiscard]] const provenance::FrozenGraph& graph() const;
    [[nodiscard]] std::shared_ptr<const provenance::EvidenceView>
    evidence() const;
    [[nodiscard]] bool available() const;
    [[nodiscard]] bool graph_materialized() const;
    [[nodiscard]] bool spilled() const;
    [[nodiscard]] std::optional<provenance::EvidenceError> failure() const;
    [[nodiscard]] const provenance::EvidenceBuildStats&
    construction_stats() const;
    [[nodiscard]] ProvenanceMemoryStats memory_stats() const;
    // Full streaming verification for publication; never restores a graph.
    [[nodiscard]] bool validate_details() const;
    void mark_unavailable(provenance::EvidenceError error) const;
    [[nodiscard]] const ProvenanceSite& site(provenance::SiteId id) const;
    [[nodiscard]] std::span<const ProvenanceRoot> roots() const noexcept {
        return roots_;
    }
    [[nodiscard]] std::span<const StageDecision> decisions() const noexcept {
        return decisions_;
    }
    [[nodiscard]] std::span<const ProvenanceScope> scopes() const noexcept {
        return scopes_;
    }
    [[nodiscard]] const ProvenanceScope& scope(provenance::SummaryId id) const;
    [[nodiscard]] std::span<const ProvenanceSite> sites() const noexcept {
        return sites_;
    }
    [[nodiscard]] const std::optional<ProvenanceIdentity>& identity() const {
        return identity_;
    }
    // Unmigrated computations and structural wrappers have no decision entry.
    [[nodiscard]] const StageDecision*
    decision(provenance::NodeId node) const noexcept;

  private:
    friend class ProvenanceRecorder;
    friend class ProvenanceSnapshot;
    ProvenanceAnalysis(provenance::FrozenGraph graph,
                       std::vector<ProvenanceSite> sites,
                       std::vector<ProvenanceRoot> roots,
                       std::vector<StageDecision> decisions,
                       std::vector<ProvenanceScope> scopes,
                       std::optional<ProvenanceIdentity> identity = {});
    ProvenanceAnalysis(std::shared_ptr<const provenance::EvidenceView> view,
                       std::vector<ProvenanceSite> sites,
                       std::vector<ProvenanceRoot> roots,
                       std::vector<StageDecision> decisions,
                       std::vector<ProvenanceScope> scopes,
                       std::optional<ProvenanceIdentity> identity,
                       provenance::EvidenceBuildStats construction,
                       std::optional<provenance::EvidenceError> failure);
    ProvenanceMemoryStats recording_memory_;
    struct StorageState;
    std::shared_ptr<StorageState> storage_;
    std::vector<ProvenanceSite> sites_;
    std::vector<ProvenanceRoot> roots_;
    std::vector<StageDecision> decisions_;
    std::vector<ProvenanceScope> scopes_;
    std::optional<ProvenanceIdentity> identity_;
};

// Owns source text and all site/decision metadata while a display query is
// paused, independently of analyzer, HIR module and caller's SourceFile.
class ProvenanceQueryTask {
  public:
    ProvenanceQueryTask(
        std::shared_ptr<const ProvenanceAnalysis> analysis,
        source::SourceFile source, provenance::NodeId root,
        provenance::QueryKind kind = provenance::QueryKind::Value,
        bool witness = false);
    [[nodiscard]] provenance::QueryTask& query() noexcept { return query_; }
    [[nodiscard]] const ProvenanceAnalysis& analysis() const noexcept {
        return *analysis_;
    }
    [[nodiscard]] const source::SourceFile& source() const noexcept {
        return source_;
    }

  private:
    std::shared_ptr<const ProvenanceAnalysis> analysis_;
    source::SourceFile source_;
    provenance::QueryTask query_;
};
struct ProvenanceQueryOptions {
    enum class Mode { Legacy, Resumable, Compressed, Topology };
    Mode mode{Mode::Legacy};
    std::size_t slice{4096};
    bool telemetry{};
};
void explain_provenance_experimental(const ProvenanceAnalysis& analysis,
                                     std::ostream& output,
                                     std::ostream& telemetry,
                                     ProvenanceQueryOptions options);

void explain_provenance(const ProvenanceAnalysis& analysis,
                        std::ostream& output);

} // namespace pagos::stage
