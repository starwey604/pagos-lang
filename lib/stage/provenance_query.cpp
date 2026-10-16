#include "pagos/stage/provenance_query.h"
#include "../cache/wire.h"
#include "pagos/stage/provenance_snapshot.h"
#include "pagos/stage/provenance_topology.h"
#include "query_storage.h"

#include <algorithm>
#include <array>
#include <new>
#include <optional>
#include <stdexcept>
#include <tuple>
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
CheckpointError checkpoint_error(cache::Error error) {
    return {error == cache::Error::IncompatibleVersion
                ? CheckpointErrorCode::IncompatibleVersion
            : error == cache::Error::LimitExceeded
                ? CheckpointErrorCode::LimitExceeded
                : CheckpointErrorCode::Corrupt,
            {}};
}
CheckpointError checkpoint_storage_error(cache::StoreError error) {
    return {error.code == cache::StoreErrorCode::LimitExceeded
                ? CheckpointErrorCode::LimitExceeded
                : CheckpointErrorCode::StorageFailure,
            {},
            error};
}
cache::TaskStatus task_status(QueryProgress progress, bool pending) {
    return progress == QueryProgress::Completed && pending
               ? cache::TaskStatus::AwaitingDelivery
           : progress == QueryProgress::Completed ? cache::TaskStatus::Completed
           : progress == QueryProgress::Cancelled ? cache::TaskStatus::Cancelled
                                                  : cache::TaskStatus::Paused;
}
void require_checkpoint(bool condition) {
    if (!condition)
        throw CheckpointError{CheckpointErrorCode::Corrupt, {}};
}
cache::Digest read_digest(cache::wire::Reader& reader) {
    cache::Digest result;
    const auto bytes = reader.raw(result.bytes.size());
    if (reader.good())
        std::ranges::copy(bytes, result.bytes.begin());
    return result;
}
// Stats are explicit wire fields, never an ABI-dependent dump of the struct.
template <class Function>
void query_counters(QueryStats& stats, Function function) {
    function(stats.admitted_states);
    function(stats.visited_states);
    function(stats.scanned_edges);
    function(stats.visited_edges);
    function(stats.bindings);
    function(stats.witness_steps);
    function(stats.queue_peak);
    function(stats.slices);
    function(stats.formula_uses);
    function(stats.formula_fallbacks);
}
} // namespace

struct QueryTask::Impl {
    using Visit = detail::QueryVisit;
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
        contexts.push_back({});
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
    detail::QueryVector<Invocation> contexts{0};
    detail::QueryIndex context_ids{5};
    detail::QueryIndex visited{4};
    detail::QueryVector<Visit> queue{1};
    std::size_t cursor{};
    Phase phase{Phase::Done};
    std::uint32_t frame{};
    NodeId summary_target;
    std::size_t edge_cursor{};
    const DependencyFormula* formula{};
    detail::QueryVector<Source> sources{2};
    detail::QueryVector<QueryState> reversed_witness{3};
    std::size_t witness_cursor{no_predecessor};
    std::size_t delivered_sources{};
    std::size_t delivered_witness{};
    std::uint64_t sequence{};
    std::shared_ptr<const FormulaIndex> formula_index;
    std::vector<NodeId> initial_calls;
    std::size_t initial_cursor{};
    struct Delivery {
        std::size_t source_begin{}, witness_begin{};
        QueryProgress progress;
    };
    std::optional<QueryCheckpointBinding> checkpoint_binding;
    cache::Digest task_id;
    std::uint64_t acknowledged{};
    std::optional<Delivery> unconfirmed;

    struct Offloaded {
        cache::TaskStore store;
        cache::TaskHead head;
        cache::Limits limits;
        EvidenceCoverage coverage;
        std::optional<QueryPagingConfig> paging{};
    };
    std::optional<Offloaded> offloaded;
    std::optional<CheckpointError> state_failure;

    std::shared_ptr<detail::QueryStorage> pages;
    std::optional<CheckpointError> paging_failure;
    std::optional<QueryProgress> storage_resume;
    std::vector<Invocation> compatibility_contexts;
    void attach_pages(std::shared_ptr<detail::QueryStorage> storage,
                      bool import = false) {
        pages = std::move(storage);
        contexts.attach(pages, import);
        queue.attach(pages, import);
        sources.attach(pages, import);
        reversed_witness.attach(pages, import);
        visited.attach(pages);
        context_ids.attach(pages);
    }
    QueryProgress logical_progress() const {
        return storage_resume.value_or(progress);
    }
    QueryPage pause_storage(CheckpointError error) {
        if (!storage_resume)
            storage_resume = progress;
        paging_failure = std::move(error);
        progress = QueryProgress::Paused;
        QueryPage page;
        page.task = task_id;
        page.progress = progress;
        page.coverage = coverage();
        page.details = DetailAvailability::Unavailable;
        page.delivered_sources = delivered_sources;
        page.pending_sources = sources.size() - delivered_sources;
        page.pending_witness = delivered_witness < reversed_witness.size();
        return page;
    }
    bool checked_step(QuerySlice& slice) {
        if (!pages)
            return step(slice);
        pages->prepare();
        const auto marker = pages->mark();
        auto state = std::tuple{
            seeded,         active,      recursive,      all_sources,  progress,
            stats,          issues,      cursor,         phase,        frame,
            summary_target, edge_cursor, witness_cursor, current_lease};
        const auto budget = slice;
        try {
            return step(slice);
        } catch (...) {
            pages->rollback(marker);
            slice = budget;
            std::tie(seeded, active, recursive, all_sources, progress, stats,
                     issues, cursor, phase, frame, summary_target, edge_cursor,
                     witness_cursor, current_lease) = std::move(state);
            throw;
        }
    }
    QueryPage redeliver() const {
        QueryPage page;
        page.task = task_id;
        page.sequence = sequence;
        for (auto i = unconfirmed->source_begin; i < delivered_sources; ++i)
            page.sources.push_back(sources[i]);
        for (auto i = unconfirmed->witness_begin; i < delivered_witness; ++i)
            page.witness.push_back(
                reversed_witness[reversed_witness.size() - 1 - i]);
        page.progress = unconfirmed->progress;
        page.coverage = coverage();
        page.delivered_sources = delivered_sources;
        page.pending_sources = sources.size() - delivered_sources;
        page.pending_witness = delivered_witness < reversed_witness.size();
        return page;
    }

    void write_control(cache::wire::Writer& writer) const {
        writer.u32(query_checkpoint_version);
        writer.u32(query_engine_version);
        writer.u32(provenance_record_model);
        const auto& binding = *checkpoint_binding;
        writer.raw(binding.closure_.bytes);
        writer.raw(binding.positions_.bytes);
        writer.raw(binding.revision_.bytes);
        writer.raw(task_id.bytes);
        writer.u32(root.value);
        writer.u8(static_cast<std::uint8_t>(kind));
        writer.boolean(shortest);
        writer.u8(static_cast<std::uint8_t>(logical_progress()));
        writer.boolean(seeded);
        writer.boolean(active);
        writer.boolean(recursive);
        writer.boolean(all_sources);
        writer.u32(issues);
        auto counters = stats;
        query_counters(counters, [&](std::size_t value) { writer.u64(value); });
        writer.u64(cursor);
        writer.u8(static_cast<std::uint8_t>(phase));
        writer.u32(frame);
        writer.u32(summary_target.value);
        writer.u64(edge_cursor);
        writer.u64(witness_cursor == no_predecessor ? UINT64_MAX
                                                    : witness_cursor);
        writer.u64(delivered_sources);
        writer.u64(delivered_witness);
        writer.u64(sequence);
        writer.u64(acknowledged);
        writer.boolean(unconfirmed.has_value());
        if (unconfirmed) {
            writer.u64(unconfirmed->source_begin);
            writer.u64(unconfirmed->witness_begin);
            writer.u8(static_cast<std::uint8_t>(unconfirmed->progress));
        }
    }
    void validate_restored();
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
        (void)visited.insert(key, position);
        ++stats.admitted_states;
        stats.queue_peak = std::max(stats.queue_peak, queue.size() - cursor);
    }
    EvidenceCoverage coverage() const {
        if (offloaded)
            return offloaded->coverage;
        if (paging_failure)
            return {.precision = EvidencePrecision::Unresolved,
                    .issues = issues | static_cast<std::uint32_t>(
                                           CoverageIssue::UnavailableEvidence)};
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
        if (!current_lease)
            current_lease = pin(state.node);
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
            if (found)
                frame = context_id(*found);
            else {
                const auto id = context_id(contexts.size());
                contexts.push_back({state.node, frame, {current.payload}});
                (void)context_ids.insert(key, id);
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

void QueryTask::Impl::validate_restored() {
    // Validate every index before indexing/rebinding. No graph traversal is
    // replayed: only context calls, reported sources and the active lease
    // need cold detail validation. The frontier remains lazy.
    require_checkpoint(
        progress >= QueryProgress::Running &&
        progress <= QueryProgress::Completed &&
        phase >= Impl::Phase::Parameter && phase <= Impl::Phase::Witness &&
        phase != Impl::Phase::Formula && (issues & ~std::uint32_t{31}) == 0 &&
        !contexts.empty() && contexts[0] == Invocation{});
    const auto valid_node = [&](NodeId node) {
        return node.value && node.value <= evidence->node_count();
    };
    const auto valid_state = [&](QueryState state) {
        return valid_node(state.node) && state.context < contexts.size();
    };
    for (std::size_t i = 1; i < contexts.size(); ++i) {
        const auto& context = contexts[i];
        require_checkpoint(valid_node(context.call) && context.parent < i &&
                           context.owner.value &&
                           context.owner.value <= evidence->summaries().size());
        const auto call = pin(context.call);
        require_checkpoint(call.node().rule == Rule::Call &&
                           call.node().payload == context.owner.value);
        if (pages)
            require_checkpoint(context_ids.find(state_key(
                                   {context.call, context.parent})) == i);
        else
            require_checkpoint(context_ids.insert(
                state_key({context.call, context.parent}), i));
    }
    require_checkpoint(
        cursor <= queue.size() && (!active || cursor < queue.size()) &&
        frame < contexts.size() && stats.admitted_states == queue.size() &&
        stats.visited_states == cursor + (active ? 1U : 0U) &&
        stats.visited_edges <= stats.scanned_edges &&
        stats.queue_peak <= queue.size() &&
        stats.bindings >= contexts.size() - 1 && !stats.formula_uses &&
        !stats.formula_fallbacks);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto& visit = queue[i];
        require_checkpoint(
            valid_state(visit.state) &&
            (i == 0 ? visit.predecessor == no_predecessor &&
                          visit.state == QueryState{root, 0}
                    : visit.predecessor < i &&
                          visit.predecessor < stats.visited_states));
        if (pages)
            require_checkpoint(visited.find(state_key(visit.state)) == i);
        else
            require_checkpoint(visited.insert(state_key(visit.state), i));
    }
    require_checkpoint(seeded == !queue.empty());
    if (!seeded)
        require_checkpoint(!active && contexts.size() == 1 &&
                           phase == Impl::Phase::Done && !all_sources &&
                           !issues);
    if (!active)
        require_checkpoint(phase == Impl::Phase::Done);
    else {
        current_lease = pin(queue[cursor].state.node);
        const auto& current = current_lease->node();
        require_checkpoint(edge_cursor <= current_lease->edges().size());
        switch (phase) {
        case Impl::Phase::Parameter:
        case Impl::Phase::ParameterEdge:
            require_checkpoint(current.rule == Rule::Parameter);
            if (phase == Impl::Phase::ParameterEdge)
                require_checkpoint(frame &&
                                   contexts[frame].owner == current.owner);
            break;
        case Impl::Phase::CallSearch:
        case Impl::Phase::CallFrame:
        case Impl::Phase::Summary: {
            require_checkpoint(current.rule == Rule::Call ||
                               current.rule == Rule::SummaryRef);
            require_checkpoint(edge_cursor == 0);
            require_checkpoint(current.payload &&
                               current.payload <= evidence->summaries().size());
            const auto& summary = evidence->summary({current.payload});
            require_checkpoint(
                summary_target ==
                (kind == QueryKind::Value ? summary.value : summary.effects));
            break;
        }
        case Impl::Phase::Edges:
            require_checkpoint(edge_cursor <= current_lease->edges().size());
            break;
        case Impl::Phase::Witness:
            require_checkpoint(shortest && sources.size() == 1);
            break;
        case Impl::Phase::Done:
            break;
        case Impl::Phase::Formula:
            require_checkpoint(false);
            break;
        }
    }
    std::size_t previous_source = no_predecessor;
    for (const auto& source : sources) {
        require_checkpoint(valid_state({source.node, source.context}));
        const auto found =
            visited.find(state_key({source.node, source.context}));
        require_checkpoint(
            found.has_value() && *found < stats.visited_states &&
            (previous_source == no_predecessor || previous_source < *found));
        previous_source = *found;
        const auto lease = pin(source.node);
        require_checkpoint(lease.node().site == source.site &&
                           (kind == QueryKind::Value
                                ? lease.node().rule == Rule::Origin ||
                                      lease.node().rule == Rule::RuntimeBoundary
                                : lease.node().rule == Rule::Effect));
    }
    require_checkpoint(stats.witness_steps == reversed_witness.size());
    if (phase == Impl::Phase::Witness) {
        auto predecessor_index = cursor;
        require_checkpoint(previous_source == cursor);
        for (const auto& state : reversed_witness) {
            require_checkpoint(predecessor_index != no_predecessor &&
                               queue[predecessor_index].state == state);
            predecessor_index = queue[predecessor_index].predecessor;
        }
        require_checkpoint(witness_cursor == predecessor_index &&
                           !all_sources &&
                           (progress != QueryProgress::Completed ||
                            predecessor_index == no_predecessor));
    } else
        require_checkpoint(reversed_witness.empty() &&
                           witness_cursor == no_predecessor);
    if (all_sources)
        require_checkpoint(progress == QueryProgress::Completed && !active &&
                           cursor == queue.size());
    if (progress == QueryProgress::Completed)
        require_checkpoint(all_sources || phase == Impl::Phase::Witness);
    require_checkpoint(
        delivered_sources <= sources.size() &&
        delivered_witness <= reversed_witness.size() &&
        (!delivered_witness || progress == QueryProgress::Completed) &&
        sequence < UINT64_MAX);
    if (unconfirmed) {
        require_checkpoint(acknowledged < UINT64_MAX &&
                           sequence == acknowledged + 1 &&
                           unconfirmed->source_begin <= delivered_sources &&
                           unconfirmed->witness_begin <= delivered_witness &&
                           (unconfirmed->source_begin < delivered_sources ||
                            unconfirmed->witness_begin < delivered_witness) &&
                           unconfirmed->progress >= QueryProgress::Running &&
                           unconfirmed->progress <= QueryProgress::Completed);
    } else
        require_checkpoint(sequence == acknowledged);
}

QueryTask::QueryTask(FrozenGraph graph, NodeId root, QueryKind kind,
                     bool witness)
    : QueryTask(EvidenceView::memory(std::move(graph)), root, kind, witness) {}
QueryTask::QueryTask(std::shared_ptr<const EvidenceView> evidence, NodeId root,
                     QueryKind kind, bool witness)
    : impl_(std::make_unique<Impl>(std::move(evidence), root, kind, witness)) {}
QueryTask::QueryTask(QueryCheckpointBinding binding, cache::Digest task_id,
                     NodeId root, QueryKind kind, bool witness)
    : QueryTask(binding.view_, root, kind, witness) {
    impl_->checkpoint_binding = std::move(binding);
    impl_->task_id = task_id;
}
QueryTask::~QueryTask() = default;
QueryTask::QueryTask(QueryTask&&) noexcept = default;
QueryTask& QueryTask::operator=(QueryTask&&) noexcept = default;
QueryTask FrozenGraph::begin_query(NodeId root, QueryKind kind,
                                   bool witness) const {
    return QueryTask(*this, root, kind, witness);
}
void QueryTask::legacy_limit(std::size_t limit) {
    if (impl_->pages || (impl_->offloaded && impl_->offloaded->paging))
        throw std::invalid_argument("paged query cannot drop states");
    impl_->limit = limit;
}
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
    if (!ensure_resident())
        throw std::runtime_error("query state unavailable");
    if (impl_->pages) {
        try {
            if (impl_->compatibility_contexts.empty())
                impl_->compatibility_contexts = impl_->contexts.materialize();
            return impl_->compatibility_contexts;
        } catch (const CheckpointError& error) {
            (void)impl_->pause_storage(error);
            throw std::runtime_error("query contexts unavailable");
        }
    }
    return impl_->contexts.resident_span();
}
void QueryTask::suspend() {
    if (impl_->progress == QueryProgress::Running)
        impl_->progress = QueryProgress::Paused;
}
void QueryTask::cancel() {
    if (impl_->storage_resume)
        impl_->storage_resume = QueryProgress::Cancelled;
    if (impl_->progress == QueryProgress::Running ||
        impl_->progress == QueryProgress::Paused)
        impl_->progress = QueryProgress::Cancelled;
}
void QueryTask::resume() {
    if (impl_->storage_resume) {
        impl_->progress = *impl_->storage_resume;
        impl_->storage_resume.reset();
        impl_->paging_failure.reset();
    }
    if (impl_->progress == QueryProgress::Paused ||
        impl_->progress == QueryProgress::Cancelled)
        impl_->progress = QueryProgress::Running;
}
QueryPage QueryTask::advance(QuerySlice slice, std::stop_token cancellation) {
    if (!ensure_resident()) {
        QueryPage page;
        page.task = impl_->task_id;
        page.progress = impl_->progress;
        page.coverage = impl_->coverage();
        page.details = DetailAvailability::Unavailable;
        return page;
    }
    auto& task = *impl_;
    std::vector<Invocation>{}.swap(task.compatibility_contexts);
    if (task.paging_failure && task.progress == QueryProgress::Cancelled) {
        QueryPage page;
        page.task = task.task_id;
        page.progress = QueryProgress::Cancelled;
        page.coverage = task.coverage();
        page.details = DetailAvailability::Unavailable;
        return page;
    }
    if (task.paging_failure)
        resume();
    if (task.unconfirmed) {
        if (cancellation.stop_requested())
            cancel();
        try {
            return task.redeliver();
        } catch (const CheckpointError& error) {
            return task.pause_storage(error);
        }
    }
    if (task.progress == QueryProgress::Paused)
        resume();
    if (task.progress == QueryProgress::Running) {
        ++task.stats.slices;
        try {
            while (!cancellation.stop_requested() && task.checked_step(slice)) {
            }
            if (cancellation.stop_requested())
                cancel();
            else if (task.progress == QueryProgress::Running)
                suspend();
        } catch (const CheckpointError& error) {
            return task.pause_storage(error);
        } catch (const EvidenceReadFailure& failure) {
            task.failure = failure.error;
            task.gap(CoverageIssue::UnavailableEvidence);
            task.current_lease.reset();
            task.progress = QueryProgress::Failed;
        } catch (const std::bad_alloc&) {
            if (task.pages)
                return task.pause_storage(
                    {CheckpointErrorCode::OutOfMemory, {}});
            task.progress = QueryProgress::Failed;
        } catch (const std::length_error&) {
            if (task.pages)
                return task.pause_storage(
                    {CheckpointErrorCode::LimitExceeded, {}});
            task.progress = QueryProgress::Failed;
        }
    }
    QueryPage page;
    // Commit delivery cursors only after the whole page exists. Allocation
    // failure must not silently acknowledge records the caller never received.
    auto delivered_sources = task.delivered_sources;
    auto delivered_witness = task.delivered_witness;
    const auto source_begin = delivered_sources;
    const auto witness_begin = delivered_witness;
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
            if (task.sequence == UINT64_MAX &&
                (!page.sources.empty() || !page.witness.empty()))
                throw std::length_error("query delivery sequence exhausted");
            task.delivered_sources = delivered_sources;
            task.delivered_witness = delivered_witness;
        }
    } catch (const CheckpointError& error) {
        return task.pause_storage(error);
    } catch (const std::bad_alloc&) {
        if (task.pages)
            return task.pause_storage({CheckpointErrorCode::OutOfMemory, {}});
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
    if (task.checkpoint_binding) {
        page.task = task.task_id;
        if (page.sequence)
            task.unconfirmed =
                Impl::Delivery{source_begin, witness_begin, task.progress};
    }
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
bool QueryTask::acknowledge(std::uint64_t sequence) {
    if (!ensure_resident())
        return false;
    auto& task = *impl_;
    if (!task.checkpoint_binding)
        return false;
    if (sequence <= task.acknowledged)
        return true;
    if (!task.unconfirmed || sequence != task.sequence)
        return false;
    task.acknowledged = sequence;
    task.unconfirmed.reset();
    return true;
}
std::expected<cache::Bytes, CheckpointError>
QueryTask::checkpoint(cache::Limits limits) const {
    if (auto loaded = ensure_resident(); !loaded)
        return std::unexpected(loaded.error());
    const auto& task = *impl_;
    if (!task.checkpoint_binding || task.progress == QueryProgress::Failed ||
        task.formula_index || task.limit || !task.initial_calls.empty())
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::InvalidState, {}});
    try {
        cache::wire::Writer writer(limits);
        cache::wire::header(writer, cache::ObjectKind::QueryCheckpoint);
        task.write_control(writer);
        writer.count(task.contexts.size());
        for (const auto& context : task.contexts) {
            writer.u32(context.call.value);
            writer.u32(context.parent);
            writer.u32(context.owner.value);
        }
        // The append-only queue is also the canonical visited set. Rebuild
        // its hash index on restore instead of storing allocator/hash layouts.
        writer.count(task.queue.size());
        for (const auto& visit : task.queue) {
            writer.u32(visit.state.node.value);
            writer.u32(visit.state.context);
            writer.u64(visit.predecessor == no_predecessor ? UINT64_MAX
                                                           : visit.predecessor);
        }
        writer.count(task.sources.size());
        for (const auto& source : task.sources) {
            writer.u32(source.node.value);
            writer.u32(source.context);
            writer.u32(source.site);
        }
        writer.count(task.reversed_witness.size());
        for (const auto& state : task.reversed_witness) {
            writer.u32(state.node.value);
            writer.u32(state.context);
        }
        auto bytes = writer.finish();
        if (!bytes)
            return std::unexpected(checkpoint_error(bytes.error()));
        const auto checksum =
            cache::fingerprint("pagos.query-checkpoint.v1", *bytes);
        if (limits.bytes - bytes->size() < checksum.bytes.size())
            return std::unexpected(
                CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
        bytes->insert(bytes->end(), checksum.bytes.begin(),
                      checksum.bytes.end());
        return std::move(*bytes);
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}

QueryTask QueryTask::read_control(QueryCheckpointBinding binding,
                                  cache::Digest task_id, NodeId root,
                                  QueryKind kind, bool witness,
                                  cache::wire::Reader& reader) {
    const auto finish = [&] {
        if (auto status = reader.finish(); !status)
            throw checkpoint_error(status.error());
    };
    if (reader.u32() != query_checkpoint_version ||
        reader.u32() != query_engine_version ||
        reader.u32() != provenance_record_model)
        reader.fail(cache::Error::IncompatibleVersion);
    if (!reader.good())
        finish();
    const auto closure = read_digest(reader);
    const auto positions = read_digest(reader);
    const auto revision = read_digest(reader);
    const auto saved_task = read_digest(reader);
    const auto saved_root = reader.u32();
    const auto saved_kind = reader.u8();
    const auto saved_witness = reader.boolean();
    if (!reader.good())
        finish();
    if (closure != binding.closure_ || positions != binding.positions_ ||
        revision != binding.revision_ || saved_task != task_id ||
        saved_root != root.value || saved_kind != static_cast<unsigned>(kind) ||
        saved_witness != witness)
        throw CheckpointError{CheckpointErrorCode::BindingMismatch, {}};
    require_checkpoint(
        binding.view_ && root.value &&
        root.value <= binding.view_->node_count() &&
        (kind == QueryKind::Value || kind == QueryKind::Effects));
    QueryTask result(std::move(binding), task_id, root, kind, witness);
    auto& task = *result.impl_;
    const auto size = [&]() -> std::size_t {
        const auto value = reader.u64();
        if (value > std::numeric_limits<std::size_t>::max())
            reader.fail(cache::Error::LimitExceeded);
        return static_cast<std::size_t>(value);
    };
    const auto predecessor = [&]() -> std::size_t {
        const auto value = reader.u64();
        if (value == UINT64_MAX)
            return no_predecessor;
        if (value >= std::numeric_limits<std::size_t>::max())
            reader.fail(cache::Error::LimitExceeded);
        return static_cast<std::size_t>(value);
    };
    task.progress = static_cast<QueryProgress>(reader.u8());
    task.seeded = reader.boolean();
    task.active = reader.boolean();
    task.recursive = reader.boolean();
    task.all_sources = reader.boolean();
    task.issues = reader.u32();
    query_counters(task.stats, [&](std::size_t& value) { value = size(); });
    task.cursor = size();
    task.phase = static_cast<Impl::Phase>(reader.u8());
    task.frame = reader.u32();
    task.summary_target = {reader.u32()};
    task.edge_cursor = size();
    task.witness_cursor = predecessor();
    task.delivered_sources = size();
    task.delivered_witness = size();
    task.sequence = reader.u64();
    task.acknowledged = reader.u64();
    if (reader.boolean()) {
        const auto source_begin = size();
        const auto witness_begin = size();
        task.unconfirmed =
            Impl::Delivery{source_begin, witness_begin,
                           static_cast<QueryProgress>(reader.u8())};
    }
    if (!reader.good())
        finish();
    return result;
}

std::expected<QueryTask, CheckpointError>
QueryTask::restore(QueryCheckpointBinding binding, cache::Digest task_id,
                   NodeId root, QueryKind kind, bool witness,
                   std::span<const std::uint8_t> bytes, cache::Limits limits) {
    try {
        if (bytes.size() > limits.bytes)
            return std::unexpected(
                CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
        require_checkpoint(bytes.size() >= 32);
        const auto payload = bytes.first(bytes.size() - 32);
        const auto checksum =
            cache::fingerprint("pagos.query-checkpoint.v1", payload);
        require_checkpoint(std::ranges::equal(checksum.bytes, bytes.last(32)));
        cache::wire::Reader reader(payload, limits);
        cache::wire::header(reader, cache::ObjectKind::QueryCheckpoint);
        const auto finish = [&] {
            if (auto status = reader.finish(); !status)
                throw checkpoint_error(status.error());
        };
        auto result = read_control(std::move(binding), task_id, root, kind,
                                   witness, reader);
        auto& task = *result.impl_;
        const auto predecessor = [&]() -> std::size_t {
            const auto value = reader.u64();
            if (value == UINT64_MAX)
                return no_predecessor;
            if (value >= std::numeric_limits<std::size_t>::max())
                reader.fail(cache::Error::LimitExceeded);
            return static_cast<std::size_t>(value);
        };
        auto count = reader.count(12);
        task.contexts.clear();
        for (std::uint32_t i = 0; i < count && reader.good(); ++i)
            task.contexts.push_back(
                {{reader.u32()}, reader.u32(), {reader.u32()}});
        count = reader.count(16);
        for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
            QueryState state{{reader.u32()}, reader.u32()};
            task.queue.push_back({state, predecessor()});
        }
        count = reader.count(12);
        for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
            const auto node = reader.u32();
            const auto context = reader.u32();
            task.sources.push_back({{node}, reader.u32(), context});
        }
        count = reader.count(8);
        for (std::uint32_t i = 0; i < count && reader.good(); ++i)
            task.reversed_witness.push_back({{reader.u32()}, reader.u32()});
        finish();

        task.validate_restored();
        return result;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const EvidenceReadFailure& error) {
        return std::unexpected(CheckpointError{
            CheckpointErrorCode::UnavailableEvidence, error.error});
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}

std::expected<QueryTask, CheckpointError>
QueryTask::begin_paged(QueryCheckpointBinding binding,
                       const cache::TaskStore& store, NodeId root,
                       QueryKind kind, bool witness, QueryPagingConfig config,
                       cache::Limits limits) {
    try {
        if (!binding.view_ || !root.value ||
            root.value > binding.view_->node_count() ||
            (kind != QueryKind::Value && kind != QueryKind::Effects))
            throw CheckpointError{CheckpointErrorCode::InvalidState, {}};
        auto previous = store.head();
        if (!previous)
            throw checkpoint_storage_error(previous.error());
        if (*previous)
            throw CheckpointError{CheckpointErrorCode::InvalidState, {}};
        QueryTask task(std::move(binding), store.task(), root, kind, witness);
        task.impl_->attach_pages(
            std::make_shared<detail::QueryStorage>(store, config, limits),
            true);
        return task;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}
const std::optional<CheckpointError>& QueryTask::storage_failure() const {
    return impl_->paging_failure ? impl_->paging_failure : impl_->state_failure;
}
QueryPagingStats QueryTask::paging_stats() const {
    return impl_->pages ? impl_->pages->stats() : QueryPagingStats{};
}
void QueryTask::evict_state_cache() const {
    if (impl_->pages)
        impl_->pages->evict();
}
std::expected<void, CheckpointError>
QueryTask::resume_with_storage(const cache::TaskStore& store) {
    if (auto loaded = ensure_resident(); !loaded)
        return loaded;
    if (!impl_->pages)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::InvalidState, {}});
    try {
        impl_->pages->rebind(store);
        resume();
        return {};
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    }
}
std::expected<cache::TaskHead, CheckpointError>
QueryTask::save_paged(const cache::TaskStore& store, std::uint64_t generation,
                      cache::Limits limits) const {
    auto& task = *impl_;
    if (!task.checkpoint_binding || task.progress == QueryProgress::Failed ||
        task.limit || task.formula_index || !task.initial_calls.empty())
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::InvalidState, {}});
    try {
        task.pages->check_store(store);
        cache::wire::Writer control(limits);
        cache::wire::header(control, cache::ObjectKind::QueryTaskManifest);
        control.u32(2);
        task.write_control(control);
        auto prefix = control.finish();
        if (!prefix)
            throw checkpoint_error(prefix.error());
        auto metadata_bytes = prefix->size();
        const auto tails = task.pages->mark();
        for (unsigned table = 0; table < 6; ++table) {
            const auto segments = task.pages->directory()[table].size() +
                                  (tails[table] ? 1U : 0U);
            const auto width = table < 4 ? 49U : 65U;
            if (task.pages->size(table) > limits.elements ||
                limits.bytes - metadata_bytes < 8 ||
                segments > (limits.bytes - metadata_bytes - 8) / width)
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            metadata_bytes += 8 + segments * width;
        }
        auto transaction = store.begin(generation);
        if (!transaction)
            throw checkpoint_storage_error(transaction.error());
        const auto previous = store.head();
        if (!previous)
            throw checkpoint_storage_error(previous.error());
        if (*previous) {
            auto bytes = store.read((**previous).manifest);
            if (!bytes)
                throw checkpoint_storage_error(bytes.error());
            cache::wire::Reader reader(*bytes, limits);
            cache::wire::header(reader, cache::ObjectKind::QueryTaskManifest);
            if (reader.u32() != 2)
                throw CheckpointError{CheckpointErrorCode::IncompatibleVersion,
                                      {}};
            (void)read_control(*task.checkpoint_binding, task.task_id,
                               task.root, task.kind, task.shortest, reader);
        }
        task.pages->seal(*transaction);
        cache::wire::Writer manifest(limits);
        manifest.raw(*prefix);
        for (unsigned table = 0; table < 6; ++table) {
            manifest.count(task.pages->size(table));
            const auto& directory = task.pages->directory()[table];
            manifest.count(directory.size());
            for (const auto& segment : directory) {
                manifest.u32(segment.first);
                manifest.u32(segment.count);
                manifest.u8(static_cast<std::uint8_t>(segment.object.kind));
                manifest.raw(segment.object.digest.bytes);
                manifest.u64(segment.object.size);
                if (table >= 4) {
                    manifest.u64(segment.minimum);
                    manifest.u64(segment.maximum);
                }
            }
        }
        auto bytes = manifest.finish();
        if (!bytes)
            throw checkpoint_error(bytes.error());
        auto object = transaction->write(cache::ObjectKind::QueryTaskManifest,
                                         std::move(*bytes));
        if (!object)
            throw checkpoint_storage_error(object.error());
        const bool pending =
            task.unconfirmed || task.delivered_sources < task.sources.size() ||
            task.delivered_witness < task.reversed_witness.size();
        auto head = transaction->commit(
            *object, task_status(task.logical_progress(), pending));
        if (!head)
            throw checkpoint_storage_error(head.error());
        return *head;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}
std::expected<QueryTask, CheckpointError> QueryTask::restore_paged(
    QueryCheckpointBinding binding, const cache::TaskStore& store,
    const cache::TaskHead& head, NodeId root, QueryKind kind, bool witness,
    QueryPagingConfig config, cache::Limits limits) {
    try {
        require_checkpoint(head.generation &&
                           head.manifest.kind ==
                               cache::ObjectKind::QueryTaskManifest &&
                           head.manifest.size <= limits.bytes);
        auto bytes = store.read(head.manifest);
        if (!bytes)
            throw checkpoint_storage_error(bytes.error());
        cache::wire::Reader reader(*bytes, limits);
        cache::wire::header(reader, cache::ObjectKind::QueryTaskManifest);
        if (reader.u32() != 2)
            throw CheckpointError{CheckpointErrorCode::IncompatibleVersion, {}};
        auto task = read_control(std::move(binding), store.task(), root, kind,
                                 witness, reader);
        detail::QueryStorage::Directory directory;
        detail::QueryStorage::Sizes totals{};
        for (unsigned table = 0; table < 6; ++table) {
            totals[table] = reader.count(0);
            const auto segments = reader.count(table < 4 ? 49 : 65);
            if (!reader.good())
                throw checkpoint_error(reader.finish().error());
            require_checkpoint(segments <= totals[table]);
            for (std::uint32_t i = 0; i < segments; ++i) {
                detail::QuerySegment segment;
                segment.first = reader.u32();
                segment.count = reader.u32();
                segment.object = {static_cast<cache::ObjectKind>(reader.u8()),
                                  read_digest(reader), reader.u64()};
                if (table >= 4) {
                    segment.minimum = reader.u64();
                    segment.maximum = reader.u64();
                }
                if (!reader.good())
                    throw checkpoint_error(reader.finish().error());
                directory[table].push_back(segment);
            }
        }
        if (auto status = reader.finish(); !status)
            throw checkpoint_error(status.error());
        auto pages =
            std::make_shared<detail::QueryStorage>(store, config, limits);
        pages->restore(std::move(directory), totals);
        auto& state = *task.impl_;
        const bool pending = state.unconfirmed ||
                             state.delivered_sources < totals[2] ||
                             state.delivered_witness < totals[3];
        require_checkpoint(task_status(state.progress, pending) == head.status);
        state.attach_pages(std::move(pages));
        // Stream cross-table validation and index bijections, retaining only
        // the configured read cache. No in-memory visited map is rebuilt.
        state.validate_restored();
        return task;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const EvidenceReadFailure& error) {
        return std::unexpected(CheckpointError{
            CheckpointErrorCode::UnavailableEvidence, error.error});
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}

std::expected<cache::TaskHead, CheckpointError>
QueryTask::save(const cache::TaskStore& store,
                std::uint64_t expected_generation, std::size_t segment_bytes,
                cache::Limits limits) const {
    if (auto loaded = ensure_resident(); !loaded)
        return std::unexpected(loaded.error());
    const auto& task = *impl_;
    if (task.pages)
        return save_paged(store, expected_generation, limits);
    if (!task.checkpoint_binding || task.progress == QueryProgress::Failed ||
        task.formula_index || task.limit || !task.initial_calls.empty() ||
        store.task() != task.task_id)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::InvalidState, {}});
    try {
        constexpr std::size_t header_bytes = 54;
        if (segment_bytes < header_bytes + 16 || segment_bytes > limits.bytes)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        const std::array counts{task.contexts.size(), task.queue.size(),
                                task.sources.size(),
                                task.reversed_witness.size()};
        constexpr std::array widths{12U, 16U, 12U, 8U};
        cache::wire::Writer control(limits);
        cache::wire::header(control, cache::ObjectKind::QueryTaskManifest);
        control.u32(1);
        task.write_control(control);
        auto prefix = control.finish();
        if (!prefix)
            throw checkpoint_error(prefix.error());
        auto directory_bytes = prefix->size();
        // Reject an unpublishable directory before emitting any leaf objects.
        for (unsigned table = 0; table < 4; ++table) {
            const auto capacity =
                (segment_bytes - header_bytes) / widths[table];
            const auto segments = counts[table] / capacity +
                                  (counts[table] % capacity != 0 ? 1U : 0U);
            if (counts[table] > limits.elements || counts[table] > UINT32_MAX ||
                limits.bytes - directory_bytes < 8 ||
                segments > (limits.bytes - directory_bytes - 8) / 49)
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            directory_bytes += 8 + segments * 49;
        }
        auto transaction = store.begin(expected_generation);
        if (!transaction)
            throw checkpoint_storage_error(transaction.error());
        // A namespace is one logical query. A caller may not reuse its task
        // ID to move a head onto a different root, revision, channel or engine.
        const auto previous = store.head();
        if (!previous)
            throw checkpoint_storage_error(previous.error());
        if (*previous) {
            auto old = store.read((**previous).manifest);
            if (!old)
                throw checkpoint_storage_error(old.error());
            cache::wire::Reader reader(*old, limits);
            cache::wire::header(reader, cache::ObjectKind::QueryTaskManifest);
            if (reader.u32() != 1)
                throw CheckpointError{CheckpointErrorCode::IncompatibleVersion,
                                      {}};
            (void)read_control(*task.checkpoint_binding, task.task_id,
                               task.root, task.kind, task.shortest, reader);
        }
        cache::wire::Writer manifest(limits);
        manifest.raw(*prefix);
        for (unsigned table = 0; table < 4; ++table) {
            manifest.count(counts[table]);
            const auto capacity =
                (segment_bytes - header_bytes) / widths[table];
            const auto segments = counts[table] / capacity +
                                  (counts[table] % capacity != 0 ? 1U : 0U);
            manifest.count(segments);
            for (std::size_t first = 0; first < counts[table];) {
                const auto count = std::min(capacity, counts[table] - first);
                cache::wire::Writer segment(
                    {.bytes = segment_bytes, .elements = limits.elements});
                cache::wire::header(segment,
                                    cache::ObjectKind::QueryStateSegment);
                segment.u32(1);
                segment.raw(task.task_id.bytes);
                segment.u8(static_cast<std::uint8_t>(table));
                segment.count(first);
                segment.count(count);
                for (std::size_t i = first; i < first + count; ++i) {
                    switch (table) {
                    case 0: {
                        const auto& context = task.contexts[i];
                        segment.u32(context.call.value);
                        segment.u32(context.parent);
                        segment.u32(context.owner.value);
                        break;
                    }
                    case 1: {
                        const auto& visit = task.queue[i];
                        segment.u32(visit.state.node.value);
                        segment.u32(visit.state.context);
                        segment.u64(visit.predecessor == no_predecessor
                                        ? UINT64_MAX
                                        : visit.predecessor);
                        break;
                    }
                    case 2: {
                        const auto& source = task.sources[i];
                        segment.u32(source.node.value);
                        segment.u32(source.context);
                        segment.u32(source.site);
                        break;
                    }
                    case 3:
                        segment.u32(task.reversed_witness[i].node.value);
                        segment.u32(task.reversed_witness[i].context);
                        break;
                    }
                }
                auto bytes = segment.finish();
                if (!bytes)
                    throw checkpoint_error(bytes.error());
                auto ref = transaction->write(
                    cache::ObjectKind::QueryStateSegment, std::move(*bytes));
                if (!ref)
                    throw checkpoint_storage_error(ref.error());
                manifest.count(first);
                manifest.count(count);
                manifest.u8(static_cast<std::uint8_t>(ref->kind));
                manifest.raw(ref->digest.bytes);
                manifest.u64(ref->size);
                first += count;
            }
        }
        auto bytes = manifest.finish();
        if (!bytes)
            throw checkpoint_error(bytes.error());
        auto root = transaction->write(cache::ObjectKind::QueryTaskManifest,
                                       std::move(*bytes));
        if (!root)
            throw checkpoint_storage_error(root.error());
        const bool pending =
            task.unconfirmed || task.delivered_sources < task.sources.size() ||
            task.delivered_witness < task.reversed_witness.size();
        auto head =
            transaction->commit(*root, task_status(task.progress, pending));
        if (!head)
            throw checkpoint_storage_error(head.error());
        return *head;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}

std::expected<QueryTask, CheckpointError>
QueryTask::restore(QueryCheckpointBinding binding,
                   const cache::TaskStore& store, const cache::TaskHead& head,
                   NodeId root, QueryKind kind, bool witness,
                   cache::Limits limits) {
    try {
        require_checkpoint(head.generation &&
                           head.manifest.kind ==
                               cache::ObjectKind::QueryTaskManifest &&
                           head.manifest.size <= limits.bytes &&
                           head.status <= cache::TaskStatus::Completed);
        auto bytes = store.read(head.manifest);
        if (!bytes)
            throw checkpoint_storage_error(bytes.error());
        cache::wire::Reader reader(*bytes, limits);
        cache::wire::header(reader, cache::ObjectKind::QueryTaskManifest);
        const auto version = reader.u32();
        if (version == 2)
            return restore_paged(std::move(binding), store, head, root, kind,
                                 witness, {}, limits);
        if (version != 1)
            throw CheckpointError{CheckpointErrorCode::IncompatibleVersion, {}};
        auto result = read_control(std::move(binding), store.task(), root, kind,
                                   witness, reader);
        auto& task = *result.impl_;
        struct Segment {
            unsigned table;
            std::uint32_t first, count;
            cache::ObjectRef ref;
        };
        std::vector<Segment> directory;
        constexpr std::array widths{12U, 16U, 12U, 8U};
        std::array<std::uint32_t, 4> totals{};
        for (unsigned table = 0; table < 4; ++table) {
            const auto total = reader.count(0);
            const auto count = reader.count(49);
            if (!reader.good())
                throw checkpoint_error(reader.finish().error());
            require_checkpoint(reader.good() && count <= total);
            std::uint32_t next{};
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto first = reader.u32(), records = reader.u32();
                cache::ObjectRef ref{
                    static_cast<cache::ObjectKind>(reader.u8()),
                    read_digest(reader), reader.u64()};
                require_checkpoint(
                    reader.good() && records && first == next &&
                    records <= total - next &&
                    ref.kind == cache::ObjectKind::QueryStateSegment &&
                    ref.size == 54 + std::uint64_t{records} * widths[table] &&
                    ref.size <= limits.bytes);
                next += records;
                directory.push_back({table, first, records, ref});
            }
            require_checkpoint(next == total);
            totals[table] = total;
        }
        if (auto status = reader.finish(); !status)
            throw checkpoint_error(status.error());
        const bool pending = task.unconfirmed ||
                             task.delivered_sources < totals[2] ||
                             task.delivered_witness < totals[3];
        require_checkpoint(task_status(task.progress, pending) == head.status);
        task.contexts.clear();
        for (const auto& entry : directory) {
            auto payload = store.read(entry.ref);
            if (!payload)
                throw checkpoint_storage_error(payload.error());
            cache::wire::Reader segment(*payload, limits);
            cache::wire::header(segment, cache::ObjectKind::QueryStateSegment);
            if (segment.u32() != 1)
                throw CheckpointError{CheckpointErrorCode::IncompatibleVersion,
                                      {}};
            require_checkpoint(read_digest(segment) == task.task_id &&
                               segment.u8() == entry.table &&
                               segment.u32() == entry.first &&
                               segment.u32() == entry.count);
            for (std::uint32_t i = 0; i < entry.count; ++i) {
                const auto node = segment.u32(), context = segment.u32();
                switch (entry.table) {
                case 0:
                    task.contexts.push_back({{node}, context, {segment.u32()}});
                    break;
                case 1: {
                    const auto predecessor = segment.u64();
                    if (predecessor != UINT64_MAX &&
                        predecessor >= std::numeric_limits<std::size_t>::max())
                        throw CheckpointError{
                            CheckpointErrorCode::LimitExceeded, {}};
                    task.queue.push_back(
                        {{{node}, context},
                         predecessor == UINT64_MAX
                             ? no_predecessor
                             : static_cast<std::size_t>(predecessor)});
                    break;
                }
                case 2:
                    task.sources.push_back({{node}, segment.u32(), context});
                    break;
                case 3:
                    task.reversed_witness.push_back({{node}, context});
                    break;
                }
            }
            if (auto status = segment.finish(); !status)
                throw checkpoint_error(status.error());
        }
        require_checkpoint(task.contexts.size() == totals[0] &&
                           task.queue.size() == totals[1] &&
                           task.sources.size() == totals[2] &&
                           task.reversed_witness.size() == totals[3]);
        task.validate_restored();
        return result;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const EvidenceReadFailure& error) {
        return std::unexpected(CheckpointError{
            CheckpointErrorCode::UnavailableEvidence, error.error});
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
}

std::expected<cache::TaskHead, CheckpointError>
QueryTask::offload(const cache::TaskStore& store,
                   std::uint64_t expected_generation, std::size_t segment_bytes,
                   cache::Limits limits) {
    const auto saved = save(store, expected_generation, segment_bytes, limits);
    if (!saved)
        return std::unexpected(saved.error());
    auto& task = *impl_;
    task.offloaded = Impl::Offloaded{
        store, *saved, limits, task.coverage(),
        task.pages ? std::optional(task.pages->config()) : std::nullopt};
    task.contexts.release();
    task.queue.release();
    task.sources.release();
    task.reversed_witness.release();
    task.visited.release();
    task.context_ids.release();
    task.current_lease.reset();
    task.pages.reset();
    std::vector<Invocation>{}.swap(task.compatibility_contexts);
    return *saved;
}
std::expected<void, CheckpointError> QueryTask::ensure_resident() const {
    auto& task = *impl_;
    if (!task.offloaded)
        return {};
    if (task.state_failure)
        return std::unexpected(*task.state_failure);
    const auto& state = *task.offloaded;
    auto restored =
        state.paging
            ? restore_paged(*task.checkpoint_binding, state.store, state.head,
                            task.root, task.kind, task.shortest, *state.paging,
                            state.limits)
            : restore(*task.checkpoint_binding, state.store, state.head,
                      task.root, task.kind, task.shortest, state.limits);
    if (!restored) {
        if (state.paging) {
            if (!task.storage_resume)
                task.storage_resume = task.progress;
            task.paging_failure = restored.error();
            task.progress = QueryProgress::Paused;
            task.offloaded->coverage.precision = EvidencePrecision::Unresolved;
            task.offloaded->coverage.issues |=
                static_cast<std::uint32_t>(CoverageIssue::UnavailableEvidence);
            task.offloaded->coverage.all_sources = false;
            return std::unexpected(restored.error());
        }
        task.state_failure = restored.error();
        task.failure = restored.error().evidence.value_or(EvidenceError{
            EvidenceErrorCode::IoFailure, restored.error().storage, {}});
        task.progress = QueryProgress::Failed;
        task.offloaded->coverage.precision = EvidencePrecision::Unresolved;
        task.offloaded->coverage.issues |=
            static_cast<std::uint32_t>(CoverageIssue::UnavailableEvidence);
        task.offloaded->coverage.all_sources = false;
        return std::unexpected(restored.error());
    }
    const auto progress =
        task.logical_progress(); // Preserve cancel/resume while offloaded.
    task = std::move(*restored->impl_);
    task.progress = progress;
    return {};
}
QueryStateMemory QueryTask::state_memory() const {
    const auto& task = *impl_;
    return {task.queue.bytes(),
            task.visited.bytes(),
            task.contexts.bytes(),
            task.context_ids.bytes(),
            task.sources.bytes(),
            task.reversed_witness.bytes(),
            task.offloaded.has_value(),
            task.pages ? task.pages->cached_bytes() : 0,
            task.pages ? task.pages->directory_bytes() : 0,
            task.compatibility_contexts.capacity() * sizeof(Invocation),
            task.pages ? task.pages->peak_buffers() : 0,
            task.pages != nullptr};
}

QueryResult QueryTask::result() const {
    if (!ensure_resident())
        throw std::runtime_error("query state unavailable");
    try {
        const auto& task = *impl_;
        QueryResult result{
            .sources = task.sources.materialize(),
            .witness = {},
            .invocations = task.contexts.materialize(),
            .visited_states = task.stats.visited_states,
            .visited_edges = task.stats.visited_edges,
            .contexts = task.contexts.size() - 1,
            .complete = task.coverage().all_sources && !task.coverage().issues,
            .recursive_call_cut =
                (task.issues & static_cast<std::uint32_t>(
                                   CoverageIssue::RecursiveCall)) != 0};
        if (task.progress == QueryProgress::Completed) {
            result.witness = task.reversed_witness.materialize();
            std::ranges::reverse(result.witness);
        }
        std::ranges::sort(result.sources,
                          [](const Source& left, const Source& right) {
                              if (left.site != right.site)
                                  return left.site < right.site;
                              if (left.context != right.context)
                                  return left.context < right.context;
                              return left.node.value < right.node.value;
                          });
        return result;
    } catch (const CheckpointError& error) {
        (void)impl_->pause_storage(error);
        throw std::runtime_error("query result unavailable");
    }
}

} // namespace pagos::stage::provenance
