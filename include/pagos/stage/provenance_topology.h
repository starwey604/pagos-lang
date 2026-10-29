#pragma once

#include "pagos/stage/provenance_query.h"

#include <string_view>
#include <unordered_map>

namespace pagos::stage::provenance {

// A formula keeps terminals in the recorded model: concrete source families,
// parameters, calls, summary references and coverage gaps. It never expands a
// callee into its caller. The two ports may share one graph root.
struct DependencyFormula {
    NodeId root;
    QueryKind kind;
    std::vector<NodeId> terms;
    std::string_view fallback;
};
struct TopologyStats {
    std::size_t nodes{};
    std::size_t edges{};
    std::size_t summary_links{};
    std::size_t formulas{};
    std::size_t formula_terms{};
    std::size_t formula_states{};
    std::size_t formula_edges{};
    std::size_t formula_reuses{};
    std::size_t source_families{}; // Unique source node/channel pairs.
    // Final capacity of records, terms and calls; excludes hash buckets.
    // Available after completion; recomputed rather than serialized.
    std::size_t formula_capacity_bytes{};
};
class FormulaIndex : public std::enable_shared_from_this<FormulaIndex> {
  public:
    FormulaIndex(const FormulaIndex&) = delete;
    FormulaIndex& operator=(const FormulaIndex&) = delete;
    [[nodiscard]] const DependencyFormula* formula(NodeId root,
                                                   QueryKind kind) const;
    [[nodiscard]] std::span<const DependencyFormula> formulas() const;
    [[nodiscard]] const FrozenGraph& graph() const;
    [[nodiscard]] const TopologyStats& stats() const;
    [[nodiscard]] QueryTask
    begin_sources(NodeId root, QueryKind kind = QueryKind::Value) const;
    // Calls are outermost first. Each subsequent call must belong to the
    // preceding callee's local dependency formula. No sibling is expanded.
    [[nodiscard]] QueryTask begin_invocation(std::span<const NodeId> calls,
                                             QueryKind kind = QueryKind::Value,
                                             bool witness = false) const;

  private:
    friend class TopologyTask;
    explicit FormulaIndex(std::shared_ptr<const EvidenceView> view);
    std::shared_ptr<const EvidenceView> evidence_;
    TopologyStats stats_;
    std::vector<DependencyFormula> formulas_;
    std::vector<NodeId> calls_;
    std::unordered_map<std::uint64_t, std::size_t> formula_ids_;
};
enum class TopologyRecordKind { Node, Edge, SummaryLink };
struct TopologyRecord {
    TopologyRecordKind kind;
    NodeId node;
    Edge edge;
    SummaryId summary;
    QueryKind port{QueryKind::Value};
    bool operator==(const TopologyRecord&) const = default;
};
struct TopologyPage {
    std::uint64_t sequence{};
    std::vector<TopologyRecord> records;
    QueryProgress progress{QueryProgress::Running};
    std::optional<EvidenceError> failure{};
    std::optional<cache::Digest> task{};
};
// Slice-bounded physical topology over either evidence backend. This does not
// prepare formulas or enumerate concrete invocation instances.
class EvidenceTopologyTask {
  public:
    EvidenceTopologyTask(std::shared_ptr<const EvidenceView> view, NodeId root);
    ~EvidenceTopologyTask();
    EvidenceTopologyTask(EvidenceTopologyTask&&) noexcept;
    EvidenceTopologyTask& operator=(EvidenceTopologyTask&&) noexcept;
    [[nodiscard]] TopologyPage
    advance(QuerySlice slice = {}, const std::stop_token& cancellation = {});
    void cancel();
    void resume();
    [[nodiscard]] QueryProgress progress() const;
    [[nodiscard]] const TopologyStats& stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Container capacities and hash value/bucket storage; excludes node links,
// allocator metadata, graph leases and caller-owned copies of pages/indexes.
struct TopologyStateMemory {
    std::size_t queue{}, visited{}, formula_records{}, formula_terms{},
        formula_index{}, calls{}, local_queue{}, local_seen{}, families{},
        delivery{};
    // Separately retained published index, observed through a weak reference.
    std::size_t retained_index{};
    bool offloaded{};
};
struct TopologyStorageStats {
    std::size_t segment_reads{}, segment_writes{}, bytes_read{},
        bytes_submitted{}, cas_bytes_added{}, reused_objects{},
        peak_codec_buffers{}, logical_bytes{};
};
inline constexpr std::uint32_t topology_engine_version = 1;
// Physical topology, followed by slice-bounded formula preparation. This is
// structural completeness; it does not assert all concrete sources were read.
class TopologyTask {
  public:
    // Both backends prepare the same finite symbolic formulas. graph() is
    // available only for a memory-backed task/index.
    TopologyTask(std::shared_ptr<const EvidenceView> view, NodeId root);
    // Explicit checkpoint opt-in, with stop-and-wait page acknowledgement.
    TopologyTask(QueryCheckpointBinding binding, cache::Digest task,
                 NodeId root);
    [[nodiscard]] bool acknowledge(std::uint64_t sequence);
    [[nodiscard]] std::expected<cache::Bytes, CheckpointError>
    checkpoint(cache::Limits limits = {}) const;
    [[nodiscard]] static std::expected<TopologyTask, CheckpointError>
    restore(QueryCheckpointBinding binding, cache::Digest task, NodeId root,
            std::span<const std::uint8_t> bytes, cache::Limits limits = {});
    [[nodiscard]] std::expected<cache::TaskHead, CheckpointError>
    save(const cache::TaskStore& store, std::uint64_t expected_generation,
         cache::Limits limits = {}) const;
    [[nodiscard]] static std::expected<TopologyTask, CheckpointError>
    restore(QueryCheckpointBinding binding, const cache::TaskStore& store,
            const cache::TaskHead& head, NodeId root,
            cache::Limits limits = {});
    // Fixed-record segments, including a flat terms table; no full byte
    // snapshot.
    [[nodiscard]] std::expected<cache::TaskHead, CheckpointError>
    save_segmented(const cache::TaskStore& store,
                   std::uint64_t expected_generation,
                   std::size_t segment_bytes = 64UZ * 1024,
                   cache::Limits limits = {}) const;
    // Publish first, then release owned task/index tables and the active lease.
    [[nodiscard]] std::expected<cache::TaskHead, CheckpointError>
    offload(const cache::TaskStore& store, std::uint64_t expected_generation,
            std::size_t segment_bytes = 64UZ * 1024, cache::Limits limits = {});
    [[nodiscard]] TopologyStateMemory state_memory() const;
    // Counters for the last segmented save/restore, not persistent logical
    // work.
    [[nodiscard]] const TopologyStorageStats& storage_stats() const;
    [[nodiscard]] const std::optional<CheckpointError>& storage_failure() const;
    ~TopologyTask();
    TopologyTask(TopologyTask&&) noexcept;
    TopologyTask& operator=(TopologyTask&&) noexcept;
    TopologyTask(const TopologyTask&) = delete;
    TopologyTask& operator=(const TopologyTask&) = delete;
    [[nodiscard]] TopologyPage
    advance(QuerySlice slice = {}, const std::stop_token& cancellation = {});
    void cancel();
    void resume();
    [[nodiscard]] QueryProgress progress() const;
    [[nodiscard]] const TopologyStats& stats() const;
    [[nodiscard]] const FrozenGraph& graph() const;
    // Available only after completion; owns the graph independently of task.
    [[nodiscard]] std::shared_ptr<const FormulaIndex> index() const;

  private:
    friend class FrozenGraph;
    struct Impl;
    TopologyTask(FrozenGraph graph, NodeId root);
    [[nodiscard]] std::expected<void, CheckpointError> ensure_resident() const;
    [[nodiscard]] static std::expected<TopologyTask, CheckpointError>
    restore_segmented(QueryCheckpointBinding binding,
                      const cache::TaskStore& store,
                      const cache::TaskHead& head, NodeId root,
                      cache::Limits limits, cache::Bytes bytes);
    std::unique_ptr<Impl> impl_;
};
} // namespace pagos::stage::provenance
