#include "pagos/stage/provenance_topology.h"
#include "../cache/wire.h"
#include "pagos/stage/provenance_snapshot.h"

#include <algorithm>
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
// QueryTaskManifest v3 is the symbolic topology task. Native query manifests
// retain versions 1 (resident) and 2 (paged); tasks cannot change kind in
// place.
constexpr std::uint32_t topology_manifest_version = 3;
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
    static void identity(cache::wire::Reader& reader,
                         const QueryCheckpointBinding& binding,
                         cache::Digest task, NodeId root) {
        cache::wire::header(reader, cache::ObjectKind::QueryTaskManifest);
        if (reader.u32() != topology_manifest_version ||
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
std::expected<cache::Bytes, CheckpointError>
TopologyTask::checkpoint(cache::Limits limits) const {
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
        cache::wire::header(writer, cache::ObjectKind::QueryTaskManifest);
        writer.u32(topology_manifest_version);
        writer.u32(topology_engine_version);
        writer.u32(provenance_record_model);
        writer.raw(task.binding->closure_.bytes);
        writer.raw(task.binding->positions_.bytes);
        writer.raw(task.binding->revision_.bytes);
        writer.raw(task.task_id.bytes);
        writer.u32(task.root.value);
        writer.u8(static_cast<std::uint8_t>(task.progress));
        writer.u64(task.sequence);
        writer.u64(task.acknowledged);
        auto stats = task.index->stats_;
        counters(stats, [&](std::size_t value) { writer.u64(value); });
        writer.u64(task.cursor);
        writer.u64(task.edge_cursor);
        writer.u64(task.link_cursor);
        writer.boolean(task.active);
        writer.boolean(task.preparing);
        writer.u64(task.formula_cursor);
        writer.u64(task.local_cursor);
        writer.u64(task.local_edge);
        writer.boolean(task.local_active);
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
std::expected<TopologyTask, CheckpointError>
TopologyTask::restore(QueryCheckpointBinding binding, cache::Digest task_id,
                      NodeId root, std::span<const std::uint8_t> bytes,
                      cache::Limits limits) {
    try {
        cache::wire::Reader reader(verified(bytes, limits), limits);
        Impl::identity(reader, binding, task_id, root);
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
            Impl::identity(reader, *impl_->binding, impl_->task_id,
                           impl_->root);
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
    auto task = restore(std::move(binding), store.task(), root, *bytes, limits);
    if (!task)
        return task;
    if (status(task->progress(), task->impl_->unconfirmed.has_value()) !=
        head.status)
        return std::unexpected(
            CheckpointError{CheckpointErrorCode::Corrupt, {}});
    return task;
}
} // namespace pagos::stage::provenance
