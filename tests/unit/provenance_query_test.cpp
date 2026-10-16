#include "pagos/stage/provenance_query.h"
#include "pagos/stage/provenance_topology.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <set>
#include <stop_token>

namespace {
using namespace pagos::stage::provenance;
QueryResult finish(QueryTask& task, QuerySlice slice = {1, 1, 1, 1}) {
    while (task.progress() != QueryProgress::Completed &&
           task.progress() != QueryProgress::Failed)
        (void)task.advance(slice);
    EXPECT_EQ(task.progress(), QueryProgress::Completed);
    return task.result();
}
std::shared_ptr<const FormulaIndex> prepare(const FrozenGraph& graph,
                                            NodeId root) {
    auto task = graph.begin_topology(root);
    EXPECT_FALSE(task.index());
    while (task.progress() != QueryProgress::Completed &&
           task.progress() != QueryProgress::Failed)
        (void)task.advance({1, 1, 1, 1});
    EXPECT_EQ(task.progress(), QueryProgress::Completed);
    return task.index();
}
void equivalent(const QueryResult& left, const QueryResult& right) {
    EXPECT_EQ(left.sources, right.sources);
    EXPECT_EQ(left.witness, right.witness);
    EXPECT_EQ(left.invocations, right.invocations);
    EXPECT_EQ(left.visited_states, right.visited_states);
    EXPECT_EQ(left.visited_edges, right.visited_edges);
    EXPECT_EQ(left.complete, right.complete);
    EXPECT_EQ(left.recursive_call_cut, right.recursive_call_cut);
}
// Context numbers are query-local. Compare semantic instance identities.
using Instances =
    std::set<std::pair<std::uint32_t, std::vector<std::uint32_t>>>;
Instances instances(const QueryResult& result) {
    Instances output;
    for (auto source : result.sources) {
        std::vector<std::uint32_t> path;
        for (auto frame = source.context; frame;
             frame = result.invocations[frame].parent)
            path.push_back(result.invocations[frame].call.value);
        output.emplace(source.node.value, path);
    }
    EXPECT_EQ(output.size(), result.sources.size());
    return output;
}
TEST(ProvenanceQuery,
     ResumesNodesEdgesBindingsAndWitnessWithoutDoubleCharging) {
    Builder builder;
    const auto inner = builder.reserve_summary();
    const auto param = builder.parameter(inner, 0, 1);
    const auto effect = builder.add(Rule::Effect, 2);
    builder.define_summary(
        inner,
        builder.add(Rule::Sequence, 3,
                    std::array{Edge{param}, Edge{effect, Relation::Effect}}));
    const auto outer = builder.reserve_summary();
    builder.define_summary(
        outer,
        builder.call(inner, std::array{builder.parameter(outer, 0, 4)}, 5));
    const auto a = builder.add(Rule::Origin, 6);
    const auto b = builder.add(Rule::Origin, 7);
    const auto root =
        builder.add(Rule::Strict, 10,
                    std::array{Edge{builder.call(outer, std::array{a}, 8)},
                               Edge{builder.call(outer, std::array{b}, 9)}});
    const auto graph = std::move(builder).freeze();
    for (const auto kind : {QueryKind::Value, QueryKind::Effects}) {
        for (const bool witness : {false, true}) {
            const auto oracle = graph.query(root, kind, witness);
            auto once = graph.begin_query(root, kind, witness);
            (void)finish(once, QuerySlice::unlimited());
            auto task = graph.begin_query(root, kind, witness);
            const auto zero = task.advance({0, 0, 0, 0});
            EXPECT_EQ(zero.progress, QueryProgress::Paused);
            EXPECT_EQ(task.stats().visited_states, 0U);
            EXPECT_EQ(task.stats().admitted_states, 0U);
            std::stop_source cancellation;
            cancellation.request_stop();
            EXPECT_EQ(
                task.advance({1, 1, 1, 1}, cancellation.get_token()).progress,
                QueryProgress::Cancelled);
            task.resume();
            equivalent(finish(task), oracle);
            EXPECT_EQ(task.stats().bindings, once.stats().bindings);
            EXPECT_EQ(task.stats().scanned_edges, once.stats().scanned_edges);
            EXPECT_EQ(task.stats().admitted_states,
                      once.stats().admitted_states);
            EXPECT_EQ(task.stats().witness_steps, once.stats().witness_steps);
            EXPECT_EQ(task.coverage().all_sources, !witness);
            EXPECT_EQ(task.coverage().witness_found, witness);
            EXPECT_EQ(task.coverage().precision,
                      witness ? EvidencePrecision::Unresolved
                              : EvidencePrecision::ExactForRecordedModel);
        }
    }
}
TEST(ProvenanceQuery,
     WideEdgeCursorCancellationAndPagedDeliveryAreIndependent) {
    Builder builder;
    std::vector<Edge> edges;
    for (unsigned i = 0; i < 1000; ++i)
        edges.push_back({builder.add(Rule::Origin, i + 1)});
    const auto root = builder.add(Rule::Strict, 1001, edges);
    const auto graph = std::move(builder).freeze();
    auto task = graph.begin_query(root);
    (void)task.advance({1, 7, 0, 0});
    EXPECT_EQ(task.stats().visited_states, 1U);
    EXPECT_EQ(task.stats().scanned_edges, 7U);
    EXPECT_EQ(task.stats().admitted_states, 8U);
    task.cancel();
    (void)task.advance(QuerySlice::unlimited());
    EXPECT_EQ(task.stats().scanned_edges, 7U);
    task.resume();
    auto page = task.advance({2000, 2000, 2000, 0});
    EXPECT_EQ(page.progress, QueryProgress::Completed);
    EXPECT_TRUE(page.coverage.all_sources);
    EXPECT_EQ(page.pending_sources, 1000U);
    EXPECT_EQ(page.sequence, 0U);
    std::vector<Source> delivered;
    std::uint64_t sequence = 0;
    while (page.pending_sources) {
        page = task.advance({0, 0, 0, 13});
        EXPECT_EQ(page.sequence, ++sequence);
        delivered.insert(delivered.end(), page.sources.begin(),
                         page.sources.end());
    }
    EXPECT_EQ(delivered, task.result().sources);
    EXPECT_EQ(task.stats().scanned_edges, 1000U);
    equivalent(task.result(), graph.query(root));
}
TEST(ProvenanceQuery, LongChainPassesLegacyMillionStateCeilingAndOwnsGraph) {
    auto task = [] {
        Builder builder;
        auto root = builder.add(Rule::Origin, 1);
        for (unsigned i = 0; i < 1'000'001; ++i)
            root = builder.add(Rule::Use, 2, std::array{Edge{root}});
        const auto graph = std::move(builder).freeze();
        EXPECT_FALSE(graph.query(root).complete);
        return graph.begin_query(root);
    }();
    const auto result = finish(task, {16384, 16384, 16384, 0});
    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.visited_states, 1'000'002U);
    ASSERT_EQ(result.sources.size(), 1U);
    EXPECT_EQ(task.graph().node(result.sources.front().node).site, 1U);
}
TEST(ProvenanceQuery, SharedDiamondVisitsStatesInsteadOfPaths) {
    Builder builder;
    auto root = builder.add(Rule::Origin, 1);
    for (unsigned i = 0; i < 40; ++i) {
        const auto a = builder.add(Rule::Use, 2, std::array{Edge{root}});
        const auto b = builder.add(Rule::Use, 3, std::array{Edge{root}});
        root = builder.add(Rule::Strict, 4, std::array{Edge{a}, Edge{b}});
    }
    const auto graph = std::move(builder).freeze();
    auto task = graph.begin_query(root);
    EXPECT_EQ(finish(task).visited_states, 121U);
    EXPECT_EQ(task.result().sources.size(), 1U);
}
TEST(ProvenanceQuery, CoverageGapsAndSummaryCyclesSurviveSlices) {
    Builder builder;
    const auto one = builder.reserve_summary();
    const auto two = builder.reserve_summary();
    const auto ref = builder.summary_ref(one, 1);
    builder.define_summary(one, builder.summary_ref(two, 2));
    builder.define_summary(two, ref);
    const auto recursive = builder.reserve_summary();
    const auto parameter = builder.parameter(recursive, 0, 3);
    builder.define_summary(recursive,
                           builder.call(recursive, std::array{parameter}, 4));
    const auto bad = builder.add(Rule::Call, 5, {}, 999);
    const auto incomplete = builder.add(Rule::Incomplete, 6);
    const auto call = builder.call(recursive, std::array{incomplete}, 7);
    const auto graph = std::move(builder).freeze();
    for (const auto root : {ref, parameter, call}) {
        auto task = graph.begin_query(root);
        equivalent(finish(task), graph.query(root));
    }
    auto missing = graph.begin_query(bad);
    EXPECT_FALSE(finish(missing).complete);
    EXPECT_NE(missing.coverage().issues &
                  static_cast<unsigned>(CoverageIssue::MissingSummary),
              0U);
    auto task = graph.begin_query(call);
    (void)finish(task);
    EXPECT_NE(task.coverage().issues &
                  static_cast<unsigned>(CoverageIssue::RecursiveCall),
              0U);
    EXPECT_NE(task.coverage().issues &
                  static_cast<unsigned>(CoverageIssue::IncompleteNode),
              0U);
    EXPECT_EQ(task.coverage().precision, EvidencePrecision::Conservative);
}
TEST(ProvenanceTopology,
     FormulaEliminatesWrapperTraversalAndPreservesOriginalWitness) {
    Builder builder;
    auto root = builder.add(Rule::Origin, 1);
    for (unsigned i = 0; i < 10000; ++i)
        root = builder.add(Rule::Use, 2, std::array{Edge{root}});
    const auto graph = std::move(builder).freeze();
    const auto index = prepare(graph, root);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->stats().nodes, 10001U);
    EXPECT_EQ(index->stats().formulas, 2U);
    auto task = index->begin_sources(root);
    EXPECT_EQ(instances(finish(task)), instances(graph.query(root)));
    EXPECT_EQ(task.stats().visited_states, 2U);
    EXPECT_EQ(task.stats().formula_uses, 1U);
    EXPECT_EQ(task.stats().scanned_edges, 1U);
    auto witness = graph.begin_query(root, QueryKind::Value, true);
    EXPECT_EQ(finish(witness, QuerySlice::unlimited()).witness.size(), 10001U);
}
TEST(ProvenanceTopology,
     NestedSubstitutionChannelsProjectionContractsAndInstancesAgree) {
    Builder builder;
    const auto inner = builder.reserve_summary();
    const auto parameter = builder.parameter(inner, 1, 1);
    const auto read = builder.add(Rule::Origin, 2);
    const auto write = builder.add(Rule::Effect, 3);
    builder.define_summary(
        inner, builder.add(Rule::Sequence, 4,
                           std::array{Edge{parameter}, Edge{read},
                                      Edge{write, Relation::Effect}}));
    const auto outer = builder.reserve_summary();
    const auto p = builder.parameter(outer, 0, 5);
    const auto literal = builder.add(Rule::Literal, 6);
    const auto unused =
        builder.add(Rule::Sequence, 7,
                    std::array{Edge{literal}, Edge{write, Relation::Effect}});
    const auto nested = builder.call(inner, std::array{unused, p}, 8);
    builder.define_summary(outer, nested);
    const auto a = builder.add(Rule::Origin, 9);
    const auto b = builder.add(Rule::RuntimeBoundary, 10,
                               std::array{Edge{a, Relation::Initializer}});
    const auto ca = builder.call(outer, std::array{a}, 11);
    const auto cb = builder.call(outer, std::array{b}, 12);
    const auto aggregate =
        builder.add(Rule::Aggregate, 13, std::array{Edge{ca}, Edge{cb}});
    const auto projected = builder.project(aggregate, 1, 14);
    const auto root = builder.add(
        Rule::RequireComptime, 15,
        std::array{Edge{projected, Relation::Contract}, Edge{aggregate}});
    const auto graph = std::move(builder).freeze();
    EXPECT_EQ(graph.summaries().size(), 2U);
    EXPECT_EQ(graph.summary(inner).value, graph.summary(inner).effects);
    const auto index = prepare(graph, root);
    for (const auto kind : {QueryKind::Value, QueryKind::Effects}) {
        auto task = index->begin_sources(root, kind);
        const auto result = finish(task);
        EXPECT_EQ(instances(result), instances(graph.query(root, kind)));
        EXPECT_EQ(result.complete, graph.query(root, kind).complete);
        auto selected = index->begin_invocation(std::array{cb, nested}, kind);
        const auto selected_result = finish(selected);
        EXPECT_FALSE(selected_result.sources.empty());
        EXPECT_EQ(selected.invocations()[1].call, cb);
        EXPECT_EQ(selected.invocations()[2].call, nested);
    }
    EXPECT_THROW((void)index->begin_invocation(std::array{ca, cb}),
                 std::invalid_argument);
    EXPECT_THROW((void)index->begin_invocation(std::span<const NodeId>{}),
                 std::invalid_argument);
}
TEST(ProvenanceTopology,
     FortyLayerCallDagStaysFiniteAndExpandsOnlySelectedPath) {
    Builder builder;
    const auto read = builder.add(Rule::Origin, 1);
    auto summary = builder.reserve_summary();
    builder.define_summary(summary, read);
    std::vector<NodeId> path;
    for (unsigned layer = 0; layer < 40; ++layer) {
        const auto a = builder.call(summary, {}, 2);
        const auto b = builder.call(summary, {}, 3);
        path.push_back(a);
        const auto next = builder.reserve_summary();
        builder.define_summary(
            next, builder.add(Rule::Strict, 4, std::array{Edge{a}, Edge{b}}));
        summary = next;
    }
    const auto root = builder.call(summary, {}, 5);
    path.push_back(root);
    std::ranges::reverse(path);
    const auto graph = std::move(builder).freeze();
    const auto index = prepare(graph, root);
    EXPECT_EQ(index->stats().nodes, 122U);
    EXPECT_EQ(index->stats().summary_links, 162U);
    EXPECT_LE(index->stats().formula_terms, 170U);
    auto selected = index->begin_invocation(path);
    const auto result = finish(selected);
    ASSERT_EQ(result.sources.size(), 1U);
    EXPECT_EQ(result.invocations.size(), 42U);
    EXPECT_EQ(result.sources.front().context, 41U);
    EXPECT_EQ(selected.stats().visited_states, 2U);
}
TEST(ProvenanceTopology,
     BacklinksFallbackAndOutputCancellationDoNotDiscardRecords) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    const auto ref = builder.summary_ref(summary, 1);
    const auto odd =
        builder.add(static_cast<Rule>(255), 2, std::array{Edge{ref}});
    builder.define_summary(summary, odd);
    const auto graph = std::move(builder).freeze();
    auto topology = graph.begin_topology(odd);
    EXPECT_TRUE(topology.advance({1, 1, 1, 0}).records.empty());
    EXPECT_EQ(topology.stats().nodes, 0U);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(topology.advance({1, 1, 1, 1}, stop.get_token()).progress,
              QueryProgress::Cancelled);
    topology.resume();
    std::size_t records = 0;
    std::uint64_t sequence = 0;
    while (topology.progress() != QueryProgress::Completed) {
        const auto page = topology.advance({1, 1, 1, 1});
        records += page.records.size();
        if (!page.records.empty()) {
            EXPECT_EQ(page.sequence, ++sequence);
        }
    }
    EXPECT_EQ(records, 5U); // Two nodes, one edge, two summary ports.
    const auto index = topology.index();
    EXPECT_EQ(index->formula(odd, QueryKind::Value)->fallback,
              "unsupported rule");
    auto task = index->begin_sources(odd);
    EXPECT_TRUE(finish(task).complete); // Same recorded-model behavior as raw.
    EXPECT_GT(task.stats().formula_fallbacks, 0U);
}
TEST(ProvenanceQuery, WitnessDeliveryHasItsOwnCursorAndNoGlobalCoverageClaim) {
    Builder builder;
    const auto source = builder.add(Rule::Origin, 1);
    const auto use = builder.add(Rule::Use, 2, std::array{Edge{source}});
    const auto root = builder.add(Rule::Strict, 3, std::array{Edge{use}});
    const auto graph = std::move(builder).freeze();
    auto task = graph.begin_query(root, QueryKind::Value, true);
    (void)task.advance({10, 10, 1, 0});
    EXPECT_EQ(task.progress(), QueryProgress::Paused);
    EXPECT_TRUE(task.coverage().witness_found);
    EXPECT_FALSE(task.coverage().all_sources);
    EXPECT_EQ(task.stats().witness_steps, 1U);
    task.cancel();
    task.resume();
    auto page = task.advance({0, 0, 10, 1});
    EXPECT_EQ(page.progress, QueryProgress::Completed);
    EXPECT_EQ(page.sources.size(), 1U);
    EXPECT_TRUE(page.pending_witness);
    std::vector<QueryState> witness;
    std::uint64_t sequence = page.sequence;
    while (page.pending_witness) {
        page = task.advance({0, 0, 0, 1});
        EXPECT_EQ(page.sequence, ++sequence);
        witness.insert(witness.end(), page.witness.begin(), page.witness.end());
    }
    EXPECT_EQ(witness,
              (std::vector<QueryState>{{root, 0}, {use, 0}, {source, 0}}));
    EXPECT_EQ(task.stats().witness_steps, 3U);
    EXPECT_EQ(task.advance({0, 0, 0, 1}).sequence, 0U);
}
TEST(ProvenanceTopology, SelectedInvocationKeepsUnusedArgumentWorkAndWitness) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    const auto literal = builder.add(Rule::Literal, 1);
    builder.define_summary(summary, literal);
    const auto write = builder.add(Rule::Effect, 2);
    const auto argument =
        builder.add(Rule::Sequence, 3,
                    std::array{Edge{literal}, Edge{write, Relation::Effect}});
    const auto call = builder.call(summary, std::array{argument}, 4);
    const auto graph = std::move(builder).freeze();
    const auto index = prepare(graph, call);
    auto task = index->begin_invocation(std::array{call}, QueryKind::Effects);
    EXPECT_EQ(instances(finish(task)),
              instances(graph.query(call, QueryKind::Effects)));
    auto witness =
        index->begin_invocation(std::array{call}, QueryKind::Effects, true);
    equivalent(finish(witness), graph.query(call, QueryKind::Effects, true));
    EXPECT_EQ(index->stats().source_families, 1U);
}
TEST(ProvenanceTopology,
     RecursiveCallsRemainPartialAndSelectedRecursivePathsAreRejected) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    const auto read = builder.add(Rule::Origin, 1);
    const auto back = builder.call(summary, {}, 2);
    builder.define_summary(
        summary,
        builder.add(Rule::Strict, 3, std::array{Edge{read}, Edge{back}}));
    const auto call = builder.call(summary, {}, 4);
    const auto graph = std::move(builder).freeze();
    const auto index = prepare(graph, call);
    auto task = index->begin_sources(call);
    const auto result = finish(task);
    EXPECT_FALSE(result.complete);
    EXPECT_TRUE(result.recursive_call_cut);
    EXPECT_EQ(instances(result), instances(graph.query(call)));
    EXPECT_THROW((void)index->begin_invocation(std::array{call, back}),
                 std::invalid_argument);
}

TEST(ProvenanceTopology,
     SmallSharedCallDagMatchesIndependentConcreteInstanceOracle) {
    Builder builder;
    const auto read = builder.add(Rule::Origin, 1);
    auto summary = builder.reserve_summary();
    builder.define_summary(summary, read);
    std::vector<std::array<NodeId, 2>> layers;
    for (unsigned depth = 0; depth < 4; ++depth) {
        const std::array calls{builder.call(summary, {}, 2),
                               builder.call(summary, {}, 3)};
        layers.push_back(calls);
        const auto next = builder.reserve_summary();
        builder.define_summary(
            next, builder.add(Rule::Strict, 4,
                              std::array{Edge{calls[0]}, Edge{calls[1]}}));
        summary = next;
    }
    const auto root = builder.call(summary, {}, 5);
    const auto graph = std::move(builder).freeze();
    Instances expected;
    for (unsigned bits = 0; bits < 16; ++bits) {
        std::vector<std::uint32_t> path;
        for (unsigned layer = 0; layer < 4; ++layer)
            path.push_back(layers[layer][(bits >> layer) & 1U].value);
        path.push_back(root.value);
        expected.emplace(read.value, path);
    }
    const auto index = prepare(graph, root);
    auto task = index->begin_sources(root);
    EXPECT_EQ(instances(finish(task)), expected);
    EXPECT_EQ(instances(graph.query(root)), expected);
    EXPECT_EQ(index->stats().source_families, 1U);
    EXPECT_EQ(task.result().sources.size(), 16U);
}

} // namespace
