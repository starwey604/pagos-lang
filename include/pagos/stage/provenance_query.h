#pragma once

#include "pagos/stage/evidence_view.h"

#include <limits>
#include <stop_token>

namespace pagos::stage::provenance {

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
    // Zero means no records were delivered. Otherwise strictly increasing.
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
    ~QueryTask();
    QueryTask(QueryTask&&) noexcept;
    QueryTask& operator=(QueryTask&&) noexcept;
    QueryTask(const QueryTask&) = delete;
    QueryTask& operator=(const QueryTask&) = delete;

    [[nodiscard]] QueryPage advance(QuerySlice slice = {},
                                    std::stop_token cancellation = {});
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
