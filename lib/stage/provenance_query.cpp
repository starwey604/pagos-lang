#include "pagos/stage/provenance_query.h"
#include "../cache/wire.h"
#include "pagos/stage/provenance_snapshot.h"
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
CheckpointError checkpoint_error(cache::Error error) {
    return {error == cache::Error::IncompatibleVersion
                ? CheckpointErrorCode::IncompatibleVersion
            : error == cache::Error::LimitExceeded
                ? CheckpointErrorCode::LimitExceeded
                : CheckpointErrorCode::Corrupt,
            {}};
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
    struct Delivery {
        std::size_t source_begin{}, witness_begin{};
        QueryProgress progress;
    };
    std::optional<QueryCheckpointBinding> checkpoint_binding;
    cache::Digest task_id;
    std::uint64_t acknowledged{};
    std::optional<Delivery> unconfirmed;

    QueryPage redeliver() const {
        QueryPage page;
        page.task = task_id;
        page.sequence = sequence;
        page.sources.assign(
            sources.begin() +
                static_cast<std::ptrdiff_t>(unconfirmed->source_begin),
            sources.begin() + static_cast<std::ptrdiff_t>(delivered_sources));
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
    if (task.unconfirmed) {
        if (cancellation.stop_requested())
            cancel();
        return task.redeliver();
    }
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
    const auto& task = *impl_;
    if (!task.checkpoint_binding || task.progress == QueryProgress::Failed ||
        task.formula_index || task.limit || !task.initial_calls.empty())
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::InvalidState, {}});
    try {
        cache::wire::Writer writer(limits);
        cache::wire::header(writer, cache::ObjectKind::QueryCheckpoint);
        writer.u32(query_checkpoint_version);
        writer.u32(query_engine_version);
        writer.u32(provenance_record_model);
        const auto& binding = *task.checkpoint_binding;
        writer.raw(binding.closure_.bytes);
        writer.raw(binding.positions_.bytes);
        writer.raw(binding.revision_.bytes);
        writer.raw(task.task_id.bytes);
        writer.u32(task.root.value);
        writer.u8(static_cast<std::uint8_t>(task.kind));
        writer.boolean(task.shortest);
        writer.u8(static_cast<std::uint8_t>(task.progress));
        writer.boolean(task.seeded);
        writer.boolean(task.active);
        writer.boolean(task.recursive);
        writer.boolean(task.all_sources);
        writer.u32(task.issues);
        auto stats = task.stats;
        query_counters(stats, [&](std::size_t value) { writer.u64(value); });
        writer.u64(task.cursor);
        writer.u8(static_cast<std::uint8_t>(task.phase));
        writer.u32(task.frame);
        writer.u32(task.summary_target.value);
        writer.u64(task.edge_cursor);
        writer.u64(task.witness_cursor == no_predecessor ? UINT64_MAX
                                                         : task.witness_cursor);
        writer.u64(task.delivered_sources);
        writer.u64(task.delivered_witness);
        writer.u64(task.sequence);
        writer.u64(task.acknowledged);
        writer.boolean(task.unconfirmed.has_value());
        if (task.unconfirmed) {
            writer.u64(task.unconfirmed->source_begin);
            writer.u64(task.unconfirmed->witness_begin);
            writer.u8(static_cast<std::uint8_t>(task.unconfirmed->progress));
        }
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
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    }
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
            saved_root != root.value ||
            saved_kind != static_cast<unsigned>(kind) ||
            saved_witness != witness)
            return std::unexpected(
                CheckpointError{CheckpointErrorCode::BindingMismatch, {}});
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

        // Validate every index before indexing/rebinding. No graph traversal is
        // replayed: only context calls, reported sources and the active lease
        // need cold detail validation. The frontier remains lazy.
        require_checkpoint(task.progress >= QueryProgress::Running &&
                           task.progress <= QueryProgress::Completed &&
                           task.phase >= Impl::Phase::Parameter &&
                           task.phase <= Impl::Phase::Witness &&
                           task.phase != Impl::Phase::Formula &&
                           (task.issues & ~std::uint32_t{31}) == 0 &&
                           !task.contexts.empty() &&
                           task.contexts[0] == Invocation{});
        const auto valid_node = [&](NodeId node) {
            return node.value && node.value <= task.evidence->node_count();
        };
        const auto valid_state = [&](QueryState state) {
            return valid_node(state.node) &&
                   state.context < task.contexts.size();
        };
        for (std::size_t i = 1; i < task.contexts.size(); ++i) {
            const auto& context = task.contexts[i];
            require_checkpoint(valid_node(context.call) && context.parent < i &&
                               context.owner.value &&
                               context.owner.value <=
                                   task.evidence->summaries().size());
            const auto call = task.pin(context.call);
            require_checkpoint(call.node().rule == Rule::Call &&
                               call.node().payload == context.owner.value);
            require_checkpoint(
                task.context_ids
                    .try_emplace(state_key({context.call, context.parent}),
                                 static_cast<std::uint32_t>(i))
                    .second);
        }
        require_checkpoint(
            task.cursor <= task.queue.size() &&
            (!task.active || task.cursor < task.queue.size()) &&
            task.frame < task.contexts.size() &&
            task.stats.admitted_states == task.queue.size() &&
            task.stats.visited_states ==
                task.cursor + (task.active ? 1U : 0U) &&
            task.stats.visited_edges <= task.stats.scanned_edges &&
            task.stats.queue_peak <= task.queue.size() &&
            task.stats.bindings >= task.contexts.size() - 1 &&
            !task.stats.formula_uses && !task.stats.formula_fallbacks);
        for (std::size_t i = 0; i < task.queue.size(); ++i) {
            const auto& visit = task.queue[i];
            require_checkpoint(
                valid_state(visit.state) &&
                (i == 0 ? visit.predecessor == no_predecessor &&
                              visit.state == QueryState{root, 0}
                        : visit.predecessor < i &&
                              visit.predecessor < task.stats.visited_states));
            require_checkpoint(
                task.visited.try_emplace(state_key(visit.state), i).second);
        }
        require_checkpoint(task.seeded == !task.queue.empty());
        if (!task.seeded)
            require_checkpoint(!task.active && task.contexts.size() == 1 &&
                               task.phase == Impl::Phase::Done &&
                               !task.all_sources && !task.issues);
        if (!task.active)
            require_checkpoint(task.phase == Impl::Phase::Done);
        else {
            task.current_lease = task.pin(task.queue[task.cursor].state.node);
            const auto& current = task.current_lease->node();
            require_checkpoint(task.edge_cursor <=
                               task.current_lease->edges().size());
            switch (task.phase) {
            case Impl::Phase::Parameter:
            case Impl::Phase::ParameterEdge:
                require_checkpoint(current.rule == Rule::Parameter);
                if (task.phase == Impl::Phase::ParameterEdge)
                    require_checkpoint(task.frame &&
                                       task.contexts[task.frame].owner ==
                                           current.owner);
                break;
            case Impl::Phase::CallSearch:
            case Impl::Phase::CallFrame:
            case Impl::Phase::Summary: {
                require_checkpoint(current.rule == Rule::Call ||
                                   current.rule == Rule::SummaryRef);
                require_checkpoint(task.edge_cursor == 0);
                require_checkpoint(current.payload &&
                                   current.payload <=
                                       task.evidence->summaries().size());
                const auto& summary = task.evidence->summary({current.payload});
                require_checkpoint(task.summary_target ==
                                   (kind == QueryKind::Value
                                        ? summary.value
                                        : summary.effects));
                break;
            }
            case Impl::Phase::Edges:
                require_checkpoint(task.edge_cursor <=
                                   task.current_lease->edges().size());
                break;
            case Impl::Phase::Witness:
                require_checkpoint(witness && task.sources.size() == 1);
                break;
            case Impl::Phase::Done:
                break;
            case Impl::Phase::Formula:
                require_checkpoint(false);
                break;
            }
        }
        std::size_t previous_source = no_predecessor;
        for (const auto& source : task.sources) {
            require_checkpoint(valid_state({source.node, source.context}));
            const auto found =
                task.visited.find(state_key({source.node, source.context}));
            require_checkpoint(found != task.visited.end() &&
                               found->second < task.stats.visited_states &&
                               (previous_source == no_predecessor ||
                                previous_source < found->second));
            previous_source = found->second;
            const auto lease = task.pin(source.node);
            require_checkpoint(
                lease.node().site == source.site &&
                (kind == QueryKind::Value
                     ? lease.node().rule == Rule::Origin ||
                           lease.node().rule == Rule::RuntimeBoundary
                     : lease.node().rule == Rule::Effect));
        }
        require_checkpoint(task.stats.witness_steps ==
                           task.reversed_witness.size());
        if (task.phase == Impl::Phase::Witness) {
            auto predecessor_index = task.cursor;
            require_checkpoint(previous_source == task.cursor);
            for (const auto& state : task.reversed_witness) {
                require_checkpoint(predecessor_index != no_predecessor &&
                                   task.queue[predecessor_index].state ==
                                       state);
                predecessor_index = task.queue[predecessor_index].predecessor;
            }
            require_checkpoint(task.witness_cursor == predecessor_index &&
                               !task.all_sources &&
                               (task.progress != QueryProgress::Completed ||
                                predecessor_index == no_predecessor));
        } else
            require_checkpoint(task.reversed_witness.empty() &&
                               task.witness_cursor == no_predecessor);
        if (task.all_sources)
            require_checkpoint(task.progress == QueryProgress::Completed &&
                               !task.active &&
                               task.cursor == task.queue.size());
        if (task.progress == QueryProgress::Completed)
            require_checkpoint(task.all_sources ||
                               task.phase == Impl::Phase::Witness);
        require_checkpoint(task.delivered_sources <= task.sources.size() &&
                           task.delivered_witness <=
                               task.reversed_witness.size() &&
                           (!task.delivered_witness ||
                            task.progress == QueryProgress::Completed) &&
                           task.sequence < UINT64_MAX);
        if (task.unconfirmed) {
            require_checkpoint(
                task.acknowledged < UINT64_MAX &&
                task.sequence == task.acknowledged + 1 &&
                task.unconfirmed->source_begin <= task.delivered_sources &&
                task.unconfirmed->witness_begin <= task.delivered_witness &&
                (task.unconfirmed->source_begin < task.delivered_sources ||
                 task.unconfirmed->witness_begin < task.delivered_witness) &&
                task.unconfirmed->progress >= QueryProgress::Running &&
                task.unconfirmed->progress <= QueryProgress::Completed);
        } else
            require_checkpoint(task.sequence == task.acknowledged);
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
