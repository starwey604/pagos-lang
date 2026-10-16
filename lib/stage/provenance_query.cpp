#include "pagos/stage/provenance_query.h"
#include "pagos/stage/provenance_topology.h"

#include <llvm/ADT/DenseMap.h>

#include <algorithm>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace pagos::stage::provenance {
namespace {
struct EvidenceReadFailure {
    EvidenceError error;
};
constexpr auto no_predecessor = std::numeric_limits<std::size_t>::max();
std::uint64_t state_key(QueryState state) {
    return (static_cast<std::uint64_t>(state.context) << 32U) |
           state.node.value;
}
std::uint32_t context_id(std::size_t size) {
    if (size >= std::numeric_limits<std::uint32_t>::max() - 1U)
        throw std::length_error("provenance context capacity exhausted");
    return static_cast<std::uint32_t>(size);
}
bool followed(QueryKind kind, Relation relation) {
    return kind == QueryKind::Effects ||
           (relation != Relation::Initializer && relation != Relation::Effect);
}
} // namespace

struct QueryTask::Impl {
    struct Visit {
        QueryState state;
        std::size_t predecessor;
    };
    enum class Phase {
        Parameter,
        ParameterEdge,
        CallSearch,
        CallFrame,
        Summary,
        Edges,
        Formula,
        Done,
        Witness
    };
    Impl(std::shared_ptr<const EvidenceView> snapshot, NodeId entry,
         QueryKind channel, bool witness)
        : evidence(std::move(snapshot)), root(entry), kind(channel),
          shortest(witness) {
        if (!evidence || !root.value || root.value > evidence->node_count())
            throw std::out_of_range("invalid provenance query root");
    }
    NodeLease pin(NodeId id) {
        auto result = evidence->pin(id);
        if (!result)
            throw EvidenceReadFailure{result.error()};
        return std::move(*result);
    }
    std::shared_ptr<const EvidenceView> evidence;
    std::optional<NodeLease> current_lease;
    std::optional<EvidenceError> failure;
    NodeId root;
    QueryKind kind;
    bool shortest;
    bool seeded{};
    bool active{};
    bool recursive{};
    bool all_sources{};
    std::optional<std::size_t> limit;
    QueryProgress progress{QueryProgress::Running};
    QueryStats stats;
    std::uint32_t issues{};
    std::vector<Invocation> contexts{Invocation{}};
    llvm::DenseMap<std::uint64_t, std::uint32_t> context_ids;
    llvm::DenseMap<std::uint64_t, std::size_t> visited;
    std::vector<Visit> queue;
    std::size_t cursor{};
    Phase phase{Phase::Done};
    std::uint32_t frame{};
    NodeId summary_target;
    std::size_t edge_cursor{};
    const DependencyFormula* formula{};
    std::vector<Source> sources;
    std::vector<QueryState> reversed_witness;
    std::size_t witness_cursor{no_predecessor};
    std::size_t delivered_sources{};
    std::size_t delivered_witness{};
    std::uint64_t sequence{};
    std::shared_ptr<const FormulaIndex> formula_index;
    std::vector<NodeId> initial_calls;
    std::size_t initial_cursor{};

    void gap(CoverageIssue issue) {
        issues |= static_cast<std::uint32_t>(issue);
    }
    void enqueue(QueryState state, std::size_t predecessor) {
        const auto key = state_key(state);
        if (visited.contains(key))
            return;
        // Only the old synchronous API uses a permanent ceiling. Resumable
        // tasks pause their edge cursor; they never drop a budgeted transition.
        if (limit && queue.size() >= *limit) {
            gap(CoverageIssue::LegacyStateLimit);
            return;
        }
        const auto position = queue.size();
        queue.push_back({state, predecessor});
        visited[key] = position;
        ++stats.admitted_states;
        stats.queue_peak = std::max(stats.queue_peak, queue.size() - cursor);
    }
    EvidenceCoverage coverage() const {
        return {.precision = !all_sources ? EvidencePrecision::Unresolved
                             : issues
                                 ? EvidencePrecision::Conservative
                                 : EvidencePrecision::ExactForRecordedModel,
                .issues = issues,
                .all_sources = all_sources,
                .witness_found = !reversed_witness.empty()};
    }
    bool bind(QuerySlice& slice) {
        if (!slice.bindings)
            return false;
        --slice.bindings;
        ++stats.bindings;
        return true;
    }
    bool edge(QuerySlice& slice, QueryState target) {
        if (!slice.edges)
            return false;
        --slice.edges;
        ++stats.scanned_edges;
        ++stats.visited_edges;
        enqueue(target, cursor);
        return true;
    }
    bool step(QuerySlice& slice) {
        if (!seeded) {
            if (initial_cursor < initial_calls.size()) {
                if (!bind(slice))
                    return false;
                const auto call = initial_calls[initial_cursor++];
                contexts.push_back({call,
                                    context_id(contexts.size() - 1),
                                    {pin(call).node().payload}});
                return true;
            }
            if (!slice.states)
                return false;
            enqueue({root, context_id(contexts.size() - 1)}, no_predecessor);
            seeded = true;
        }
        if (!active) {
            if (cursor == queue.size()) {
                all_sources = true;
                progress = QueryProgress::Completed;
                return false;
            }
            if (!slice.states)
                return false;
            --slice.states;
            ++stats.visited_states;
            active = true;
            frame = queue[cursor].state.context;
            edge_cursor = 0;
            recursive = false;
            current_lease = pin(queue[cursor].state.node);
            const auto& current = current_lease->node();
            if (current.rule == Rule::Incomplete)
                gap(CoverageIssue::IncompleteNode);
            const bool source = kind == QueryKind::Value
                                    ? current.rule == Rule::Origin ||
                                          current.rule == Rule::RuntimeBoundary
                                    : current.rule == Rule::Effect;
            if (source) {
                sources.push_back(
                    {queue[cursor].state.node, current.site, frame});
                if (shortest) {
                    witness_cursor = cursor;
                    phase = Phase::Witness;
                    return true;
                }
            }
            if (current.rule == Rule::Parameter)
                phase = Phase::Parameter;
            else if (current.rule == Rule::Call ||
                     current.rule == Rule::SummaryRef) {
                if (!current.payload ||
                    current.payload > evidence->summaries().size()) {
                    gap(CoverageIssue::MissingSummary);
                    phase = Phase::Done;
                } else {
                    const auto& summary = evidence->summary({current.payload});
                    summary_target = kind == QueryKind::Value ? summary.value
                                                              : summary.effects;
                    phase = current.rule == Rule::Call ? Phase::CallSearch
                                                       : Phase::Summary;
                }
            } else {
                phase = Phase::Edges;
                formula =
                    formula_index
                        ? formula_index->formula(queue[cursor].state.node, kind)
                        : nullptr;
                if (formula && formula->fallback.empty() && !source &&
                    current.rule != Rule::Incomplete &&
                    std::ranges::find(formula->terms,
                                      queue[cursor].state.node) ==
                        formula->terms.end()) {
                    phase = Phase::Formula;
                    ++stats.formula_uses;
                } else if (formula_index)
                    ++stats.formula_fallbacks;
            }
            return true;
        }
        const auto state = queue[cursor].state;
        const auto& current = current_lease->node();
        switch (phase) {
        case Phase::Parameter:
            if (!frame) {
                gap(CoverageIssue::UnboundParameter);
                phase = Phase::Done;
            } else if (contexts[frame].owner != current.owner) {
                if (!bind(slice))
                    return false;
                frame = contexts[frame].parent;
            } else {
                if (!bind(slice))
                    return false;
                phase = Phase::ParameterEdge;
            }
            return true;
        case Phase::ParameterEdge: {
            const auto& invocation = contexts[frame];
            const auto call_lease = pin(invocation.call);
            const auto arguments = call_lease.edges();
            if (current.payload >= arguments.size()) {
                gap(CoverageIssue::UnboundParameter);
                phase = Phase::Done;
            } else {
                if (!edge(slice, {arguments[current.payload].target,
                                  invocation.parent}))
                    return false;
                phase = Phase::Done;
            }
            return true;
        }
        case Phase::CallSearch:
            if (frame) {
                if (!bind(slice))
                    return false;
                recursive |=
                    contexts[frame].owner == SummaryId{current.payload};
                frame = contexts[frame].parent;
            } else if (recursive) {
                gap(CoverageIssue::RecursiveCall);
                phase = Phase::Edges;
            } else {
                frame = state.context;
                phase = Phase::CallFrame;
            }
            return true;
        case Phase::CallFrame: {
            if (!bind(slice))
                return false;
            const auto key = state_key(state);
            const auto found = context_ids.find(key);
            if (found != context_ids.end())
                frame = found->second;
            else {
                const auto id = context_id(contexts.size());
                contexts.push_back({state.node, frame, {current.payload}});
                context_ids[key] = id;
                frame = id;
            }
            phase = Phase::Summary;
            return true;
        }
        case Phase::Summary:
            if (!edge(slice, {summary_target, frame}))
                return false;
            phase = kind == QueryKind::Effects ? Phase::Edges : Phase::Done;
            return true;
        case Phase::Edges: {
            const auto edges = current_lease->edges();
            if (edge_cursor == edges.size()) {
                phase = Phase::Done;
                return true;
            }
            if (!slice.edges)
                return false;
            --slice.edges;
            ++stats.scanned_edges;
            const auto& next = edges[edge_cursor++];
            if (followed(kind, next.relation)) {
                ++stats.visited_edges;
                enqueue({next.target, state.context}, cursor);
            }
            return true;
        }
        case Phase::Formula:
            if (edge_cursor == formula->terms.size()) {
                phase = Phase::Done;
                return true;
            }
            if (!edge(slice, {formula->terms[edge_cursor], state.context}))
                return false;
            ++edge_cursor;
            return true;
        case Phase::Done:
            current_lease.reset();
            active = false;
            ++cursor;
            return true;
        case Phase::Witness:
            if (witness_cursor != no_predecessor) {
                if (!slice.bindings)
                    return false;
                --slice.bindings;
                ++stats.witness_steps;
                reversed_witness.push_back(queue[witness_cursor].state);
                witness_cursor = queue[witness_cursor].predecessor;
                return true;
            }
            current_lease.reset();
            progress = QueryProgress::Completed;
            return false;
        }
        return false;
    }
};

QueryTask::QueryTask(FrozenGraph graph, NodeId root, QueryKind kind,
                     bool witness)
    : QueryTask(EvidenceView::memory(std::move(graph)), root, kind, witness) {}
QueryTask::QueryTask(std::shared_ptr<const EvidenceView> evidence, NodeId root,
                     QueryKind kind, bool witness)
    : impl_(std::make_unique<Impl>(std::move(evidence), root, kind, witness)) {}
QueryTask::~QueryTask() = default;
QueryTask::QueryTask(QueryTask&&) noexcept = default;
QueryTask& QueryTask::operator=(QueryTask&&) noexcept = default;
QueryTask FrozenGraph::begin_query(NodeId root, QueryKind kind,
                                   bool witness) const {
    return QueryTask(*this, root, kind, witness);
}
void QueryTask::legacy_limit(std::size_t limit) { impl_->limit = limit; }
void QueryTask::formulas(std::shared_ptr<const FormulaIndex> index) {
    impl_->formula_index = std::move(index);
}
void QueryTask::invocation_path(std::span<const NodeId> calls) {
    impl_->initial_calls.assign(calls.begin(), calls.end());
}
QueryProgress QueryTask::progress() const { return impl_->progress; }
EvidenceCoverage QueryTask::coverage() const { return impl_->coverage(); }
const QueryStats& QueryTask::stats() const { return impl_->stats; }
const FrozenGraph& QueryTask::graph() const {
    const auto* graph = impl_->evidence->memory_graph();
    if (!graph)
        throw std::logic_error("disk evidence has no resident FrozenGraph");
    return *graph;
}
const EvidenceView& QueryTask::evidence() const { return *impl_->evidence; }
const std::optional<EvidenceError>& QueryTask::failure() const {
    return impl_->failure;
}
std::span<const Invocation> QueryTask::invocations() const {
    return impl_->contexts;
}
void QueryTask::suspend() {
    if (impl_->progress == QueryProgress::Running)
        impl_->progress = QueryProgress::Paused;
}
void QueryTask::cancel() {
    if (impl_->progress == QueryProgress::Running ||
        impl_->progress == QueryProgress::Paused)
        impl_->progress = QueryProgress::Cancelled;
}
void QueryTask::resume() {
    if (impl_->progress == QueryProgress::Paused ||
        impl_->progress == QueryProgress::Cancelled)
        impl_->progress = QueryProgress::Running;
}
QueryPage QueryTask::advance(QuerySlice slice, std::stop_token cancellation) {
    auto& task = *impl_;
    if (task.progress == QueryProgress::Paused)
        resume();
    if (task.progress == QueryProgress::Running) {
        ++task.stats.slices;
        try {
            while (!cancellation.stop_requested() && task.step(slice)) {
            }
            if (cancellation.stop_requested())
                cancel();
            else if (task.progress == QueryProgress::Running)
                suspend();
        } catch (const EvidenceReadFailure& failure) {
            task.failure = failure.error;
            task.gap(CoverageIssue::UnavailableEvidence);
            task.current_lease.reset();
            task.progress = QueryProgress::Failed;
        } catch (const std::bad_alloc&) {
            task.progress = QueryProgress::Failed;
        } catch (const std::length_error&) {
            task.progress = QueryProgress::Failed;
        }
    }
    QueryPage page;
    // Commit delivery cursors only after the whole page exists. Allocation
    // failure must not silently acknowledge records the caller never received.
    auto delivered_sources = task.delivered_sources;
    auto delivered_witness = task.delivered_witness;
    try {
        if (task.progress != QueryProgress::Failed) {
            while (slice.output && delivered_sources < task.sources.size()) {
                page.sources.push_back(task.sources[delivered_sources]);
                ++delivered_sources;
                --slice.output;
            }
            if (task.progress == QueryProgress::Completed) {
                while (slice.output &&
                       delivered_witness < task.reversed_witness.size()) {
                    page.witness.push_back(
                        task.reversed_witness[task.reversed_witness.size() - 1 -
                                              delivered_witness]);
                    ++delivered_witness;
                    --slice.output;
                }
            }
            task.delivered_sources = delivered_sources;
            task.delivered_witness = delivered_witness;
        }
    } catch (const std::bad_alloc&) {
        task.progress = QueryProgress::Failed;
        page.sources.clear();
        page.witness.clear();
    } catch (const std::length_error&) {
        task.progress = QueryProgress::Failed;
        page.sources.clear();
        page.witness.clear();
    }
    if (!page.sources.empty() || !page.witness.empty())
        page.sequence = ++task.sequence;
    page.progress = task.progress;
    page.coverage = task.coverage();
    page.details = task.progress == QueryProgress::Failed
                       ? DetailAvailability::Unavailable
                       : DetailAvailability::Resident;
    page.delivered_sources = task.delivered_sources;
    page.pending_sources = task.sources.size() - task.delivered_sources;
    page.pending_witness =
        task.delivered_witness < task.reversed_witness.size();
    return page;
}
QueryResult QueryTask::result() const {
    const auto& task = *impl_;
    QueryResult result{
        .sources = task.sources,
        .witness = {},
        .invocations = task.contexts,
        .visited_states = task.stats.visited_states,
        .visited_edges = task.stats.visited_edges,
        .contexts = task.contexts.size() - 1,
        .complete = task.all_sources && !task.issues,
        .recursive_call_cut =
            (task.issues &
             static_cast<std::uint32_t>(CoverageIssue::RecursiveCall)) != 0};
    if (task.progress == QueryProgress::Completed)
        result.witness.assign(task.reversed_witness.rbegin(),
                              task.reversed_witness.rend());
    std::ranges::sort(result.sources,
                      [](const Source& left, const Source& right) {
                          if (left.site != right.site)
                              return left.site < right.site;
                          if (left.context != right.context)
                              return left.context < right.context;
                          return left.node.value < right.node.value;
                      });
    return result;
}

} // namespace pagos::stage::provenance
