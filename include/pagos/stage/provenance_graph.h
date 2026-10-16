#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pagos::stage::provenance {

// IDs belong to one graph. Zero is invalid; they are not cross-revision keys.
struct NodeId {
    std::uint32_t value{};
    bool operator==(const NodeId&) const = default;
};
struct SummaryId {
    std::uint32_t value{};
    bool operator==(const SummaryId&) const = default;
};
using SiteId = std::uint32_t;

enum class Rule : std::uint8_t {
    Literal,
    Origin,
    RuntimeBoundary,
    Use,
    Strict,
    Select,
    RequireComptime,
    Aggregate,
    Projection,
    Parameter,
    Call,
    SummaryRef,
    Effect,
    Sequence,
    Incomplete, // An aborted analysis or deliberately unavailable summary.
};
enum class Relation : std::uint8_t {
    Data,
    Control,
    Contract,
    Effect,
    Initializer,
};
struct Edge {
    NodeId target;
    Relation relation{Relation::Data};
    bool operator==(const Edge&) const = default;
};
struct Node {
    Rule rule{Rule::Literal};
    SiteId site{};
    std::uint32_t edge_begin{};
    std::uint32_t edge_count{};
    std::uint32_t payload{}; // Parameter slot or referenced summary ID.
    SummaryId owner;         // Parameter scope.
};

enum class Storage { Append, SelectiveIntern };
enum class Sharing { Unique, Eligible };
enum class QueryKind { Value, Effects };

struct StoreStats {
    std::size_t nodes{};
    std::size_t edges{};
    std::size_t summaries{};
    std::size_t intern_hits{};
    std::size_t node_capacity_bytes{};
    std::size_t edge_capacity_bytes{};
    std::size_t summary_capacity_bytes{};
    std::size_t intern_capacity_bytes{};
};

struct QueryState {
    NodeId node;
    std::uint32_t context{};
    bool operator==(const QueryState&) const = default;
};
struct Source {
    NodeId node;
    SiteId site{};
    std::uint32_t context{};
    bool operator==(const Source&) const = default;
};
struct Invocation {
    NodeId call;
    std::uint32_t parent{};
    SummaryId owner;
    bool operator==(const Invocation&) const = default;
};
struct QueryResult {
    std::vector<Source> sources;
    // Root-to-source BFS witness. All-source queries do not enumerate paths.
    std::vector<QueryState> witness;
    // Context IDs in sources/witness index this owned table; slot zero is the
    // caller's root context. Retain it so a tool can resolve call-site chains.
    std::vector<Invocation> invocations;
    std::size_t visited_states{};
    std::size_t visited_edges{};
    std::size_t contexts{};
    bool complete{true};
    bool recursive_call_cut{};
};

struct GraphData;
class QueryTask;
class TopologyTask;
class FormulaIndex;

struct SummaryDescription {
    SummaryId id;
    NodeId value;
    NodeId effects;
};

// Cheap copies share immutable storage. Publishing a graph still requires the
// caller's normal thread synchronization and lifetime management.
class FrozenGraph {
  public:
    [[nodiscard]] const Node& node(NodeId id) const;
    [[nodiscard]] std::span<const Edge> edges(NodeId id) const;
    [[nodiscard]] StoreStats stats() const;
    [[nodiscard]] std::span<const SummaryDescription> summaries() const;
    [[nodiscard]] const SummaryDescription& summary(SummaryId id) const;
    [[nodiscard]] QueryTask begin_query(NodeId root,
                                        QueryKind kind = QueryKind::Value,
                                        bool shortest_witness = false) const;
    [[nodiscard]] TopologyTask begin_topology(NodeId root) const;
    [[nodiscard]] QueryResult query(NodeId root,
                                    QueryKind kind = QueryKind::Value,
                                    bool shortest_witness = false,
                                    std::size_t state_limit = 1'000'000) const;

  private:
    friend class Builder;
    explicit FrozenGraph(std::shared_ptr<const GraphData> data);
    std::shared_ptr<const GraphData> data_;
};

// One task owns a Builder. Construction is not synchronized. Freeze validates
// all summary publications and transfers storage, consuming the Builder.
class Builder {
  public:
    explicit Builder(Storage storage = Storage::Append);
    ~Builder();
    Builder(Builder&&) noexcept;
    Builder& operator=(Builder&&) noexcept;
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;

    NodeId add(Rule rule, SiteId site, std::span<const Edge> edges = {},
               std::uint32_t payload = 0, SummaryId owner = {},
               Sharing sharing = Sharing::Unique);
    SummaryId reserve_summary();
    // Publication is once-only; SummaryRef permits finite cyclic summaries.
    void define_summary(SummaryId summary, NodeId root);
    NodeId parameter(SummaryId owner, std::uint32_t slot, SiteId site);
    NodeId call(SummaryId summary, std::span<const NodeId> arguments,
                SiteId site);
    NodeId summary_ref(SummaryId summary, SiteId site);
    NodeId project(NodeId aggregate, std::uint32_t member, SiteId site);

    [[nodiscard]] const Node& node(NodeId id) const;
    [[nodiscard]] std::span<const Edge> edges(NodeId id) const;
    [[nodiscard]] StoreStats stats() const;
    [[nodiscard]] FrozenGraph freeze() &&;

  private:
    struct Impl;
    [[nodiscard]] Impl& state();
    [[nodiscard]] const Impl& state() const;
    std::unique_ptr<Impl> impl_;
};

} // namespace pagos::stage::provenance
