#pragma once

#include "pagos/stage/provenance_query.h"

#include <compare>

namespace pagos::stage::provenance {

// Finite symbolic dependencies, not concrete invocation instances. Parameter
// atoms name formal slots; source atoms name recorded source/effect families.
enum class RecursiveAtomKind { Parameter, SourceFamily };
struct RecursiveAtom {
    RecursiveAtomKind kind;
    std::uint32_t owner{}; // Parameter's SummaryId; zero for source families.
    std::uint32_t slot{};
    std::uint32_t node{}; // Source family's NodeId; zero for parameters.
    auto operator<=>(const RecursiveAtom&) const = default;
};
enum class RecursiveIssue : std::uint32_t {
    IncompleteNode = 1,
    MissingSummary = 2,
    UnboundParameter = 4,
    ForeignScope = 8,
    UnsupportedRule = 16,
    UnsupportedRelation = 32,
    UnavailableEvidence = 64
};
enum class RecursiveLinkKind { Dependency, Summary, Argument };
struct RecursiveLink {
    NodeId from, to;
    RecursiveLinkKind kind;
    Relation relation{Relation::Data};
    std::uint32_t slot{}; // Actual slot for Argument; not a formal binding.
    bool operator==(const RecursiveLink&) const = default;
};
// SCCs of the finite equation graph, including summary and argument links.
// These are structural cycle witnesses, not terminating execution paths.
struct RecursiveComponent {
    std::vector<NodeId> nodes;
    std::vector<SummaryId> summaries;
    bool cyclic{};
    bool operator==(const RecursiveComponent&) const = default;
};
struct RecursiveStats {
    std::size_t steps{}, slices{}, nodes{}, links{}, components{},
        cyclic_components{}, evaluations{}, facts_added{}, queue_peak{};
};
struct RecursiveResult {
    SummaryId summary;
    QueryKind channel;
    std::vector<RecursiveAtom> atoms;
    EvidencePrecision precision{EvidencePrecision::Unresolved};
    std::uint32_t issues{};
    bool fixed_point{};
    bool instances_merged{};
    // Even an exact formal dependency relation is not a concrete source list.
    bool concrete_sources_complete{false};
    bool operator==(const RecursiveResult&) const = default;
};

// C4d1 experiment. Owns the evidence; never runs Stage or expands call frames.
// Discovery, iterative SCC construction and monotone transfer are sliced by
// primitive steps (one edge/fact/queue operation), not elapsed time or RSS.
inline constexpr std::uint32_t recursive_engine_version = 1;
// C4d2 exposes an explicit summary CLI. C4d3a snapshots are resident, with no
// TaskStore publication, automatic unloading or result-delivery receipts yet.
class RecursiveSummaryTask {
  public:
    RecursiveSummaryTask(std::shared_ptr<const EvidenceView> evidence,
                         SummaryId summary,
                         QueryKind channel = QueryKind::Value);
    RecursiveSummaryTask(QueryCheckpointBinding binding, cache::Digest task,
                         SummaryId summary,
                         QueryKind channel = QueryKind::Value);
    [[nodiscard]] std::expected<cache::Bytes, CheckpointError>
    checkpoint(cache::Limits limits = {}) const;
    [[nodiscard]] static std::expected<RecursiveSummaryTask, CheckpointError>
    restore(QueryCheckpointBinding binding, cache::Digest task,
            SummaryId summary, QueryKind channel,
            std::span<const std::uint8_t> bytes, cache::Limits limits = {});
    ~RecursiveSummaryTask();
    RecursiveSummaryTask(RecursiveSummaryTask&&) noexcept;
    RecursiveSummaryTask& operator=(RecursiveSummaryTask&&) noexcept;
    RecursiveSummaryTask(const RecursiveSummaryTask&) = delete;
    RecursiveSummaryTask& operator=(const RecursiveSummaryTask&) = delete;
    [[nodiscard]] QueryProgress advance(std::size_t steps = 4096,
                                        std::stop_token cancellation = {});
    void cancel();
    void resume();
    [[nodiscard]] QueryProgress progress() const;
    [[nodiscard]] const RecursiveStats& stats() const;
    [[nodiscard]] const std::optional<EvidenceError>& failure() const;
    // Detached copy; early results are explicitly Unresolved. Parameter atoms
    // belonging to the selected summary remain symbolic, not unbound inputs.
    [[nodiscard]] RecursiveResult result() const;
    // Retain evidence() to inspect the original rules and call payloads.
    [[nodiscard]] std::span<const RecursiveLink> links() const;
    [[nodiscard]] std::span<const RecursiveComponent> components() const;
    [[nodiscard]] const EvidenceView& evidence() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pagos::stage::provenance
