#pragma once

#include "pagos/stage/provenance_recursive.h"

#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace pagos::cache::wire {
class Writer;
}

namespace pagos::stage::provenance {
namespace recursive_detail {
struct ReadFailure {
    EvidenceError error;
};
constexpr auto absent = std::numeric_limits<std::size_t>::max();
inline bool followed(QueryKind channel, Relation relation) {
    return channel == QueryKind::Effects ||
           (relation != Relation::Effect && relation != Relation::Initializer);
}
inline std::uint32_t bit(RecursiveIssue issue) {
    return static_cast<std::uint32_t>(issue);
}
} // namespace recursive_detail
using namespace recursive_detail;

struct RecursiveSummaryTask::Impl {
    struct Equation {
        NodeId id;
        Node node;
        std::vector<std::size_t> inputs, users, arguments;
        std::size_t callee{absent}, component{absent};
        std::set<RecursiveAtom> facts;
        std::uint32_t issues{};
        bool merged{}, queued{};
    };
    struct Frame {
        std::size_t node, edge{};
    };
    enum class Phase {
        Discover,
        Forward,
        Reverse,
        Summaries,
        Seed,
        Solve,
        Done
    };
    Impl(std::shared_ptr<const EvidenceView> view, SummaryId selected,
         QueryKind kind, bool seed = true)
        : view(std::move(view)), selected(selected), channel(kind) {
        if (!this->view || !selected.value ||
            selected.value > this->view->summaries().size())
            throw std::out_of_range("invalid recursive summary");
        if (kind != QueryKind::Value && kind != QueryKind::Effects)
            throw std::invalid_argument("invalid recursive channel");
        if (seed) {
            register_summary(selected);
            (void)admit(root(selected));
        }
    }
    std::shared_ptr<const EvidenceView> view;
    std::optional<QueryCheckpointBinding> binding;
    cache::Digest task_id;
    SummaryId selected;
    QueryKind channel;
    QueryProgress progress{QueryProgress::Running};
    Phase phase{Phase::Discover};
    RecursiveStats stats;
    bool result_acknowledged{};
    std::optional<EvidenceError> failure;
    struct Offloaded {
        cache::TaskStore store;
        cache::TaskHead head;
        cache::Limits limits;
        QueryProgress resume_progress;
    };
    std::optional<Offloaded> offloaded;
    std::optional<CheckpointError> storage_error;
    RecursiveStorageStats io;
    std::vector<Equation> equations;
    std::unordered_map<std::uint32_t, std::size_t> ids;
    std::vector<RecursiveLink> links;
    std::vector<RecursiveComponent> components;
    std::vector<SummaryId> summaries;
    std::set<std::uint32_t> known_summaries;
    std::size_t cursor{}, edge{};
    bool summary_link{};
    std::optional<NodeLease> lease;
    std::vector<std::uint8_t> seen;
    std::vector<Frame> stack;
    std::vector<std::size_t> order;
    std::size_t component_cursor{}, member{};
    std::deque<std::size_t> work;
    struct Transfer {
        std::size_t node{absent}, input{}, notify{}, bound{absent};
        bool started{}, changed{}, notifying{};
        std::set<RecursiveAtom>::const_iterator atom, argument;
    } transfer;

    void write_state(cache::wire::Writer& writer) const;
    void read_state(cache::wire::Reader& reader);
    void validate_state(bool active);
    void rebind(const std::optional<RecursiveAtom>& saved_atom,
                const std::optional<RecursiveAtom>& saved_argument);
    void write_identity(cache::wire::Writer& writer,
                        std::uint32_t version) const;
    static void identity(cache::wire::Reader& reader,
                         const QueryCheckpointBinding& binding,
                         cache::Digest task, SummaryId summary,
                         QueryKind channel, std::uint32_t version);

    std::size_t input_count(const Equation& equation) const {
        return equation.node.rule == Rule::Call
                   ? (equation.callee != absent ? 1U : 0U) +
                         (channel == QueryKind::Effects
                              ? equation.arguments.size()
                              : 0U)
                   : equation.inputs.size();
    }
    std::size_t input_at(const Equation& equation, std::size_t index) const {
        if (equation.node.rule != Rule::Call)
            return equation.inputs[index];
        if (equation.callee != absent && index == 0)
            return equation.callee;
        return equation
            .arguments[index - (equation.callee != absent ? 1U : 0U)];
    }

    NodeId root(SummaryId id) const {
        const auto& description = view->summary(id);
        return channel == QueryKind::Value ? description.value
                                           : description.effects;
    }
    void register_summary(SummaryId id) {
        if (known_summaries.insert(id.value).second)
            summaries.push_back(id);
    }
    std::size_t admit(NodeId node) {
        if (!node.value || node.value > view->node_count())
            throw ReadFailure{{EvidenceErrorCode::InvalidReference, {}, {}}};
        if (const auto found = ids.find(node.value); found != ids.end())
            return found->second;
        const auto id = equations.size();
        equations.push_back({.id = node, .node = {}});
        ids.emplace(node.value, id);
        return id;
    }
    std::size_t link(std::size_t from, NodeId to, RecursiveLinkKind kind,
                     Relation relation = Relation::Data,
                     std::uint32_t slot = 0) {
        const auto target = admit(to); // May move equations.
        equations[from].inputs.push_back(target);
        equations[target].users.push_back(from);
        links.push_back({equations[from].id, to, kind, relation, slot});
        ++stats.links;
        return target;
    }
    void discover() {
        if (!lease) {
            if (cursor == equations.size()) {
                seen.resize(equations.size());
                cursor = 0;
                phase = Phase::Forward;
                return;
            }
            auto loaded = view->pin(equations[cursor].id);
            if (!loaded)
                throw ReadFailure{loaded.error()};
            lease = std::move(*loaded);
            auto& equation = equations[cursor];
            equation.node = lease->node();
            const auto& node = equation.node;
            ++stats.nodes;
            if (node.rule > Rule::Incomplete)
                equation.issues |= bit(RecursiveIssue::UnsupportedRule);
            if (node.rule == Rule::Incomplete)
                equation.issues |= bit(RecursiveIssue::IncompleteNode);
            if (node.rule == Rule::Parameter) {
                if (!node.owner.value ||
                    node.owner.value > view->summaries().size())
                    equation.issues |= bit(RecursiveIssue::UnboundParameter);
                else {
                    equation.facts.insert({RecursiveAtomKind::Parameter,
                                           node.owner.value, node.payload, 0});
                    ++stats.facts_added;
                }
            }
            if (channel == QueryKind::Value
                    ? node.rule == Rule::Origin ||
                          node.rule == Rule::RuntimeBoundary
                    : node.rule == Rule::Effect) {
                equation.facts.insert(
                    {RecursiveAtomKind::SourceFamily, 0, 0, equation.id.value});
                ++stats.facts_added;
            }
            edge = 0;
            summary_link =
                node.rule == Rule::Call || node.rule == Rule::SummaryRef;
            return;
        }
        const auto node = equations[cursor].node;
        if (summary_link) {
            summary_link = false;
            if (!node.payload || node.payload > view->summaries().size())
                equations[cursor].issues |= bit(RecursiveIssue::MissingSummary);
            else {
                register_summary({node.payload});
                const auto callee = link(cursor, root({node.payload}),
                                         RecursiveLinkKind::Summary);
                equations[cursor].callee = callee;
            }
            return;
        }
        const auto edges = lease->edges();
        if (node.rule != Rule::Parameter && node.rule != Rule::SummaryRef &&
            edge < edges.size()) {
            const auto slot = edge++;
            const auto next = edges[slot];
            if (next.relation > Relation::Initializer)
                equations[cursor].issues |=
                    bit(RecursiveIssue::UnsupportedRelation);
            if (node.rule == Rule::Call) {
                const auto actual =
                    link(cursor, next.target, RecursiveLinkKind::Argument,
                         next.relation, static_cast<std::uint32_t>(slot));
                equations[cursor].arguments.push_back(actual);
            } else if (followed(channel, next.relation))
                (void)link(cursor, next.target, RecursiveLinkKind::Dependency,
                           next.relation);
            return;
        }
        lease.reset();
        ++cursor;
    }
    void forward() {
        if (stack.empty()) {
            if (cursor == equations.size()) {
                cursor = order.size();
                phase = Phase::Reverse;
                return;
            }
            const auto next = cursor++;
            if (!seen[next]) {
                seen[next] = true;
                stack.push_back({next});
            }
            return;
        }
        auto& frame = stack.back();
        const auto& inputs = equations[frame.node].inputs;
        if (frame.edge < inputs.size()) {
            const auto next = inputs[frame.edge++];
            if (!seen[next]) {
                seen[next] = true;
                stack.push_back({next});
            }
        } else {
            order.push_back(frame.node);
            stack.pop_back();
        }
    }
    void assign(std::size_t node) {
        auto& component = components.back();
        equations[node].component = components.size() - 1;
        component.nodes.push_back(equations[node].id);
        if (component.nodes.size() == 2 && !component.cyclic) {
            component.cyclic = true;
            ++stats.cyclic_components;
        }
        stack.push_back({node});
    }
    void reverse() {
        if (stack.empty()) {
            if (!cursor) {
                cursor = 0;
                phase = Phase::Summaries;
                return;
            }
            const auto next = order[--cursor];
            if (equations[next].component == absent) {
                components.emplace_back();
                ++stats.components;
                assign(next);
            }
            return;
        }
        auto& frame = stack.back();
        const auto& users = equations[frame.node].users;
        if (frame.edge < users.size()) {
            const auto next = users[frame.edge++];
            if (next == frame.node && !components.back().cyclic) {
                components.back().cyclic = true;
                ++stats.cyclic_components;
            }
            if (equations[next].component == absent)
                assign(next);
        } else
            stack.pop_back();
    }
    void enqueue(std::size_t node) {
        if (!equations[node].queued) {
            equations[node].queued = true;
            work.push_back(node);
            stats.queue_peak = std::max(stats.queue_peak, work.size());
        }
    }
    void merge(Equation& destination, const Equation& source) {
        const auto old = destination.issues;
        destination.issues |= source.issues;
        transfer.changed |=
            old != destination.issues || (!destination.merged && source.merged);
        destination.merged |= source.merged;
    }
    void add(Equation& destination, RecursiveAtom atom) {
        if (destination.facts.insert(atom).second) {
            transfer.changed = true;
            ++stats.facts_added;
        }
    }
    void issue(Equation& destination, RecursiveIssue value) {
        const auto old = destination.issues;
        destination.issues |= bit(value);
        transfer.changed |= old != destination.issues;
    }
    void solve() {
        auto& state = transfer;
        if (state.node == absent) {
            if (work.empty()) {
                member = 0;
                phase = Phase::Seed;
                return;
            }
            state = {};
            state.node = work.front();
            work.pop_front();
            equations[state.node].queued = false;
            ++stats.evaluations;
            return;
        }
        auto& destination = equations[state.node];
        if (state.notifying) {
            if (state.changed && state.notify < destination.users.size()) {
                const auto user = destination.users[state.notify++];
                if (equations[user].component == destination.component)
                    enqueue(user);
            } else
                state.node = absent;
            return;
        }
        if (state.bound != absent) {
            const auto& argument = equations[state.bound];
            if (state.argument != argument.facts.end())
                add(destination, *state.argument++);
            else {
                state.bound = absent;
                ++state.atom;
            }
            return;
        }
        const bool call = destination.node.rule == Rule::Call;
        // Calls read the callee first, then (Effects only) all actual work.
        const auto count = input_count(destination);
        if (state.input == count) {
            state.notifying = true;
            return;
        }
        const bool callee =
            call && destination.callee != absent && state.input == 0;
        const auto input = input_at(destination, state.input);
        const auto& source = equations[input];
        if (!state.started) {
            merge(destination, source);
            state.atom = source.facts.begin();
            state.started = true;
            return;
        }
        if (state.atom == source.facts.end()) {
            ++state.input;
            state.started = false;
            return;
        }
        const auto atom = *state.atom;
        if (!callee)
            add(destination, *state.atom++);
        else if (atom.kind == RecursiveAtomKind::SourceFamily) {
            add(destination, atom);
            state.changed |= !destination.merged;
            destination.merged = true;
            ++state.atom;
        } else if (atom.owner != destination.node.payload) {
            issue(destination, RecursiveIssue::ForeignScope);
            add(destination, atom);
            ++state.atom;
        } else if (atom.slot >= destination.arguments.size()) {
            issue(destination, RecursiveIssue::UnboundParameter);
            ++state.atom;
        } else {
            state.bound = destination.arguments[atom.slot];
            merge(destination, equations[state.bound]);
            state.argument = equations[state.bound].facts.begin();
        }
    }
    void step() {
        switch (phase) {
        case Phase::Discover:
            discover();
            break;
        case Phase::Forward:
            forward();
            break;
        case Phase::Reverse:
            reverse();
            break;
        case Phase::Summaries:
            if (cursor < summaries.size()) {
                const auto summary = summaries[cursor++];
                const auto equation = ids.at(root(summary).value);
                components[equations[equation].component].summaries.push_back(
                    summary);
            } else {
                component_cursor = components.size();
                phase = Phase::Seed;
            }
            break;
        case Phase::Seed:
            if (member == 0) {
                if (!component_cursor) {
                    phase = Phase::Done;
                    progress = QueryProgress::Completed;
                    break;
                }
                --component_cursor;
            }
            if (member < components[component_cursor].nodes.size()) {
                enqueue(
                    ids.at(components[component_cursor].nodes[member++].value));
            } else
                phase = Phase::Solve;
            break;
        case Phase::Solve:
            solve();
            break;
        case Phase::Done:
            break;
        }
    }
};

} // namespace pagos::stage::provenance
