#pragma once

#include "pagos/cache/task_store.h"
#include "pagos/stage/evidence_view.h"

#include <limits>
#include <stop_token>

namespace pagos::stage {
class FunctionEvidenceView;
}
namespace pagos::cache::wire {
class Reader;
}
namespace pagos::stage::provenance {

inline constexpr std::uint32_t query_checkpoint_version = 1;
// Bump when traversal semantics or the function view's virtual ID layout
// changes.
inline constexpr std::uint32_t query_engine_version = 1;

// Only a validated function snapshot adapter can create this binding. Its IDs
// are portable solely inside the exact root closure, position map and source
// revision. Retaining this handle also retains the view; no pointers go on
// disk.
class QueryCheckpointBinding {
  private:
    friend class pagos::stage::FunctionEvidenceView;
    friend class QueryTask;
    friend class TopologyTask;
    friend class RecursiveSummaryTask;
    QueryCheckpointBinding(std::shared_ptr<const EvidenceView> view,
                           cache::Digest closure, cache::Digest positions,
                           cache::Digest revision)
        : view_(std::move(view)), closure_(closure), positions_(positions),
          revision_(revision) {}
    std::shared_ptr<const EvidenceView> view_;
    cache::Digest closure_, positions_, revision_;
};
enum class CheckpointErrorCode {
    InvalidState,
    IncompatibleVersion,
    BindingMismatch,
    Corrupt,
    LimitExceeded,
    UnavailableEvidence,
    OutOfMemory,
    StorageFailure
};
struct CheckpointError {
    CheckpointErrorCode code;
    std::optional<EvidenceError> evidence;
    std::optional<cache::StoreError> storage{};
};
struct QueryStateMemory {
    std::size_t queue{}, visited{}, contexts{}, context_index{}, sources{},
        witness{};
    bool offloaded{};
    std::size_t cached{}, directory{}, compatibility{}, peak_buffers{};
    bool paged{};
};
struct QueryPagingConfig {
    std::size_t segment_bytes{16 * 1024};
    // Owned append buffers + decoded read cache, excluding directory, CAS
    // codec scratch, graph leases and explicit detached compatibility results.
    std::size_t resident_bytes{256 * 1024};
    std::size_t cache_bytes{64 * 1024};
};
struct QueryPagingStats {
    std::size_t reads{}, hits{}, writes{}, bytes_read{}, bytes_written{};
};

enum class QueryProgress { Running, Paused, Cancelled, Completed, Failed };
enum class EvidencePrecision {
    Unresolved,
    ExactForRecordedModel,
    Conservative
};
enum class DetailAvailability { Resident, Unavailable };
enum class CoverageIssue : std::uint32_t {
    IncompleteNode = 1,
    UnboundParameter = 2,
    RecursiveCall = 4,
    LegacyStateLimit = 8,
    MissingSummary = 16,
    UnavailableEvidence = 32
};
struct EvidenceCoverage {
    EvidencePrecision precision{EvidencePrecision::Unresolved};
    std::uint32_t issues{};
    bool all_sources{};
    bool witness_found{};
};

struct QuerySlice {
    std::size_t states{4096};
    std::size_t edges{16384};
    std::size_t bindings{4096};
    std::size_t output{256};
    [[nodiscard]] static QuerySlice unlimited() {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        return {maximum, maximum, maximum, maximum};
    }
};
struct QueryStats {
    std::size_t admitted_states{};
    std::size_t visited_states{};
    std::size_t scanned_edges{}; // Includes filtered physical edges.
    std::size_t visited_edges{}; // Followed physical/synthetic dependencies.
    std::size_t bindings{};      // Context ancestry steps and frame creation.
    std::size_t witness_steps{};
    std::size_t queue_peak{};
    std::size_t slices{};
    std::size_t formula_uses{};
    std::size_t formula_fallbacks{};
};
struct QueryPage {
    // Present for checkpointable tasks. Consumers deduplicate (task, sequence).
    std::optional<cache::Digest> task;
    // Zero means no records. New pages increase; an unconfirmed page replays
    // with its original sequence when checkpointing is enabled.
    std::uint64_t sequence{};
    std::vector<Source> sources;
    std::vector<QueryState> witness;
    QueryProgress progress{QueryProgress::Running};
    EvidenceCoverage coverage;
    DetailAvailability details{DetailAvailability::Resident};
    std::size_t delivered_sources{};
    std::size_t pending_sources{};
    bool pending_witness{};
};

// Single-threaded, move-only task. stop_token may be requested concurrently.
// Pages contain graph-local/context IDs. Keep the task, or retain its graph
// and owned result, while resolving them; invocations() spans remain valid
// until the next advance or offload. invocations()/result() explicitly restore
// offloaded tables and throw runtime_error if query state cannot be loaded.
class QueryTask {
  public:
    // Does not load cold blocks until advance admits the first state.
    QueryTask(std::shared_ptr<const EvidenceView> evidence, NodeId root,
              QueryKind kind = QueryKind::Value, bool witness = false);
    // Explicit opt-in, stop-and-wait delivery: advance repeats the unconfirmed
    // page without doing work until acknowledge succeeds. task_id is supplied
    // by the caller and must be unique for a new logical query.
    QueryTask(QueryCheckpointBinding binding, cache::Digest task_id,
              NodeId root, QueryKind kind = QueryKind::Value,
              bool witness = false);
    [[nodiscard]] static std::expected<QueryTask, CheckpointError>
    begin_paged(QueryCheckpointBinding binding, const cache::TaskStore& store,
                NodeId root, QueryKind kind = QueryKind::Value,
                bool witness = false, QueryPagingConfig config = {},
                cache::Limits limits = {});
    // Stream validation with bounded buffers; does not rebuild full tables or
    // hash indexes. Version 2 paged snapshots only (v1 keeps its old restore).
    [[nodiscard]] static std::expected<QueryTask, CheckpointError>
    restore_paged(QueryCheckpointBinding binding, const cache::TaskStore& store,
                  const cache::TaskHead& head, NodeId root, QueryKind kind,
                  bool witness, QueryPagingConfig config = {},
                  cache::Limits limits = {});
    [[nodiscard]] const std::optional<CheckpointError>& storage_failure() const;
    // Physical counters for the current loaded backend, reset on restoration.
    [[nodiscard]] QueryPagingStats paging_stats() const;
    void evict_state_cache() const;
    // Same namespace/task only, e.g. explicit read-only -> writable transition
    // or increased disk quota. Does not advance work or publish a new head.
    [[nodiscard]] std::expected<void, CheckpointError>
    resume_with_storage(const cache::TaskStore& store);
    ~QueryTask();
    QueryTask(QueryTask&&) noexcept;
    QueryTask& operator=(QueryTask&&) noexcept;
    QueryTask(const QueryTask&) = delete;
    QueryTask& operator=(const QueryTask&) = delete;

    [[nodiscard]] QueryPage advance(QuerySlice slice = {},
                                    std::stop_token cancellation = {});
    // Idempotently confirm through this page. Confirm only after committing
    // consumer records/receipts. The watermark persists in the next checkpoint.
    [[nodiscard]] bool acknowledge(std::uint64_t sequence);
    // Encoding with no implicit disk writes or cursor changes; offloaded tables
    // are read back first. The caller may
    // atomically publish these bytes as ObjectKind::QueryCheckpoint and retain
    // its ObjectRef independently of the semantic query index.
    [[nodiscard]] std::expected<cache::Bytes, CheckpointError>
    checkpoint(cache::Limits limits = {}) const;
    // Stream fixed-size table segments and publish the task head last. CAS
    // objects from earlier generations remain until explicit task maintenance.
    [[nodiscard]] std::expected<cache::TaskHead, CheckpointError>
    save(const cache::TaskStore& store, std::uint64_t expected_generation,
         std::size_t segment_bytes = 64 * 1024,
         cache::Limits limits = {}) const;
    // Same publication, then actually release vector/hash capacity and active
    // evidence lease. Next advance (or result/invocations) restores the tables.
    [[nodiscard]] std::expected<cache::TaskHead, CheckpointError>
    offload(const cache::TaskStore& store, std::uint64_t expected_generation,
            std::size_t segment_bytes = 64 * 1024, cache::Limits limits = {});
    [[nodiscard]] QueryStateMemory state_memory() const;
    [[nodiscard]] static std::expected<QueryTask, CheckpointError>
    restore(QueryCheckpointBinding binding, const cache::TaskStore& store,
            const cache::TaskHead& head, NodeId root, QueryKind kind,
            bool witness, cache::Limits limits = {});
    // Validate caller's expected task/root/channel/mode as well as snapshot
    // binding. Does not run Stage, traverse edges or charge work again.
    [[nodiscard]] static std::expected<QueryTask, CheckpointError>
    restore(QueryCheckpointBinding binding, cache::Digest task_id, NodeId root,
            QueryKind kind, bool witness, std::span<const std::uint8_t> bytes,
            cache::Limits limits = {});
    // Legacy compatibility ceiling; unlike slices, it drops excess states.
    void legacy_limit(std::size_t limit);
    void suspend();
    void cancel();
    void resume();
    [[nodiscard]] QueryProgress progress() const;
    [[nodiscard]] EvidenceCoverage coverage() const;
    [[nodiscard]] const QueryStats& stats() const;
    [[nodiscard]] const FrozenGraph& graph() const;
    // graph() preserves the memory adapter's old contract; it throws
    // logic_error for disk views. Disk callers resolve IDs using evidence().
    [[nodiscard]] const EvidenceView& evidence() const;
    [[nodiscard]] const std::optional<EvidenceError>& failure() const;
    [[nodiscard]] std::span<const Invocation> invocations() const;
    // Value access for page renderers, without materializing all contexts.
    // Throws out_of_range for an invalid context; runtime_error on IO failure.
    [[nodiscard]] Invocation invocation(std::uint32_t context) const;
    // Sorted, detached compatibility view; obtaining it does not advance work.
    [[nodiscard]] QueryResult result() const;

  private:
    friend class FrozenGraph;
    friend class FormulaIndex;
    struct Impl;
    QueryTask(FrozenGraph graph, NodeId root, QueryKind kind, bool witness);
    void formulas(std::shared_ptr<const FormulaIndex> index);
    void invocation_path(std::span<const NodeId> calls);
    [[nodiscard]] std::expected<void, CheckpointError> ensure_resident() const;
    static QueryTask read_control(QueryCheckpointBinding binding,
                                  cache::Digest task, NodeId root,
                                  QueryKind kind, bool witness,
                                  cache::wire::Reader& reader);
    [[nodiscard]] std::expected<cache::TaskHead, CheckpointError>
    save_paged(const cache::TaskStore& store, std::uint64_t generation,
               cache::Limits limits) const;
    std::unique_ptr<Impl> impl_;
};

} // namespace pagos::stage::provenance
