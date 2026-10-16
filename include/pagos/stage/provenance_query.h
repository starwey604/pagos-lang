#pragma once

#include "pagos/stage/evidence_view.h"

#include <limits>
#include <stop_token>

namespace pagos::stage {
class FunctionEvidenceView;
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
    OutOfMemory
};
struct CheckpointError {
    CheckpointErrorCode code;
    std::optional<EvidenceError> evidence;
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
// until the next advance.
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
    // Pure encoding: no implicit disk writes or cursor changes. The caller may
    // atomically publish these bytes as ObjectKind::QueryCheckpoint and retain
    // its ObjectRef independently of the semantic query index.
    [[nodiscard]] std::expected<cache::Bytes, CheckpointError>
    checkpoint(cache::Limits limits = {}) const;
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
    // Sorted, detached compatibility view; obtaining it does not advance work.
    [[nodiscard]] QueryResult result() const;

  private:
    friend class FrozenGraph;
    friend class FormulaIndex;
    struct Impl;
    QueryTask(FrozenGraph graph, NodeId root, QueryKind kind, bool witness);
    void formulas(std::shared_ptr<const FormulaIndex> index);
    void invocation_path(std::span<const NodeId> calls);
    std::unique_ptr<Impl> impl_;
};

} // namespace pagos::stage::provenance
