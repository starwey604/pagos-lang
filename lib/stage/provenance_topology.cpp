#include "pagos/stage/provenance_topology.h"
#include "../cache/wire.h"
#include "pagos/stage/provenance_snapshot.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace pagos::stage::provenance {
namespace {
struct ReadFailure {
    EvidenceError error;
};
NodeLease pin(const EvidenceView& view, NodeId node) {
    auto lease = view.pin(node);
    if (!lease)
        throw ReadFailure{lease.error()};
    return std::move(*lease);
}
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
// Symbolic topology uses v3 resident and v4 segmented manifests. Native queries
// retain v1/v2; a task cannot change between native and symbolic query kinds.
constexpr std::uint32_t topology_manifest_version = 3;
constexpr std::uint32_t topology_segmented_version = 4;
CheckpointError codec_error(cache::Error error) {
    return {error == cache::Error::IncompatibleVersion
                ? CheckpointErrorCode::IncompatibleVersion
            : error == cache::Error::LimitExceeded
                ? CheckpointErrorCode::LimitExceeded
                : CheckpointErrorCode::Corrupt,
            {}};
}
CheckpointError store_error(cache::StoreError error) {
    return {error.code == cache::StoreErrorCode::LimitExceeded
                ? CheckpointErrorCode::LimitExceeded
                : CheckpointErrorCode::StorageFailure,
            {},
            error};
}
void require(bool condition) {
    if (!condition)
        throw CheckpointError{CheckpointErrorCode::Corrupt, {}};
}
void finish(cache::wire::Reader& reader) {
    if (auto result = reader.finish(); !result)
        throw codec_error(result.error());
}
cache::Digest digest(cache::wire::Reader& reader) {
    cache::Digest result;
    auto bytes = reader.raw(32);
    if (reader.good())
        std::ranges::copy(bytes, result.bytes.begin());
    return result;
}
std::size_t size(cache::wire::Reader& reader) {
    const auto value = reader.u64();
    if (value > std::numeric_limits<std::size_t>::max())
        throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
    return static_cast<std::size_t>(value);
}
template <class F> void counters(TopologyStats& stats, F f) {
    f(stats.nodes);
    f(stats.edges);
    f(stats.summary_links);
    f(stats.formulas);
    f(stats.formula_terms);
    f(stats.formula_states);
    f(stats.formula_edges);
    f(stats.formula_reuses);
    f(stats.source_families);
}
cache::TaskStatus status(QueryProgress progress, bool pending) {
    return progress == QueryProgress::Completed
               ? (pending ? cache::TaskStatus::AwaitingDelivery
                          : cache::TaskStatus::Completed)
           : progress == QueryProgress::Cancelled ? cache::TaskStatus::Cancelled
                                                  : cache::TaskStatus::Paused;
}
std::span<const std::uint8_t> verified(std::span<const std::uint8_t> bytes,
                                       cache::Limits limits) {
    if (bytes.size() > limits.bytes)
        throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
    require(bytes.size() >= 32);
    const auto payload = bytes.first(bytes.size() - 32);
    const auto hash = cache::fingerprint("pagos.topology-task.v1", payload);
    require(std::ranges::equal(hash.bytes, bytes.last(32)));
    return payload;
}
} // namespace
FormulaIndex::FormulaIndex(std::shared_ptr<const EvidenceView> view)
    : evidence_(std::move(view)) {}
const FrozenGraph& FormulaIndex::graph() const {
    if (auto* graph = evidence_->memory_graph())
        return *graph;
    throw std::logic_error("disk formulas have no resident FrozenGraph");
}
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
    auto task = QueryTask(evidence_, root, kind);
    task.formulas(shared_from_this());
    return task;
}
QueryTask FormulaIndex::begin_invocation(std::span<const NodeId> calls,
                                         QueryKind kind, bool witness) const
    try {
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
                    const auto lease = pin(*evidence_, term);
                    const auto& node = lease.node();
                    if (node.rule == Rule::SummaryRef && node.payload &&
                        node.payload <= evidence_->summaries().size()) {
                        const auto& summary =
                            evidence_->summary({node.payload});
                        pending.push_back(kind == QueryKind::Value
                                              ? summary.value
                                              : summary.effects);
                    }
                }
            }
            if (!found)
                throw std::invalid_argument("disconnected invocation path");
        }
        const auto lease = pin(*evidence_, call);
        const auto& node = lease.node();
        if (!owners.insert(node.payload).second)
            throw std::invalid_argument(
                "recursive invocation path requires recursive solver");
        const auto& summary = evidence_->summary({node.payload});
        root = kind == QueryKind::Value ? summary.value : summary.effects;
    }
    auto task = QueryTask(evidence_, calls.back(), kind, witness);
    task.invocation_path(calls.first(calls.size() - 1));
    if (!witness)
        task.formulas(shared_from_this());
    return task;
} catch (const ReadFailure&) {
    throw std::runtime_error("invocation evidence unavailable");
}
struct TopologyTask::Impl {
    Impl(std::shared_ptr<FormulaIndex> inventory, NodeId root)
        : index(std::move(inventory)), root(root) {
        if (!index->evidence_ || !root.value ||
            root.value > index->evidence_->node_count())
            throw std::out_of_range("invalid topology root");
        enqueue(root);
        add_formula(root, QueryKind::Value);
        add_formula(root, QueryKind::Effects);
    }
    std::shared_ptr<FormulaIndex> index;
    NodeId root;
    std::optional<NodeLease> lease;
    std::optional<EvidenceError> failure;
    std::optional<QueryCheckpointBinding> binding;
    cache::Digest task_id;
    std::uint64_t acknowledged{};
    std::optional<TopologyPage> unconfirmed;
    struct Offloaded {
        cache::TaskStore store;
        cache::TaskHead head;
        cache::Limits limits;
        QueryProgress resume_progress;
        std::weak_ptr<const FormulaIndex> retained;
    };
    std::optional<Offloaded> offloaded;
    std::optional<CheckpointError> storage_error;
    TopologyStorageStats io;
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
    void add_formula(NodeId entry, QueryKind kind) {
        if (index->formula(entry, kind)) {
            ++index->stats_.formula_reuses;
            return;
        }
        index->formula_ids_.emplace(formula_key(entry, kind),
                                    index->formulas_.size());
        index->formulas_.push_back({entry, kind, {}, {}});
    }
    bool step(QuerySlice& slice, std::vector<TopologyRecord>& output) {
        auto& stats = index->stats_;
        const auto& graph = *index->evidence_;
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
                auto loaded = pin(graph, node);
                output.push_back(
                    {TopologyRecordKind::Node, node, {}, {}, QueryKind::Value});
                ++stats.nodes;
                lease = std::move(loaded);
                if (lease->node().rule == Rule::Call)
                    index->calls_.push_back(node);
                active = true;
                edge_cursor = 0;
                link_cursor = 0;
                return true;
            }
            const auto node = queue[cursor];
            const auto edges = lease->edges();
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
                if (lease->node().rule == Rule::Call) {
                    add_formula(edge.target, QueryKind::Value);
                    add_formula(edge.target, QueryKind::Effects);
                }
                return true;
            }
            const auto& record = lease->node();
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
            lease.reset();
            edge_cursor = link_cursor = 0;
            ++cursor;
            return true;
        }
        if (formula_cursor == index->formulas_.size()) {
            stats.formulas = index->formulas_.size();
            stats.source_families = families.size();
            capacity();
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

                ++formula_cursor;
                local_queue.clear();
                local_seen.clear();
                local_cursor = local_edge = 0;
                return true;
            }
            if (!slice.states)
                return false;
            --slice.states;
            const auto id = local_queue[local_cursor];
            lease = pin(graph, id);
            ++stats.formula_states;
            const auto& node = lease->node();
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
                local_edge = lease->edges().size();
            return true;
        }
        const auto edges = lease->edges();
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
        lease.reset();
        local_edge = 0;
        ++local_cursor;
        return true;
    }
    void capacity() const {
        auto& bytes = index->stats_.formula_capacity_bytes;
        bytes = index->formulas_.capacity() * sizeof(DependencyFormula) +
                index->calls_.capacity() * sizeof(NodeId);
        for (const auto& formula : index->formulas_)
            bytes += formula.terms.capacity() * sizeof(NodeId);
    }
    void validate();
    void write_control(cache::wire::Writer& writer,
                       std::uint32_t version) const;
    static TopologyTask read_control(QueryCheckpointBinding binding,
                                     cache::Digest task_id, NodeId root,
                                     cache::wire::Reader& reader,
                                     std::uint32_t version);
    static void identity(cache::wire::Reader& reader,
                         const QueryCheckpointBinding& binding,
                         cache::Digest task, NodeId root,
                         std::uint32_t version = topology_manifest_version) {
        cache::wire::header(reader, cache::ObjectKind::QueryTaskManifest);
        if (reader.u32() != version ||
            reader.u32() != topology_engine_version ||
            reader.u32() != provenance_record_model)
            reader.fail(cache::Error::IncompatibleVersion);
        if (!reader.good())
            finish(reader);
        const auto closure = digest(reader), positions = digest(reader),
                   revision = digest(reader), task_id = digest(reader);
        const auto entry = reader.u32();
        if (!reader.good())
            finish(reader);
        if (closure != binding.closure_ || positions != binding.positions_ ||
            revision != binding.revision_ || task_id != task ||
            entry != root.value)
            throw CheckpointError{CheckpointErrorCode::BindingMismatch, {}};
        require(binding.view_ && root.value &&
                root.value <= binding.view_->node_count());
    }
};
TopologyTask::TopologyTask(FrozenGraph graph, NodeId root)
    : TopologyTask(EvidenceView::memory(std::move(graph)), root) {}
TopologyTask::TopologyTask(std::shared_ptr<const EvidenceView> view,
                           NodeId root)
    : impl_(std::make_unique<Impl>(
          std::shared_ptr<FormulaIndex>(new FormulaIndex(std::move(view))),
          root)) {}
TopologyTask::TopologyTask(QueryCheckpointBinding binding, cache::Digest task,
                           NodeId root)
    : TopologyTask(binding.view_, root) {
    impl_->binding = std::move(binding);
    impl_->task_id = task;
}
TopologyTask FrozenGraph::begin_topology(NodeId root) const {
    return TopologyTask(*this, root);
}
TopologyTask::~TopologyTask() = default;
TopologyTask::TopologyTask(TopologyTask&&) noexcept = default;
TopologyTask& TopologyTask::operator=(TopologyTask&&) noexcept = default;
QueryProgress TopologyTask::progress() const { return impl_->progress; }
const FrozenGraph& TopologyTask::graph() const { return impl_->index->graph(); }
const TopologyStats& TopologyTask::stats() const {
    return impl_->index->stats_;
}
std::shared_ptr<const FormulaIndex> TopologyTask::index() const {
    if (!ensure_resident())
        throw std::runtime_error("topology state unavailable");
    return progress() == QueryProgress::Completed ? impl_->index : nullptr;
}
void TopologyTask::cancel() {
    if (impl_->offloaded) {
        auto& logical = impl_->offloaded->resume_progress;
        if (logical == QueryProgress::Running ||
            logical == QueryProgress::Paused)
            impl_->progress = logical = QueryProgress::Cancelled;
        return;
    }
    if (progress() == QueryProgress::Running ||
        progress() == QueryProgress::Paused)
        impl_->progress = QueryProgress::Cancelled;
}
void TopologyTask::resume() {
    if (impl_->offloaded) {
        auto& logical = impl_->offloaded->resume_progress;
        if (logical == QueryProgress::Paused ||
            logical == QueryProgress::Cancelled)
            logical = QueryProgress::Running;
        impl_->progress = logical;
        return;
    }
    if (progress() == QueryProgress::Paused ||
        progress() == QueryProgress::Cancelled)
        impl_->progress = QueryProgress::Running;
}
TopologyPage TopologyTask::advance(QuerySlice slice,
                                   std::stop_token cancellation) {
    if (auto ready = ensure_resident(); !ready) {
        TopologyPage page;
        page.task = impl_->task_id;
        page.progress = impl_->progress;
        page.failure = EvidenceError{
            EvidenceErrorCode::IoFailure, ready.error().storage, {}};
        return page;
    }
    if (impl_->unconfirmed) {
        if (cancellation.stop_requested())
            cancel();
        return *impl_->unconfirmed;
    }
    if (progress() == QueryProgress::Paused)
        resume();
    TopologyPage page;
    page.task = impl_->binding ? std::optional(impl_->task_id) : std::nullopt;
    if (progress() == QueryProgress::Running) {
        try {
            while (!cancellation.stop_requested() &&
                   impl_->step(slice, page.records)) {
            }
            if (cancellation.stop_requested())
                cancel();
            else if (progress() == QueryProgress::Running)
                impl_->progress = QueryProgress::Paused;
        } catch (const ReadFailure& error) {
            impl_->failure = error.error;
            impl_->progress = QueryProgress::Failed;
        } catch (const std::bad_alloc&) {
            impl_->progress = QueryProgress::Failed;
        } catch (const std::length_error&) {
            impl_->progress = QueryProgress::Failed;
        }
    }
    page.progress = progress();
    page.failure = impl_->failure;
    if (!page.records.empty()) {
        if (impl_->sequence == UINT64_MAX) {
            impl_->progress = page.progress = QueryProgress::Failed;
            page.records.clear();
            return page;
        }
        page.sequence = ++impl_->sequence;
        if (impl_->binding && page.progress != QueryProgress::Failed) {
            try {
                impl_->unconfirmed = page;
            } catch (const std::bad_alloc&) {
                impl_->progress = page.progress = QueryProgress::Failed;
                page.records.clear();
                page.sequence = 0;
            }
        }
    }
    return page;
}
bool TopologyTask::acknowledge(std::uint64_t sequence) {
    if (!ensure_resident())
        return false;
    auto& task = *impl_;
    if (!task.binding)
        return false;
    if (sequence <= task.acknowledged)
        return true;
    if (!task.unconfirmed || sequence != task.sequence)
        return false;
    task.acknowledged = sequence;
    task.unconfirmed.reset();
    return true;
}
void TopologyTask::Impl::write_control(cache::wire::Writer& writer,
                                       std::uint32_t version) const {
    cache::wire::header(writer, cache::ObjectKind::QueryTaskManifest);
    writer.u32(version);
    writer.u32(topology_engine_version);
    writer.u32(provenance_record_model);
    writer.raw(binding->closure_.bytes);
    writer.raw(binding->positions_.bytes);
    writer.raw(binding->revision_.bytes);
    writer.raw(task_id.bytes);
    writer.u32(root.value);
    writer.u8(static_cast<std::uint8_t>(progress));
    writer.u64(sequence);
    writer.u64(acknowledged);
    auto stats = index->stats_;
    counters(stats, [&](std::size_t value) { writer.u64(value); });
    writer.u64(cursor);
    writer.u64(edge_cursor);
    writer.u64(link_cursor);
    writer.boolean(active);
    writer.boolean(preparing);
    writer.u64(formula_cursor);
    writer.u64(local_cursor);
    writer.u64(local_edge);
    writer.boolean(local_active);
}
std::expected<cache::Bytes, CheckpointError>
TopologyTask::checkpoint(cache::Limits limits) const {
    if (auto ready = ensure_resident(); !ready)
        return std::unexpected(ready.error());
    const auto& task = *impl_;
    if (!task.binding || task.progress == QueryProgress::Failed)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::InvalidState, {}});
    try {
        if (limits.bytes < 32)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        auto bounds = limits;
        bounds.bytes -= 32;
        cache::wire::Writer writer(bounds);
        task.write_control(writer, topology_manifest_version);
        const auto nodes = [&](const auto& values) {
            writer.count(values.size());
            for (auto value : values)
                writer.u32(value.value);
        };
        nodes(task.queue);
        writer.count(task.index->formulas_.size());
        for (const auto& formula : task.index->formulas_) {
            writer.u32(formula.root.value);
            writer.u8(static_cast<std::uint8_t>(formula.kind));
            if (formula.fallback != "" &&
                formula.fallback != "unsupported rule" &&
                formula.fallback != "unsupported relation")
                throw CheckpointError{CheckpointErrorCode::InvalidState, {}};
            writer.u8(formula.fallback.empty()                 ? 0
                      : formula.fallback == "unsupported rule" ? 1
                                                               : 2);
            nodes(formula.terms);
        }
        nodes(task.index->calls_);
        nodes(task.local_queue);
        writer.boolean(task.unconfirmed.has_value());
        if (task.unconfirmed) {
            writer.u8(static_cast<std::uint8_t>(task.unconfirmed->progress));
            writer.count(task.unconfirmed->records.size());
            for (const auto& record : task.unconfirmed->records) {
                writer.u8(static_cast<std::uint8_t>(record.kind));
                writer.u32(record.node.value);
                writer.u32(record.edge.target.value);
                writer.u8(static_cast<std::uint8_t>(record.edge.relation));
                writer.u32(record.summary.value);
                writer.u8(static_cast<std::uint8_t>(record.port));
            }
        }
        auto bytes = writer.finish();
        if (!bytes)
            throw codec_error(bytes.error());
        const auto checksum =
            cache::fingerprint("pagos.topology-task.v1", *bytes);
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
TopologyTask TopologyTask::Impl::read_control(QueryCheckpointBinding binding,
                                              cache::Digest task_id,
                                              NodeId root,
                                              cache::wire::Reader& reader,
                                              std::uint32_t version) {
    Impl::identity(reader, binding, task_id, root, version);
    TopologyTask result(std::move(binding), task_id, root);
    auto& task = *result.impl_;
    task.queue.clear();
    task.visited.clear();
    task.index->formulas_.clear();
    task.index->formula_ids_.clear();
    task.progress = static_cast<QueryProgress>(reader.u8());
    task.sequence = reader.u64();
    task.acknowledged = reader.u64();
    counters(task.index->stats_,
             [&](std::size_t& value) { value = size(reader); });
    task.cursor = size(reader);
    task.edge_cursor = size(reader);
    task.link_cursor = size(reader);
    task.active = reader.boolean();
    task.preparing = reader.boolean();
    task.formula_cursor = size(reader);
    task.local_cursor = size(reader);
    task.local_edge = size(reader);
    task.local_active = reader.boolean();
    if (!reader.good())
        finish(reader);
    return result;
}
std::expected<TopologyTask, CheckpointError>
TopologyTask::restore(QueryCheckpointBinding binding, cache::Digest task_id,
                      NodeId root, std::span<const std::uint8_t> bytes,
                      cache::Limits limits) {
    try {
        cache::wire::Reader reader(verified(bytes, limits), limits);
        auto result = Impl::read_control(std::move(binding), task_id, root,
                                         reader, topology_manifest_version);
        auto& task = *result.impl_;
        const auto nodes = [&](auto& values) {
            const auto count = reader.count(4);
            for (std::uint32_t i = 0; i < count && reader.good(); ++i)
                values.push_back({reader.u32()});
        };
        nodes(task.queue);
        const auto count = reader.count(10);
        for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
            DependencyFormula formula{
                {reader.u32()}, static_cast<QueryKind>(reader.u8()), {}, {}};
            const auto fallback = reader.u8();
            require(fallback <= 2);
            formula.fallback = fallback == 0   ? ""
                               : fallback == 1 ? "unsupported rule"
                                               : "unsupported relation";
            nodes(formula.terms);
            task.index->formulas_.push_back(std::move(formula));
        }
        nodes(task.index->calls_);
        nodes(task.local_queue);
        if (reader.boolean()) {
            TopologyPage page;
            page.task = task_id;
            page.sequence = task.sequence;
            page.progress = static_cast<QueryProgress>(reader.u8());
            const auto records = reader.count(15);
            for (std::uint32_t i = 0; i < records && reader.good(); ++i) {
                TopologyRecord record;
                record.kind = static_cast<TopologyRecordKind>(reader.u8());
                record.node = {reader.u32()};
                record.edge.target = {reader.u32()};
                record.edge.relation = static_cast<Relation>(reader.u8());
                record.summary = {reader.u32()};
                record.port = static_cast<QueryKind>(reader.u8());
                page.records.push_back(record);
            }
            task.unconfirmed = std::move(page);
        }
        finish(reader);
        task.validate();
        if (task.progress == QueryProgress::Completed)
            task.capacity();
        return result;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const ReadFailure& error) {
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
void TopologyTask::Impl::validate() {
    const auto& view = *index->evidence_;
    auto& stats = index->stats_;
    const auto valid_node = [&](NodeId id) {
        return id.value && id.value <= view.node_count();
    };
    require(progress >= QueryProgress::Running &&
            progress <= QueryProgress::Completed && !queue.empty() &&
            queue[0] == root && cursor <= queue.size() &&
            (!active || cursor < queue.size()) &&
            stats.nodes == cursor + (active ? 1U : 0U) &&
            !index->formulas_.empty() &&
            formula_cursor <= index->formulas_.size());
    for (const auto id : queue)
        require(valid_node(id) && visited.insert(id.value).second);
    std::size_t edges = 0, links = 0;
    std::vector<NodeId> calls;
    for (std::size_t i = 0; i < stats.nodes; ++i) {
        const auto record = pin(view, queue[i]);
        if (record.node().rule == Rule::Call)
            calls.push_back(queue[i]);
        const auto count = i < cursor ? record.edges().size() : edge_cursor;
        require(count <= record.edges().size() && count <= SIZE_MAX - edges);
        edges += count;
        const auto& node = record.node();
        const bool linked =
            (node.rule == Rule::Call || node.rule == Rule::SummaryRef) &&
            node.payload && node.payload <= view.summaries().size();
        const auto ports = i < cursor ? (linked ? 2U : 0U) : link_cursor;
        require(ports <= 2 &&
                (!ports || (linked && count == record.edges().size())) &&
                ports <= SIZE_MAX - links);
        links += ports;
        if (i == cursor && active)
            lease = record;
    }
    require(edges == stats.edges && links == stats.summary_links &&
            calls == index->calls_ &&
            (active || (!edge_cursor && !link_cursor)));
    if (!preparing)
        require(!local_active && local_queue.empty() && !formula_cursor &&
                !local_cursor && !local_edge && !stats.formula_states &&
                !stats.formula_edges);
    else
        require(!active && cursor == queue.size());
    std::size_t terms = 0;
    for (std::size_t i = 0; i < index->formulas_.size(); ++i) {
        const auto& formula = index->formulas_[i];
        require(valid_node(formula.root) &&
                visited.contains(formula.root.value) &&
                (formula.kind == QueryKind::Value ||
                 formula.kind == QueryKind::Effects) &&
                index->formula_ids_
                    .emplace(formula_key(formula.root, formula.kind), i)
                    .second);
        if (i < formula_cursor) {
            require(formula.terms.size() <= SIZE_MAX - terms);
            terms += formula.terms.size();
        }
        if (i > formula_cursor || !preparing)
            require(formula.terms.empty() && formula.fallback.empty());
        std::unordered_set<std::uint32_t> seen;
        for (const auto term : formula.terms) {
            require(valid_node(term) && visited.contains(term.value) &&
                    seen.insert(term.value).second);
            const auto record = pin(view, term);
            require(terminal(record.node().rule, formula.kind));
            if (formula.kind == QueryKind::Value
                    ? record.node().rule == Rule::Origin ||
                          record.node().rule == Rule::RuntimeBoundary
                    : record.node().rule == Rule::Effect)
                families.insert(formula_key(term, formula.kind));
        }
    }
    require(index->formula(root, QueryKind::Value) &&
            index->formula(root, QueryKind::Effects) &&
            terms == stats.formula_terms &&
            stats.formula_states >= formula_cursor &&
            local_cursor <= local_queue.size() &&
            (!local_active || local_cursor < local_queue.size()));
    for (auto id : local_queue)
        require(valid_node(id) && visited.contains(id.value) &&
                local_seen.insert(id.value).second);
    if (!local_queue.empty()) {
        require(preparing && formula_cursor < index->formulas_.size() &&
                local_queue[0] == index->formulas_[formula_cursor].root);
        if (local_active) {
            lease = pin(view, local_queue[local_cursor]);
            require(local_edge <= lease->edges().size());
        } else
            require(!local_edge);
    } else
        require(!local_active && !local_cursor && !local_edge);
    if (progress == QueryProgress::Completed)
        require(preparing && formula_cursor == index->formulas_.size() &&
                local_queue.empty() &&
                stats.formulas == index->formulas_.size() &&
                stats.source_families == families.size());
    else
        require(!stats.formulas && !stats.source_families);
    require(sequence < UINT64_MAX);
    if (unconfirmed) {
        require(!unconfirmed->records.empty() && acknowledged < UINT64_MAX &&
                sequence == acknowledged + 1 &&
                unconfirmed->progress >= QueryProgress::Running &&
                unconfirmed->progress <= QueryProgress::Completed);
        for (const auto& item : unconfirmed->records) {
            require(valid_node(item.node) && visited.contains(item.node.value));
            const auto record = pin(view, item.node);
            switch (item.kind) {
            case TopologyRecordKind::Node:
                require(item.edge == Edge{} && item.summary == SummaryId{} &&
                        item.port == QueryKind::Value);
                break;
            case TopologyRecordKind::Edge:
                require(item.summary == SummaryId{} &&
                        item.port == QueryKind::Value &&
                        std::ranges::find(record.edges(), item.edge) !=
                            record.edges().end());
                break;
            case TopologyRecordKind::SummaryLink: {
                require((record.node().rule == Rule::Call ||
                         record.node().rule == Rule::SummaryRef) &&
                        item.summary.value &&
                        item.summary.value == record.node().payload &&
                        item.summary.value <= view.summaries().size() &&
                        (item.port == QueryKind::Value ||
                         item.port == QueryKind::Effects) &&
                        item.edge.relation == Relation::Data);
                const auto& summary = view.summary(item.summary);
                require(item.edge.target == (item.port == QueryKind::Value
                                                 ? summary.value
                                                 : summary.effects));
                break;
            }
            default:
                require(false);
            }
        }
    } else
        require(sequence == acknowledged);
}
std::expected<cache::TaskHead, CheckpointError>
TopologyTask::save(const cache::TaskStore& store,
                   std::uint64_t expected_generation,
                   cache::Limits limits) const {
    if (store.task() != impl_->task_id)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::BindingMismatch, {}});
    auto bytes = checkpoint(limits);
    if (!bytes)
        return std::unexpected(bytes.error());
    try {
        auto writer = store.begin(expected_generation);
        if (!writer)
            throw store_error(writer.error());
        auto previous = store.head();
        if (!previous)
            throw store_error(previous.error());
        if (*previous) {
            if ((**previous).manifest.size > limits.bytes)
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            auto old = store.read((**previous).manifest);
            if (!old)
                throw store_error(old.error());
            cache::wire::Reader reader(verified(*old, limits), limits);
            cache::wire::Reader version_reader(verified(*old, limits), limits);
            cache::wire::header(version_reader,
                                cache::ObjectKind::QueryTaskManifest);
            const auto version = version_reader.u32();
            require(version == topology_manifest_version ||
                    version == topology_segmented_version);
            Impl::identity(reader, *impl_->binding, impl_->task_id, impl_->root,
                           version);
        }
        auto object = writer->write(cache::ObjectKind::QueryTaskManifest,
                                    std::move(*bytes));
        if (!object)
            throw store_error(object.error());
        auto head = writer->commit(
            *object, status(impl_->progress, impl_->unconfirmed.has_value()));
        if (!head)
            throw store_error(head.error());
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
std::expected<TopologyTask, CheckpointError> TopologyTask::restore(
    QueryCheckpointBinding binding, const cache::TaskStore& store,
    const cache::TaskHead& head, NodeId root, cache::Limits limits) {
    if (!head.generation ||
        head.manifest.kind != cache::ObjectKind::QueryTaskManifest)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::Corrupt, {}});
    if (head.manifest.size > limits.bytes)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::LimitExceeded, {}});
    auto bytes = store.read(head.manifest);
    if (!bytes)
        return std::unexpected(store_error(bytes.error()));
    cache::wire::Reader version_reader(*bytes, limits);
    cache::wire::header(version_reader, cache::ObjectKind::QueryTaskManifest);
    if (version_reader.u32() == topology_segmented_version)
        return restore_segmented(std::move(binding), store, head, root, limits,
                                 std::move(*bytes));
    auto task = restore(std::move(binding), store.task(), root, *bytes, limits);
    if (!task)
        return task;
    if (status(task->progress(), task->impl_->unconfirmed.has_value()) !=
        head.status)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::Corrupt, {}});
    return task;
}
namespace {
struct TopologySegment {
    std::uint32_t first{}, count{};
    cache::ObjectRef object;
};
constexpr std::array topology_widths{4U, 18U, 4U, 4U, 4U, 15U};
std::size_t index_bytes(const FormulaIndex& index) {
    // Public records/terms only. The task adds its private calls and hash
    // index.
    std::size_t bytes = 0;
    for (const auto& formula : index.formulas())
        bytes += formula.terms.capacity() * sizeof(NodeId);
    return bytes;
}
template <class Set> std::size_t hash_bytes(const Set& values) {
    return values.bucket_count() * sizeof(void*) +
           values.size() * sizeof(typename Set::value_type);
}
} // namespace
TopologyStateMemory TopologyTask::state_memory() const {
    const auto& task = *impl_;
    const auto index_memory = [](const FormulaIndex& index) {
        return index.formulas_.capacity() * sizeof(DependencyFormula) +
               index_bytes(index) + index.calls_.capacity() * sizeof(NodeId) +
               hash_bytes(index.formula_ids_);
    };
    TopologyStateMemory memory;
    memory.offloaded = task.offloaded.has_value();
    if (task.offloaded) {
        if (auto retained = task.offloaded->retained.lock())
            memory.retained_index = index_memory(*retained);
        return memory;
    }
    memory.queue = task.queue.capacity() * sizeof(NodeId);
    memory.visited = hash_bytes(task.visited);
    memory.formula_records =
        task.index->formulas_.capacity() * sizeof(DependencyFormula);
    memory.formula_terms = index_bytes(*task.index);
    memory.formula_index = hash_bytes(task.index->formula_ids_);
    memory.calls = task.index->calls_.capacity() * sizeof(NodeId);
    memory.local_queue = task.local_queue.capacity() * sizeof(NodeId);
    memory.local_seen = hash_bytes(task.local_seen);
    memory.families = hash_bytes(task.families);
    memory.delivery = task.unconfirmed ? task.unconfirmed->records.capacity() *
                                             sizeof(TopologyRecord)
                                       : 0;
    return memory;
}
const TopologyStorageStats& TopologyTask::storage_stats() const {
    return impl_->io;
}
const std::optional<CheckpointError>& TopologyTask::storage_failure() const {
    return impl_->storage_error;
}
std::expected<cache::TaskHead, CheckpointError> TopologyTask::save_segmented(
    const cache::TaskStore& store, std::uint64_t generation,
    std::size_t segment_bytes, cache::Limits limits) const {
    if (auto ready = ensure_resident(); !ready)
        return std::unexpected(ready.error());
    auto& task = *impl_;
    if (!task.binding || task.progress == QueryProgress::Failed)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::InvalidState, {}});
    if (store.task() != task.task_id)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::BindingMismatch, {}});
    task.io = {};
    try {
        if (segment_bytes < 72 || segment_bytes > limits.bytes ||
            limits.bytes < 32)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        std::array<std::size_t, 6> totals{
            task.queue.size(),
            task.index->formulas_.size(),
            0,
            task.index->calls_.size(),
            task.local_queue.size(),
            task.unconfirmed ? task.unconfirmed->records.size() : 0};
        for (const auto& formula : task.index->formulas_) {
            if (formula.terms.size() > limits.elements - totals[2])
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            totals[2] += formula.terms.size();
        }
        cache::wire::Writer prefix(limits);
        task.write_control(prefix, topology_segmented_version);
        prefix.boolean(task.unconfirmed.has_value());
        prefix.u8(task.unconfirmed
                      ? static_cast<std::uint8_t>(task.unconfirmed->progress)
                      : 0);
        auto control = prefix.finish();
        if (!control)
            throw codec_error(control.error());
        std::size_t metadata = control->size();
        for (unsigned table = 0; table < 6; ++table) {
            const auto capacity = (segment_bytes - 54) / topology_widths[table];
            const auto segments =
                totals[table] / capacity + (totals[table] % capacity != 0);
            if (totals[table] > limits.elements || totals[table] > UINT32_MAX ||
                limits.bytes - metadata < 8 ||
                segments > (limits.bytes - metadata - 8) / 49)
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            metadata += 8 + segments * 49;
            task.io.logical_bytes += totals[table] * topology_widths[table];
        }
        if (limits.bytes - metadata < 32)
            throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
        auto transaction = store.begin(generation);
        if (!transaction)
            throw store_error(transaction.error());
        auto old_head = store.head();
        if (!old_head)
            throw store_error(old_head.error());
        if (*old_head) {
            if ((**old_head).manifest.size > limits.bytes)
                throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
            auto old = store.read((**old_head).manifest);
            if (!old)
                throw store_error(old.error());
            task.io.bytes_read += old->size();
            task.io.peak_codec_buffers =
                std::max(task.io.peak_codec_buffers,
                         control->capacity() + old->capacity());
            const auto payload = verified(*old, limits);
            cache::wire::Reader version_reader(payload, limits);
            cache::wire::header(version_reader,
                                cache::ObjectKind::QueryTaskManifest);
            const auto version = version_reader.u32();
            if (version != topology_manifest_version &&
                version != topology_segmented_version)
                throw CheckpointError{CheckpointErrorCode::IncompatibleVersion,
                                      {}};
            cache::wire::Reader identity_reader(payload, limits);
            Impl::identity(identity_reader, *task.binding, task.task_id,
                           task.root, version);
        }
        const auto write = [&](cache::ObjectKind kind, cache::Bytes bytes) {
            task.io.bytes_submitted += bytes.size();
            task.io.peak_codec_buffers =
                std::max(task.io.peak_codec_buffers,
                         control->capacity() + bytes.capacity());
            const auto before = transaction->charged_bytes();
            auto object = transaction->write(kind, std::move(bytes));
            if (!object)
                throw store_error(object.error());
            const auto added = transaction->charged_bytes() - before;
            task.io.cas_bytes_added += added;
            if (!added)
                ++task.io.reused_objects;
            return *object;
        };
        std::array<std::vector<TopologySegment>, 6> directories;
        for (unsigned table = 0; table < 6; ++table) {
            const auto capacity = (segment_bytes - 54) / topology_widths[table];
            std::size_t first = 0, count = 0, filled = 0;
            cache::wire::Writer block(limits);
            const auto start = [&] {
                count = std::min(capacity, totals[table] - first);
                filled = 0;
                block = cache::wire::Writer(limits);
                cache::wire::header(block,
                                    cache::ObjectKind::QueryStateSegment);
                block.u32(topology_segmented_version);
                block.raw(task.task_id.bytes);
                block.u8(static_cast<std::uint8_t>(table));
                block.count(first);
                block.count(count);
            };
            if (totals[table])
                start();
            const auto emit = [&](auto encode) {
                encode(block);
                if (++filled != count)
                    return;
                auto bytes = block.finish();
                if (!bytes)
                    throw codec_error(bytes.error());
                const auto object = write(cache::ObjectKind::QueryStateSegment,
                                          std::move(*bytes));
                directories[table].push_back({static_cast<std::uint32_t>(first),
                                              static_cast<std::uint32_t>(count),
                                              object});
                ++task.io.segment_writes;
                first += count;
                if (first < totals[table])
                    start();
            };
            if (table == 0 || table == 3 || table == 4) {
                const auto& nodes = table == 0   ? task.queue
                                    : table == 3 ? task.index->calls_
                                                 : task.local_queue;
                for (auto id : nodes)
                    emit([&](auto& output) { output.u32(id.value); });
            } else if (table == 1) {
                std::size_t term_begin = 0;
                for (const auto& formula : task.index->formulas_) {
                    emit([&](auto& output) {
                        output.u32(formula.root.value);
                        output.u8(static_cast<std::uint8_t>(formula.kind));
                        if (formula.fallback != "" &&
                            formula.fallback != "unsupported rule" &&
                            formula.fallback != "unsupported relation")
                            throw CheckpointError{
                                CheckpointErrorCode::InvalidState, {}};
                        output.u8(formula.fallback.empty()                 ? 0
                                  : formula.fallback == "unsupported rule" ? 1
                                                                           : 2);
                        output.u64(term_begin);
                        output.count(formula.terms.size());
                    });
                    term_begin += formula.terms.size();
                }
            } else if (table == 2) {
                for (const auto& formula : task.index->formulas_)
                    for (auto term : formula.terms)
                        emit([&](auto& output) { output.u32(term.value); });
            } else if (task.unconfirmed) {
                for (const auto& record : task.unconfirmed->records)
                    emit([&](auto& output) {
                        output.u8(static_cast<std::uint8_t>(record.kind));
                        output.u32(record.node.value);
                        output.u32(record.edge.target.value);
                        output.u8(
                            static_cast<std::uint8_t>(record.edge.relation));
                        output.u32(record.summary.value);
                        output.u8(static_cast<std::uint8_t>(record.port));
                    });
            }
            require(first == totals[table]);
        }
        cache::wire::Writer manifest(limits);
        manifest.raw(*control);
        for (unsigned table = 0; table < 6; ++table) {
            manifest.count(totals[table]);
            manifest.count(directories[table].size());
            for (const auto& segment : directories[table]) {
                manifest.u32(segment.first);
                manifest.u32(segment.count);
                manifest.u8(static_cast<std::uint8_t>(segment.object.kind));
                manifest.raw(segment.object.digest.bytes);
                manifest.u64(segment.object.size);
            }
        }
        auto bytes = manifest.finish();
        if (!bytes)
            throw codec_error(bytes.error());
        const auto checksum =
            cache::fingerprint("pagos.topology-task.v1", *bytes);
        bytes->insert(bytes->end(), checksum.bytes.begin(),
                      checksum.bytes.end());
        auto object =
            write(cache::ObjectKind::QueryTaskManifest, std::move(*bytes));
        auto head = transaction->commit(
            object, status(task.progress, task.unconfirmed.has_value()));
        if (!head)
            throw store_error(head.error());
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
std::expected<TopologyTask, CheckpointError>
TopologyTask::restore_segmented(QueryCheckpointBinding binding,
                                const cache::TaskStore& store,
                                const cache::TaskHead& head, NodeId root,
                                cache::Limits limits, cache::Bytes bytes) {
    try {
        cache::wire::Reader reader(verified(bytes, limits), limits);
        auto result = Impl::read_control(std::move(binding), store.task(), root,
                                         reader, 4);
        auto& task = *result.impl_;
        task.io.bytes_read = bytes.size();
        task.io.peak_codec_buffers = bytes.capacity();
        const bool pending = reader.boolean();
        const auto delivery_progress = static_cast<QueryProgress>(reader.u8());
        if (pending) {
            task.unconfirmed = TopologyPage{};
            task.unconfirmed->task = store.task();
            task.unconfirmed->sequence = task.sequence;
            task.unconfirmed->progress = delivery_progress;
        } else
            require(delivery_progress == QueryProgress::Running);
        std::array<std::size_t, 6> totals;
        std::array<std::vector<TopologySegment>, 6> directories;
        for (unsigned table = 0; table < 6; ++table) {
            totals[table] = reader.count(0);
            const auto segments = reader.count(49);
            if (!reader.good())
                finish(reader);
            require(segments <= totals[table]);
            std::size_t next = 0;
            for (std::uint32_t i = 0; i < segments; ++i) {
                TopologySegment segment;
                segment.first = reader.u32();
                segment.count = reader.u32();
                segment.object = {static_cast<cache::ObjectKind>(reader.u8()),
                                  digest(reader), reader.u64()};
                if (!reader.good())
                    finish(reader);
                require(segment.first == next && segment.count &&
                        segment.count <= totals[table] - next &&
                        segment.object.kind ==
                            cache::ObjectKind::QueryStateSegment &&
                        segment.object.size ==
                            54 + std::uint64_t{segment.count} *
                                     topology_widths[table]);
                if (segment.object.size > limits.bytes)
                    throw CheckpointError{CheckpointErrorCode::LimitExceeded,
                                          {}};
                next += segment.count;
                directories[table].push_back(segment);
            }
            require(next == totals[table]);
            task.io.logical_bytes += totals[table] * topology_widths[table];
        }
        finish(reader);
        require(pending == (totals[5] != 0) &&
                status(task.progress, pending) == head.status);
        std::size_t expected_terms = 0, formula_cursor = 0, term_cursor = 0;
        for (unsigned table = 0; table < 6; ++table) {
            for (const auto& segment : directories[table]) {
                auto payload = store.read(segment.object);
                if (!payload)
                    throw store_error(payload.error());
                ++task.io.segment_reads;
                task.io.bytes_read += payload->size();
                task.io.peak_codec_buffers =
                    std::max(task.io.peak_codec_buffers,
                             bytes.capacity() + payload->capacity());
                cache::wire::Reader input(*payload, limits);
                cache::wire::header(input,
                                    cache::ObjectKind::QueryStateSegment);
                if (input.u32() != topology_segmented_version)
                    throw CheckpointError{
                        CheckpointErrorCode::IncompatibleVersion, {}};
                require(digest(input) == store.task() && input.u8() == table &&
                        input.u32() == segment.first &&
                        input.u32() == segment.count);
                for (std::uint32_t i = 0; i < segment.count && input.good();
                     ++i) {
                    if (table == 0 || table == 3 || table == 4) {
                        auto& nodes = table == 0   ? task.queue
                                      : table == 3 ? task.index->calls_
                                                   : task.local_queue;
                        nodes.push_back({input.u32()});
                    } else if (table == 1) {
                        DependencyFormula formula{
                            {input.u32()},
                            static_cast<QueryKind>(input.u8()),
                            {},
                            {}};
                        const auto fallback = input.u8();
                        require(fallback <= 2);
                        formula.fallback = fallback == 0 ? ""
                                           : fallback == 1
                                               ? "unsupported rule"
                                               : "unsupported relation";
                        const auto first = size(input);
                        const auto count = input.u32();
                        require(first == expected_terms &&
                                count <= totals[2] - expected_terms);
                        expected_terms += count;
                        formula.terms.resize(count);
                        task.index->formulas_.push_back(std::move(formula));
                    } else if (table == 2) {
                        while (formula_cursor < task.index->formulas_.size() &&
                               term_cursor ==
                                   task.index->formulas_[formula_cursor]
                                       .terms.size()) {
                            ++formula_cursor;
                            term_cursor = 0;
                        }
                        require(formula_cursor < task.index->formulas_.size());
                        task.index->formulas_[formula_cursor]
                            .terms[term_cursor++] = {input.u32()};
                    } else {
                        TopologyRecord record;
                        record.kind =
                            static_cast<TopologyRecordKind>(input.u8());
                        record.node = {input.u32()};
                        record.edge.target = {input.u32()};
                        record.edge.relation =
                            static_cast<Relation>(input.u8());
                        record.summary = {input.u32()};
                        record.port = static_cast<QueryKind>(input.u8());
                        task.unconfirmed->records.push_back(record);
                    }
                }
                finish(input);
            }
            if (table == 1)
                require(expected_terms == totals[2]);
        }
        task.validate();
        if (task.progress == QueryProgress::Completed)
            task.capacity();
        return result;
    } catch (const CheckpointError& error) {
        return std::unexpected(error);
    } catch (const ReadFailure& error) {
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
TopologyTask::offload(const cache::TaskStore& store, std::uint64_t generation,
                      std::size_t segment_bytes, cache::Limits limits) {
    if (auto ready = ensure_resident(); !ready)
        return std::unexpected(ready.error());
    auto& task = *impl_;
    try {
        auto empty = std::shared_ptr<FormulaIndex>(
            new FormulaIndex(task.index->evidence_));
        auto saved = save_segmented(store, generation, segment_bytes, limits);
        if (!saved)
            return saved;
        empty->stats_ = task.index->stats_;
        empty->stats_.formula_capacity_bytes = 0;
        task.offloaded =
            Impl::Offloaded{store, *saved, limits, task.progress, task.index};
        task.index = std::move(empty);
        decltype(task.queue){}.swap(task.queue);
        decltype(task.visited){}.swap(task.visited);
        decltype(task.local_queue){}.swap(task.local_queue);
        decltype(task.local_seen){}.swap(task.local_seen);
        decltype(task.families){}.swap(task.families);
        task.unconfirmed.reset();
        task.lease.reset();
        return saved;
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::OutOfMemory, {}});
    }
}
std::expected<void, CheckpointError> TopologyTask::ensure_resident() const {
    auto& task = *impl_;
    if (!task.offloaded)
        return {};
    const auto& state = *task.offloaded;
    auto restored = restore(*task.binding, state.store, state.head, task.root,
                            state.limits);
    if (!restored) {
        task.storage_error = restored.error();
        task.progress = state.resume_progress == QueryProgress::Cancelled
                            ? QueryProgress::Cancelled
                            : QueryProgress::Paused;
        return std::unexpected(restored.error());
    }
    const auto progress = state.resume_progress;
    task = std::move(*restored->impl_);
    task.progress = progress;
    return {};
}
} // namespace pagos::stage::provenance
