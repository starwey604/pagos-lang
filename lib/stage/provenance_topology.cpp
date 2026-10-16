#include "pagos/stage/provenance_topology.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace pagos::stage::provenance {
namespace {
std::uint64_t formula_key(NodeId root, QueryKind kind) {
    return (static_cast<std::uint64_t>(root.value) << 1U) |
           (kind == QueryKind::Effects ? 1U : 0U);
}
bool followed(QueryKind kind, Relation relation) {
    return kind == QueryKind::Effects ||
           (relation != Relation::Effect && relation != Relation::Initializer);
}
bool terminal(Rule rule, QueryKind kind) {
    return rule == Rule::Parameter || rule == Rule::Call ||
           rule == Rule::SummaryRef || rule == Rule::Incomplete ||
           (kind == QueryKind::Value
                ? rule == Rule::Origin || rule == Rule::RuntimeBoundary
                : rule == Rule::Effect);
}
} // namespace
FormulaIndex::FormulaIndex(FrozenGraph graph) : graph_(std::move(graph)) {}
const FrozenGraph& FormulaIndex::graph() const { return graph_; }
const TopologyStats& FormulaIndex::stats() const { return stats_; }
std::span<const DependencyFormula> FormulaIndex::formulas() const {
    return formulas_;
}
const DependencyFormula* FormulaIndex::formula(NodeId root,
                                               QueryKind kind) const {
    const auto found = formula_ids_.find(formula_key(root, kind));
    return found == formula_ids_.end() ? nullptr : &formulas_[found->second];
}
QueryTask FormulaIndex::begin_sources(NodeId root, QueryKind kind) const {
    if (!formula(root, kind))
        throw std::out_of_range("root outside formula inventory");
    auto task = graph_.begin_query(root, kind);
    task.formulas(shared_from_this());
    return task;
}
QueryTask FormulaIndex::begin_invocation(std::span<const NodeId> calls,
                                         QueryKind kind, bool witness) const {
    if (calls.empty())
        throw std::invalid_argument("empty invocation path");
    NodeId root;
    std::unordered_set<std::uint32_t> owners;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const auto call = calls[i];
        if (std::ranges::find(calls_, call) == calls_.end())
            throw std::invalid_argument("call outside topology inventory");
        if (i) {
            // SummaryRef links preserve the current context. Walk these links
            // finitely while validating the next selected invocation.
            std::vector<NodeId> pending{root};
            std::unordered_set<std::uint32_t> seen;
            bool found = false;
            while (!pending.empty()) {
                const auto current = pending.back();
                pending.pop_back();
                if (!seen.insert(current.value).second)
                    continue;
                const auto* local = formula(current, kind);
                if (!local)
                    throw std::invalid_argument(
                        "unavailable invocation formula");
                for (const auto term : local->terms) {
                    if (term == call)
                        found = true;
                    const auto& node = graph_.node(term);
                    if (node.rule == Rule::SummaryRef && node.payload &&
                        node.payload <= graph_.summaries().size()) {
                        const auto& summary = graph_.summary({node.payload});
                        pending.push_back(kind == QueryKind::Value
                                              ? summary.value
                                              : summary.effects);
                    }
                }
            }
            if (!found)
                throw std::invalid_argument("disconnected invocation path");
        }
        const auto& node = graph_.node(call);
        if (!owners.insert(node.payload).second)
            throw std::invalid_argument(
                "recursive invocation path requires recursive solver");
        const auto& summary = graph_.summary({node.payload});
        root = kind == QueryKind::Value ? summary.value : summary.effects;
    }
    auto task = graph_.begin_query(calls.back(), kind, witness);
    task.invocation_path(calls.first(calls.size() - 1));
    if (!witness)
        task.formulas(shared_from_this());
    return task;
}
struct TopologyTask::Impl {
    Impl(std::shared_ptr<FormulaIndex> inventory, NodeId root)
        : index(std::move(inventory)) {
        (void)index->graph_.node(root);
        enqueue(root);
        add_formula(root, QueryKind::Value);
        add_formula(root, QueryKind::Effects);
    }
    std::shared_ptr<FormulaIndex> index;
    QueryProgress progress{QueryProgress::Running};
    std::uint64_t sequence{};
    std::vector<NodeId> queue;
    std::unordered_set<std::uint32_t> visited;
    std::size_t cursor{}, edge_cursor{}, link_cursor{};
    bool active{}, preparing{};
    std::size_t formula_cursor{}, local_cursor{}, local_edge{};
    std::vector<NodeId> local_queue;
    std::unordered_set<std::uint32_t> local_seen;
    bool local_active{};
    std::unordered_set<std::uint64_t> families;
    void enqueue(NodeId node) {
        if (visited.insert(node.value).second)
            queue.push_back(node);
    }
    void add_formula(NodeId root, QueryKind kind) {
        if (index->formula(root, kind)) {
            ++index->stats_.formula_reuses;
            return;
        }
        index->formula_ids_.emplace(formula_key(root, kind),
                                    index->formulas_.size());
        index->formulas_.push_back({root, kind, {}, {}});
    }
    bool step(QuerySlice& slice, std::vector<TopologyRecord>& output) {
        auto& stats = index->stats_;
        const auto& graph = index->graph_;
        if (!preparing) {
            if (!active) {
                if (cursor == queue.size()) {
                    preparing = true;
                    return true;
                }
                if (!slice.states || !slice.output)
                    return false;
                --slice.states;
                --slice.output;
                const auto node = queue[cursor];
                output.push_back(
                    {TopologyRecordKind::Node, node, {}, {}, QueryKind::Value});
                ++stats.nodes;
                if (graph.node(node).rule == Rule::Call)
                    index->calls_.push_back(node);
                active = true;
                edge_cursor = 0;
                link_cursor = 0;
                return true;
            }
            const auto node = queue[cursor];
            const auto edges = graph.edges(node);
            if (edge_cursor < edges.size()) {
                if (!slice.edges || !slice.output)
                    return false;
                --slice.edges;
                --slice.output;
                const auto edge = edges[edge_cursor++];
                output.push_back({TopologyRecordKind::Edge,
                                  node,
                                  edge,
                                  {},
                                  QueryKind::Value});
                enqueue(edge.target);
                ++stats.edges;
                if (graph.node(node).rule == Rule::Call) {
                    add_formula(edge.target, QueryKind::Value);
                    add_formula(edge.target, QueryKind::Effects);
                }
                return true;
            }
            const auto& record = graph.node(node);
            if ((record.rule == Rule::Call ||
                 record.rule == Rule::SummaryRef) &&
                record.payload && record.payload <= graph.summaries().size() &&
                link_cursor < 2) {
                if (!slice.bindings || !slice.output)
                    return false;
                --slice.bindings;
                --slice.output;
                const auto kind =
                    link_cursor++ == 0 ? QueryKind::Value : QueryKind::Effects;
                const auto& summary = graph.summary({record.payload});
                const auto target =
                    kind == QueryKind::Value ? summary.value : summary.effects;
                output.push_back({TopologyRecordKind::SummaryLink,
                                  node,
                                  {target},
                                  summary.id,
                                  kind});
                enqueue(target);
                add_formula(target, kind);
                ++stats.summary_links;
                return true;
            }
            active = false;
            ++cursor;
            return true;
        }
        if (formula_cursor == index->formulas_.size()) {
            stats.formulas = index->formulas_.size();
            stats.source_families = families.size();
            stats.formula_capacity_bytes +=
                index->formulas_.capacity() * sizeof(DependencyFormula) +
                index->calls_.capacity() * sizeof(NodeId);
            progress = QueryProgress::Completed;
            return false;
        }
        auto& formula = index->formulas_[formula_cursor];
        if (local_queue.empty()) {
            local_queue.push_back(formula.root);
            local_seen.insert(formula.root.value);
        }
        if (!local_active) {
            if (local_cursor == local_queue.size()) {
                stats.formula_terms += formula.terms.size();
                stats.formula_capacity_bytes +=
                    formula.terms.capacity() * sizeof(NodeId);
                ++formula_cursor;
                local_queue.clear();
                local_seen.clear();
                local_cursor = 0;
                return true;
            }
            if (!slice.states)
                return false;
            --slice.states;
            ++stats.formula_states;
            const auto id = local_queue[local_cursor];
            const auto& node = graph.node(id);
            if (static_cast<unsigned>(node.rule) >
                static_cast<unsigned>(Rule::Incomplete))
                formula.fallback = "unsupported rule";
            if (terminal(node.rule, formula.kind))
                formula.terms.push_back(id);
            if (formula.kind == QueryKind::Value
                    ? node.rule == Rule::Origin ||
                          node.rule == Rule::RuntimeBoundary
                    : node.rule == Rule::Effect)
                families.insert(formula_key(id, formula.kind));
            local_edge = 0;
            local_active = true;
            // Calls/parameters are shared substitution terms. Effects of call
            // arguments remain represented by the call evaluator itself.
            if (node.rule == Rule::Call || node.rule == Rule::SummaryRef ||
                node.rule == Rule::Parameter)
                local_edge = graph.edges(id).size();
            return true;
        }
        const auto edges = graph.edges(local_queue[local_cursor]);
        if (local_edge < edges.size()) {
            if (!slice.edges)
                return false;
            --slice.edges;
            ++stats.formula_edges;
            const auto edge = edges[local_edge++];
            if (static_cast<unsigned>(edge.relation) >
                static_cast<unsigned>(Relation::Initializer))
                formula.fallback = "unsupported relation";
            if (followed(formula.kind, edge.relation) &&
                local_seen.insert(edge.target.value).second)
                local_queue.push_back(edge.target);
            return true;
        }
        local_active = false;
        ++local_cursor;
        return true;
    }
};
TopologyTask::TopologyTask(FrozenGraph graph, NodeId root)
    : impl_(std::make_unique<Impl>(
          std::shared_ptr<FormulaIndex>(new FormulaIndex(std::move(graph))),
          root)) {}
TopologyTask FrozenGraph::begin_topology(NodeId root) const {
    return TopologyTask(*this, root);
}
TopologyTask::~TopologyTask() = default;
TopologyTask::TopologyTask(TopologyTask&&) noexcept = default;
TopologyTask& TopologyTask::operator=(TopologyTask&&) noexcept = default;
QueryProgress TopologyTask::progress() const { return impl_->progress; }
const FrozenGraph& TopologyTask::graph() const { return impl_->index->graph_; }
const TopologyStats& TopologyTask::stats() const {
    return impl_->index->stats_;
}
std::shared_ptr<const FormulaIndex> TopologyTask::index() const {
    return progress() == QueryProgress::Completed ? impl_->index : nullptr;
}
void TopologyTask::cancel() {
    if (progress() == QueryProgress::Running ||
        progress() == QueryProgress::Paused)
        impl_->progress = QueryProgress::Cancelled;
}
void TopologyTask::resume() {
    if (progress() == QueryProgress::Paused ||
        progress() == QueryProgress::Cancelled)
        impl_->progress = QueryProgress::Running;
}
TopologyPage TopologyTask::advance(QuerySlice slice,
                                   std::stop_token cancellation) {
    if (progress() == QueryProgress::Paused)
        resume();
    TopologyPage page;
    if (progress() == QueryProgress::Running) {
        try {
            while (!cancellation.stop_requested() &&
                   impl_->step(slice, page.records)) {
            }
            if (cancellation.stop_requested())
                cancel();
            else if (progress() == QueryProgress::Running)
                impl_->progress = QueryProgress::Paused;
        } catch (const std::bad_alloc&) {
            impl_->progress = QueryProgress::Failed;
        } catch (const std::length_error&) {
            impl_->progress = QueryProgress::Failed;
        }
    }
    if (!page.records.empty())
        page.sequence = ++impl_->sequence;
    page.progress = progress();
    return page;
}
} // namespace pagos::stage::provenance
