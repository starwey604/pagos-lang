#include "pagos/stage/provenance_graph.h"
#include "pagos/stage/provenance_query.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pagos::stage::provenance {
namespace {

std::uint32_t checked_id(std::size_t value) {
    // Leave the two DenseMap marker values unused as well.
    if (value >= std::numeric_limits<std::uint32_t>::max() - 1U)
        throw std::length_error("provenance ID capacity exhausted");
    return static_cast<std::uint32_t>(value);
}

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
    return hash ^ (value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U));
}

std::uint64_t key(const Node& node, std::span<const Edge> edges) {
    auto hash = mix(0, static_cast<std::uint8_t>(node.rule));
    hash = mix(hash, node.site);
    hash = mix(hash, node.payload);
    hash = mix(hash, node.owner.value);
    for (const auto& edge : edges) {
        hash = mix(hash, edge.target.value);
        hash = mix(hash, static_cast<std::uint8_t>(edge.relation));
    }
    // Reserve DenseMap's empty/tombstone markers.
    return hash & 0x7fffffffffffffffULL;
}

bool may_share(Rule rule) {
    return rule == Rule::Literal || rule == Rule::Strict ||
           rule == Rule::Aggregate || rule == Rule::Parameter;
}

} // namespace

struct GraphData {
    std::vector<Node> nodes{Node{}};
    std::vector<Edge> edges;
    std::vector<SummaryDescription> summaries{SummaryDescription{}};
    std::size_t intern_hits{};
};

struct Builder::Impl {
    explicit Impl(Storage mode) : storage(mode) {}
    Storage storage;
    std::unique_ptr<GraphData> graph{std::make_unique<GraphData>()};
    llvm::DenseMap<std::uint64_t, NodeId> intern;
    std::vector<NodeId> collision_next;
};

Builder::Builder(Storage storage) : impl_(std::make_unique<Impl>(storage)) {}
Builder::~Builder() = default;
Builder::Builder(Builder&&) noexcept = default;
Builder& Builder::operator=(Builder&&) noexcept = default;

Builder::Impl& Builder::state() {
    if (!impl_)
        throw std::logic_error("provenance Builder already consumed");
    return *impl_;
}
const Builder::Impl& Builder::state() const {
    if (!impl_)
        throw std::logic_error("provenance Builder already consumed");
    return *impl_;
}

FrozenGraph::FrozenGraph(std::shared_ptr<const GraphData> data)
    : data_(std::move(data)) {}

FrozenGraph Builder::freeze() && {
    for (std::size_t index = 1; index < state().graph->summaries.size();
         ++index)
        if (state().graph->summaries[index].value.value == 0)
            throw std::logic_error("unpublished provenance summary");
    std::shared_ptr<const GraphData> data(std::move(state().graph));
    impl_.reset(); // Discard the construction-only interning index.
    return FrozenGraph(std::move(data));
}

const Node& FrozenGraph::node(NodeId id) const {
    if (id.value == 0)
        throw std::out_of_range("invalid provenance node");
    return data_->nodes.at(id.value);
}
std::span<const Edge> FrozenGraph::edges(NodeId id) const {
    const auto& value = node(id);
    return std::span(data_->edges).subspan(value.edge_begin, value.edge_count);
}
StoreStats FrozenGraph::stats() const {
    return {.nodes = data_->nodes.size() - 1,
            .edges = data_->edges.size(),
            .summaries = data_->summaries.size() - 1,
            .intern_hits = data_->intern_hits,
            .node_capacity_bytes = data_->nodes.capacity() * sizeof(Node),
            .edge_capacity_bytes = data_->edges.capacity() * sizeof(Edge),
            .summary_capacity_bytes =
                data_->summaries.capacity() * sizeof(SummaryDescription)};
}

std::span<const SummaryDescription> FrozenGraph::summaries() const {
    return std::span(data_->summaries).subspan(1);
}
const SummaryDescription& FrozenGraph::summary(SummaryId id) const {
    if (id.value == 0)
        throw std::out_of_range("invalid provenance summary");
    return data_->summaries.at(id.value);
}

const Node& Builder::node(NodeId id) const {
    if (id.value == 0)
        throw std::out_of_range("invalid provenance node");
    return state().graph->nodes.at(id.value);
}

std::span<const Edge> Builder::edges(NodeId id) const {
    const auto& value = node(id);
    return std::span(state().graph->edges)
        .subspan(value.edge_begin, value.edge_count);
}

NodeId Builder::add(Rule rule, SiteId site, std::span<const Edge> inputs,
                    std::uint32_t payload, SummaryId owner, Sharing sharing) {
    for (const auto& edge : inputs)
        (void)node(edge.target);
    const Node candidate{.rule = rule,
                         .site = site,
                         .edge_count = checked_id(inputs.size()),
                         .payload = payload,
                         .owner = owner};
    const bool intern = state().storage == Storage::SelectiveIntern &&
                        sharing == Sharing::Eligible && may_share(rule);
    const auto hash = intern ? key(candidate, inputs) : 0;
    NodeId first;
    if (intern) {
        first = state().intern.lookup(hash);
        for (auto current = first; current.value != 0;
             current = state().collision_next.at(current.value)) {
            const auto& previous = node(current);
            if (previous.rule == rule && previous.site == site &&
                previous.payload == payload && previous.owner == owner &&
                std::ranges::equal(edges(current), inputs)) {
                ++state().graph->intern_hits;
                return current;
            }
        }
    }
    const auto id = NodeId{checked_id(state().graph->nodes.size())};
    const auto offset = checked_id(state().graph->edges.size());
    (void)checked_id(state().graph->edges.size() + inputs.size());
    // Copy before vector growth, so a span from this Builder is safe.
    const llvm::SmallVector<Edge, 4> copied(inputs.begin(), inputs.end());
    state().graph->edges.insert(state().graph->edges.end(), copied.begin(),
                                copied.end());
    auto stored = candidate;
    stored.edge_begin = offset;
    state().graph->nodes.push_back(stored);
    if (intern) {
        state().collision_next.resize(state().graph->nodes.size());
        state().collision_next[id.value] = first;
        state().intern[hash] = id;
    }
    return id;
}

SummaryId Builder::reserve_summary() {
    const auto id = SummaryId{checked_id(state().graph->summaries.size())};
    state().graph->summaries.push_back({.id = id});
    return id;
}

void Builder::define_summary(SummaryId summary, NodeId root) {
    (void)node(root);
    if (summary.value == 0)
        throw std::out_of_range("invalid provenance summary");
    auto& target = state().graph->summaries.at(summary.value);
    if (target.value.value != 0)
        throw std::logic_error("provenance summary already published");
    target.value = root;
    target.effects = root;
}

NodeId Builder::parameter(SummaryId owner, std::uint32_t slot, SiteId site) {
    if (owner.value == 0 || owner.value >= state().graph->summaries.size())
        throw std::out_of_range("invalid provenance parameter scope");
    return add(Rule::Parameter, site, {}, slot, owner, Sharing::Eligible);
}

NodeId Builder::call(SummaryId summary, std::span<const NodeId> arguments,
                     SiteId site) {
    if (summary.value == 0 || summary.value >= state().graph->summaries.size())
        throw std::out_of_range("invalid provenance callee");
    llvm::SmallVector<Edge, 4> inputs;
    inputs.reserve(arguments.size());
    for (const auto argument : arguments)
        inputs.push_back({argument, Relation::Data});
    return add(Rule::Call, site, inputs, summary.value);
}

NodeId Builder::summary_ref(SummaryId summary, SiteId site) {
    if (summary.value == 0 || summary.value >= state().graph->summaries.size())
        throw std::out_of_range("invalid provenance summary reference");
    return add(Rule::SummaryRef, site, {}, summary.value);
}

NodeId Builder::project(NodeId aggregate, std::uint32_t member, SiteId site) {
    auto base = aggregate;
    while (node(base).rule == Rule::Use || node(base).rule == Rule::Sequence) {
        NodeId value;
        bool transparent = true;
        for (const auto& edge : edges(base)) {
            if (edge.relation == Relation::Data && value.value == 0)
                value = edge.target;
            else if (edge.relation != Relation::Effect &&
                     edge.relation != Relation::Initializer)
                transparent = false;
        }
        if (!transparent || value.value == 0)
            break;
        base = value;
    }
    const auto& value = node(base);
    llvm::SmallVector<Edge, 2> inputs;
    if (value.rule == Rule::Aggregate) {
        const auto members = edges(base);
        if (member >= members.size())
            throw std::out_of_range("invalid provenance member index");
        inputs.push_back(members[member]);
        inputs.push_back({aggregate, Relation::Initializer});
    } else {
        // In particular, an explicit Runtime boundary is never transparent.
        inputs.push_back({aggregate, Relation::Data});
    }
    return add(Rule::Projection, site, inputs, member);
}

StoreStats Builder::stats() const {
    return {
        .nodes = state().graph->nodes.size() - 1,
        .edges = state().graph->edges.size(),
        .summaries = state().graph->summaries.size() - 1,
        .intern_hits = state().graph->intern_hits,
        .node_capacity_bytes = state().graph->nodes.capacity() * sizeof(Node),
        .edge_capacity_bytes = state().graph->edges.capacity() * sizeof(Edge),
        .summary_capacity_bytes =
            state().graph->summaries.capacity() * sizeof(SummaryDescription),
        .intern_capacity_bytes =
            state().intern.getMemorySize() +
            state().collision_next.capacity() * sizeof(NodeId)};
}

QueryResult FrozenGraph::query(NodeId root, QueryKind kind,
                               bool shortest_witness,
                               std::size_t state_limit) const {
    auto task = begin_query(root, kind, shortest_witness);
    task.legacy_limit(state_limit);
    const auto page = task.advance(QuerySlice::unlimited());
    if (page.progress == QueryProgress::Failed)
        throw std::runtime_error("provenance query allocation failed");
    return task.result();
}

} // namespace pagos::stage::provenance
