#include "pagos/stage/provenance_graph.h"

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

std::uint64_t state_key(QueryState state) {
    return (static_cast<std::uint64_t>(state.context) << 32U) |
           state.node.value;
}

} // namespace

struct GraphData {
    std::vector<Node> nodes{Node{}};
    std::vector<Edge> edges;
    std::vector<NodeId> summaries{NodeId{}};
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
        if (state().graph->summaries[index].value == 0)
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
                data_->summaries.capacity() * sizeof(NodeId)};
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
    state().graph->summaries.push_back({});
    return id;
}

void Builder::define_summary(SummaryId summary, NodeId root) {
    (void)node(root);
    if (summary.value == 0)
        throw std::out_of_range("invalid provenance summary");
    auto& target = state().graph->summaries.at(summary.value);
    if (target.value != 0)
        throw std::logic_error("provenance summary already published");
    target = root;
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
            state().graph->summaries.capacity() * sizeof(NodeId),
        .intern_capacity_bytes =
            state().intern.getMemorySize() +
            state().collision_next.capacity() * sizeof(NodeId)};
}

QueryResult FrozenGraph::query(NodeId root, QueryKind kind,
                               bool shortest_witness,
                               std::size_t state_limit) const {
    (void)node(root);
    struct Visit {
        QueryState state;
        std::size_t predecessor{};
    };
    QueryResult result;
    std::vector<Invocation> contexts(1);
    llvm::DenseMap<std::uint64_t, std::uint32_t> context_ids;
    llvm::DenseMap<std::uint64_t, std::size_t> visited;
    std::vector<Visit> queue;
    const auto enqueue = [&](QueryState state, std::size_t predecessor) {
        const auto key = state_key(state);
        if (visited.contains(key))
            return;
        if (queue.size() >= state_limit) {
            result.complete = false;
            return;
        }
        visited[key] = queue.size();
        queue.push_back({state, predecessor});
    };
    constexpr auto no_predecessor = std::numeric_limits<std::size_t>::max();
    enqueue({root, 0}, no_predecessor);
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
        const auto state = queue[cursor].state;
        const auto& current = node(state.node);
        ++result.visited_states;
        if (current.rule == Rule::Incomplete)
            result.complete = false;
        const bool source = kind == QueryKind::Value
                                ? current.rule == Rule::Origin ||
                                      current.rule == Rule::RuntimeBoundary
                                : current.rule == Rule::Effect;
        if (source) {
            result.sources.push_back({state.node, current.site, state.context});
            if (shortest_witness) {
                for (auto path = cursor; path != no_predecessor;
                     path = queue[path].predecessor)
                    result.witness.push_back(queue[path].state);
                std::ranges::reverse(result.witness);
                // This query promises one witness, not all reachable sources.
                result.complete = false;
                break;
            }
        }
        if (current.rule == Rule::Parameter) {
            auto frame = state.context;
            while (frame != 0 && contexts[frame].owner != current.owner)
                frame = contexts[frame].parent;
            if (frame == 0) {
                result.complete = false;
            } else {
                const auto& context = contexts[frame];
                const auto arguments = edges(context.call);
                if (current.payload >= arguments.size()) {
                    result.complete = false;
                } else {
                    ++result.visited_edges;
                    enqueue({arguments[current.payload].target, context.parent},
                            cursor);
                }
            }
            continue;
        }
        if (current.rule == Rule::Call || current.rule == Rule::SummaryRef) {
            const auto target = data_->summaries.at(current.payload);
            if (target.value == 0) {
                result.complete = false;
                continue;
            }
            auto frame = state.context;
            bool cut = false;
            if (current.rule == Rule::Call) {
                const auto owner = SummaryId{current.payload};
                bool recursive = false;
                for (auto parent = frame; parent != 0;
                     parent = contexts[parent].parent)
                    recursive |= contexts[parent].owner == owner;
                if (recursive) {
                    cut = true;
                    result.complete = false;
                    result.recursive_call_cut = true;
                } else {
                    const auto key = state_key(state);
                    const auto found = context_ids.find(key);
                    if (found != context_ids.end()) {
                        frame = found->second;
                    } else {
                        const auto id = checked_id(contexts.size());
                        contexts.push_back({state.node, frame, owner});
                        context_ids[key] = id;
                        frame = id;
                    }
                    ++result.visited_edges;
                    enqueue({target, frame}, cursor);
                }
            } else {
                ++result.visited_edges;
                enqueue({target, frame}, cursor);
            }
            // Value queries bind only parameters actually used by the summary.
            // Effect queries also inspect evaluation of every call argument.
            if (kind != QueryKind::Effects && !cut)
                continue;
        }
        for (const auto& edge : edges(state.node)) {
            if (kind == QueryKind::Value &&
                (edge.relation == Relation::Initializer ||
                 edge.relation == Relation::Effect))
                continue;
            ++result.visited_edges;
            enqueue({edge.target, state.context}, cursor);
        }
    }
    result.contexts = contexts.size() - 1;
    result.invocations = std::move(contexts);
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
