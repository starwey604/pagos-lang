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
    std::size_t source_families{};        // Unique source node/channel pairs.
    std::size_t formula_capacity_bytes{}; // Records, terms, calls; excludes
                                          // hash buckets.
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
    explicit FormulaIndex(FrozenGraph graph);
    FrozenGraph graph_;
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
};
struct TopologyPage {
    std::uint64_t sequence{};
    std::vector<TopologyRecord> records;
    QueryProgress progress{QueryProgress::Running};
    std::optional<EvidenceError> failure{};
};
// Slice-bounded physical topology over either evidence backend. This does not
// prepare formulas or enumerate concrete invocation instances.
class EvidenceTopologyTask {
  public:
    EvidenceTopologyTask(std::shared_ptr<const EvidenceView> view, NodeId root);
    ~EvidenceTopologyTask();
    EvidenceTopologyTask(EvidenceTopologyTask&&) noexcept;
    EvidenceTopologyTask& operator=(EvidenceTopologyTask&&) noexcept;
    [[nodiscard]] TopologyPage advance(QuerySlice slice = {},
                                       std::stop_token cancellation = {});
    void cancel();
    void resume();
    [[nodiscard]] QueryProgress progress() const;
    [[nodiscard]] const TopologyStats& stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Physical topology, followed by slice-bounded formula preparation. This is
// structural completeness; it does not assert all concrete sources were read.
class TopologyTask {
  public:
    ~TopologyTask();
    TopologyTask(TopologyTask&&) noexcept;
    TopologyTask& operator=(TopologyTask&&) noexcept;
    TopologyTask(const TopologyTask&) = delete;
    TopologyTask& operator=(const TopologyTask&) = delete;
    [[nodiscard]] TopologyPage advance(QuerySlice slice = {},
                                       std::stop_token cancellation = {});
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
    std::unique_ptr<Impl> impl_;
};
} // namespace pagos::stage::provenance
