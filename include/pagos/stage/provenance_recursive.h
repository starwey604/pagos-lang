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
// Container payload/capacities, excluding tree links, allocator metadata,
// deque baseline allocation, evidence/cache leases and detached result copies.
struct RecursiveStateMemory {
    std::size_t equations{}, facts{}, relations{}, indexes{}, scc{}, worklist{};
    bool offloaded{};
};
struct RecursiveStorageStats {
    std::size_t segment_reads{}, segment_writes{}, bytes_read{},
        bytes_submitted{}, cas_bytes_added{}, reused_objects{},
        peak_codec_buffers{}, logical_bytes{}, directory_bytes{},
        validation_payload_bytes{};
};

// C4d1 experiment. Owns the evidence; never runs Stage or expands call frames.
// Discovery, iterative SCC construction and monotone transfer are sliced by
// primitive steps (one edge/fact/queue operation), not elapsed time or RSS.
inline constexpr std::uint32_t recursive_engine_version = 1;
// C4d3b supports explicit segmented TaskStore saves and paused unloading.
// Active state/restore are resident. Completed state is the immutable result
// receipt (sequence 1); stdout delivery alone never acknowledges it.
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
    [[nodiscard]] std::expected<cache::TaskHead, CheckpointError>
    save_segmented(const cache::TaskStore& store,
                   std::uint64_t expected_generation,
                   std::size_t segment_bytes = 16UZ * 1024,
                   cache::Limits limits = {}) const;
    [[nodiscard]] static std::expected<RecursiveSummaryTask, CheckpointError>
    restore(QueryCheckpointBinding binding, const cache::TaskStore& store,
            const cache::TaskHead& head, SummaryId summary, QueryKind channel,
            cache::Limits limits = {});
    [[nodiscard]] std::expected<cache::TaskHead, CheckpointError>
    offload(const cache::TaskStore& store, std::uint64_t expected_generation,
            std::size_t segment_bytes = 16UZ * 1024, cache::Limits limits = {});
    [[nodiscard]] RecursiveStateMemory state_memory() const;
    [[nodiscard]] const RecursiveStorageStats& storage_stats() const;
    [[nodiscard]] const std::optional<CheckpointError>& storage_failure() const;
    ~RecursiveSummaryTask();
    RecursiveSummaryTask(RecursiveSummaryTask&&) noexcept;
    RecursiveSummaryTask& operator=(RecursiveSummaryTask&&) noexcept;
    RecursiveSummaryTask(const RecursiveSummaryTask&) = delete;
    RecursiveSummaryTask& operator=(const RecursiveSummaryTask&) = delete;
    [[nodiscard]] QueryProgress
    advance(std::size_t steps = 4096, const std::stop_token& cancellation = {});
    void cancel();
    void resume();
    [[nodiscard]] std::uint64_t pending_sequence() const;
    [[nodiscard]] std::uint64_t acknowledged_sequence() const;
    // Only sequence 1 of a completed task is valid; repeated acknowledgement
    // is idempotent. Persist with save_segmented/offload before reporting it.
    [[nodiscard]] bool acknowledge_result(std::uint64_t sequence);
    [[nodiscard]] QueryProgress progress() const;
    [[nodiscard]] const RecursiveStats& stats() const;
    [[nodiscard]] const std::optional<EvidenceError>& failure() const;
    // Detached copy; early results are explicitly Unresolved. Parameter atoms
    // belonging to the selected summary remain symbolic, not unbound inputs.
    // result/links/components/checkpoint explicitly restore unloaded state;
    // result/links/components throw runtime_error if storage is unavailable.
    // References/spans remain valid until the next mutation or restoration.
    [[nodiscard]] RecursiveResult result() const;
    // Retain evidence() to inspect the original rules and call payloads.
    [[nodiscard]] std::span<const RecursiveLink> links() const;
    [[nodiscard]] std::span<const RecursiveComponent> components() const;
    [[nodiscard]] const EvidenceView& evidence() const;

  private:
    struct Impl;
    [[nodiscard]] std::expected<void, CheckpointError> ensure_resident() const;
    mutable std::unique_ptr<Impl> impl_;
};
} // namespace pagos::stage::provenance
