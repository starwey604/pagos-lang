#include "pagos/stage/provenance_recursive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stop_token>

namespace {
using namespace pagos::stage::provenance;
using Atoms = std::set<RecursiveAtom>;
RecursiveAtom parameter(SummaryId owner, std::uint32_t slot) {
    return {RecursiveAtomKind::Parameter, owner.value, slot, 0};
}
RecursiveAtom family(NodeId node) {
    return {RecursiveAtomKind::SourceFamily, 0, 0, node.value};
}
RecursiveResult finish(RecursiveSummaryTask& task, std::size_t slice = 1) {
    std::size_t turns = 0;
    while (task.progress() != QueryProgress::Completed &&
           task.progress() != QueryProgress::Failed && turns++ < 1'000'000) {
        const auto before = task.stats().steps;
        (void)task.advance(slice);
        EXPECT_LE(task.stats().steps - before, slice);
    }
    EXPECT_EQ(task.progress(), QueryProgress::Completed);
    return task.result();
}
Atoms atoms(const RecursiveResult& result) {
    return {result.atoms.begin(), result.atoms.end()};
}

// Independent operational oracle: bounded call unfolding with explicit formal
// environments, rather than a worklist or equations over node-local sets.
using Environment = std::map<std::uint32_t, std::vector<Atoms>>;
Atoms unfold(const FrozenGraph& graph, NodeId id, QueryKind kind,
             const Environment& environment, unsigned depth) {
    const auto& node = graph.node(id);
    Atoms output;
    const auto include = [&](const Atoms& values) {
        output.insert(values.begin(), values.end());
    };
    if (node.rule == Rule::Parameter) {
        const auto found = environment.find(node.owner.value);
        if (found == environment.end())
            output.insert(parameter(node.owner, node.payload));
        else if (node.payload < found->second.size())
            include(found->second[node.payload]);
        return output;
    }
    if (node.rule == Rule::Call) {
        std::vector<Atoms> actuals;
        for (const auto& edge : graph.edges(id)) {
            actuals.push_back(
                unfold(graph, edge.target, kind, environment, depth));
            if (kind == QueryKind::Effects)
                include(actuals.back());
        }
        if (depth) {
            auto nested = environment;
            nested[node.payload] = std::move(actuals);
            const auto& summary = graph.summary({node.payload});
            include(unfold(graph,
                           kind == QueryKind::Value ? summary.value
                                                    : summary.effects,
                           kind, nested, depth - 1));
        }
        return output;
    }
    if (kind == QueryKind::Value
            ? node.rule == Rule::Origin || node.rule == Rule::RuntimeBoundary
            : node.rule == Rule::Effect)
        output.insert(family(id));
    for (const auto& edge : graph.edges(id))
        if (kind == QueryKind::Effects ||
            (edge.relation != Relation::Effect &&
             edge.relation != Relation::Initializer))
            include(unfold(graph, edge.target, kind, environment, depth));
    return output;
}

TEST(RecursiveSummary, ParameterPermutationMatchesFiniteUnfoldings) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    std::array<NodeId, 4> parameters;
    for (unsigned i = 0; i < parameters.size(); ++i)
        parameters[i] = builder.parameter(summary, i, i + 1);
    const auto recursive = builder.call(
        summary,
        std::array{parameters[1], parameters[2], parameters[3], parameters[0]},
        5);
    const auto root = builder.add(
        Rule::Select, 6,
        std::array{Edge{parameters[0], Relation::Control}, Edge{recursive}});
    builder.define_summary(summary, root);
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask task(EvidenceView::memory(graph), summary);
    const auto result = finish(task);
    EXPECT_EQ(result.precision, EvidencePrecision::ExactForRecordedModel);
    EXPECT_EQ(result.issues, 0U);
    EXPECT_TRUE(result.fixed_point);
    EXPECT_FALSE(result.concrete_sources_complete);
    EXPECT_FALSE(result.instances_merged);
    EXPECT_EQ(atoms(result),
              (Atoms{parameter(summary, 0), parameter(summary, 1),
                     parameter(summary, 2), parameter(summary, 3)}));
    EXPECT_LT(unfold(graph, root, QueryKind::Value, {}, 1).size(), 4U);
    for (unsigned depth : {3U, 4U, 8U})
        EXPECT_EQ(atoms(result),
                  unfold(graph, root, QueryKind::Value, {}, depth));
    EXPECT_EQ(task.stats().cyclic_components, 1U);
    const auto component = std::ranges::find_if(
        task.components(), [](const auto& item) { return item.cyclic; });
    ASSERT_NE(component, task.components().end());
    EXPECT_EQ(component->summaries, std::vector{summary});
    EXPECT_NE(std::ranges::find(component->nodes, recursive),
              component->nodes.end());
    EXPECT_NE(std::ranges::find(component->nodes, root),
              component->nodes.end());
    EXPECT_NE(std::ranges::find(
                  task.links(),
                  RecursiveLink{recursive, root, RecursiveLinkKind::Summary}),
              task.links().end());
    EXPECT_NE(std::ranges::find(task.links(),
                                RecursiveLink{recursive, parameters[0],
                                              RecursiveLinkKind::Argument,
                                              Relation::Data, 3}),
              task.links().end());
}

TEST(RecursiveSummary, MutualRecursionSubstitutesEachOwnersSlots) {
    Builder builder;
    const auto f = builder.reserve_summary(), g = builder.reserve_summary();
    const auto f0 = builder.parameter(f, 0, 1), f1 = builder.parameter(f, 1, 2);
    const auto g0 = builder.parameter(g, 0, 3), g1 = builder.parameter(g, 1, 4);
    const auto froot = builder.call(g, std::array{f1, f0}, 5);
    const auto groot = builder.add(
        Rule::Strict, 7,
        std::array{Edge{g0}, Edge{builder.call(f, std::array{g0, g1}, 6)}});
    builder.define_summary(f, froot);
    builder.define_summary(g, groot);
    const auto graph = std::move(builder).freeze();
    for (const auto summary : {f, g}) {
        RecursiveSummaryTask task(EvidenceView::memory(graph), summary);
        const auto result = finish(task);
        EXPECT_EQ(result.precision, EvidencePrecision::ExactForRecordedModel);
        EXPECT_EQ(atoms(result),
                  (Atoms{parameter(summary, 0), parameter(summary, 1)}));
        const auto root = graph.summary(summary).value;
        EXPECT_EQ(atoms(result), unfold(graph, root, QueryKind::Value, {}, 8));
        const auto component = std::ranges::find_if(
            task.components(), [](const auto& item) { return item.cyclic; });
        ASSERT_NE(component, task.components().end());
        EXPECT_EQ(component->summaries.size(), 2U);
    }
}

TEST(RecursiveSummary, SummaryReferenceCyclesPreserveSourceFamiliesExactly) {
    Builder builder;
    const auto a = builder.reserve_summary(), b = builder.reserve_summary();
    const auto source = builder.add(Rule::Origin, 1);
    const auto ar =
        builder.add(Rule::Strict, 3,
                    std::array{Edge{source}, Edge{builder.summary_ref(b, 2)}});
    builder.define_summary(a, ar);
    builder.define_summary(b, builder.summary_ref(a, 4));
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask task(EvidenceView::memory(graph), a);
    const auto result = finish(task);
    EXPECT_EQ(atoms(result), Atoms{family(source)});
    EXPECT_EQ(result.precision, EvidencePrecision::ExactForRecordedModel);
    EXPECT_EQ(task.stats().cyclic_components, 1U);
    EXPECT_EQ(graph.query(ar).sources.size(), 1U);
}

TEST(RecursiveSummary, RecursiveInternalSourcesAndEffectsRetainFiniteCycles) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    const auto source = builder.add(Rule::Origin, 1);
    const auto effect = builder.add(Rule::Effect, 2);
    const auto recurse = builder.call(summary, {}, 3);
    const auto root =
        builder.add(Rule::Sequence, 4,
                    std::array{Edge{source}, Edge{effect, Relation::Effect},
                               Edge{recurse}});
    builder.define_summary(summary, root);
    const auto graph = std::move(builder).freeze();
    for (const auto kind : {QueryKind::Value, QueryKind::Effects}) {
        RecursiveSummaryTask task(EvidenceView::memory(graph), summary, kind);
        const auto result = finish(task);
        EXPECT_EQ(atoms(result),
                  Atoms{family(kind == QueryKind::Value ? source : effect)});
        EXPECT_EQ(result.precision, EvidencePrecision::Conservative);
        EXPECT_TRUE(result.instances_merged);
        EXPECT_EQ(result.issues, 0U);
        EXPECT_EQ(task.stats().cyclic_components, 1U);
        EXPECT_LT(task.stats().nodes, 6U);
        EXPECT_EQ(atoms(result), unfold(graph, root, kind, {}, 5));
    }
}

TEST(RecursiveSummary, ConstantValueDoesNotEraseRecursiveWorkOrEffects) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    const auto effect = builder.add(Rule::Effect, 1);
    const auto root = builder.add(
        Rule::Sequence, 4,
        std::array{Edge{builder.add(Rule::Literal, 2)},
                   Edge{builder.call(summary, {}, 3), Relation::Initializer},
                   Edge{effect, Relation::Effect}});
    builder.define_summary(summary, root);
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask value(EvidenceView::memory(graph), summary);
    EXPECT_TRUE(finish(value).atoms.empty());
    EXPECT_EQ(value.result().precision,
              EvidencePrecision::ExactForRecordedModel);
    RecursiveSummaryTask effects(EvidenceView::memory(graph), summary,
                                 QueryKind::Effects);
    EXPECT_EQ(atoms(finish(effects)), Atoms{family(effect)});
    EXPECT_EQ(effects.result().precision, EvidencePrecision::Conservative);
    EXPECT_EQ(effects.stats().cyclic_components, 1U);
}

TEST(RecursiveSummary, UnusedValueArgumentsDoNotContaminateCoverage) {
    Builder builder;
    const auto outer = builder.reserve_summary(),
               inner = builder.reserve_summary();
    builder.define_summary(inner, builder.parameter(inner, 0, 1));
    const auto used = builder.parameter(outer, 2, 2);
    const auto gap = builder.add(Rule::Incomplete, 3);
    builder.define_summary(outer,
                           builder.call(inner, std::array{used, gap}, 4));
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask value(EvidenceView::memory(graph), outer);
    const auto exact = finish(value);
    EXPECT_EQ(atoms(exact), Atoms{parameter(outer, 2)});
    EXPECT_EQ(exact.precision, EvidencePrecision::ExactForRecordedModel);
    RecursiveSummaryTask effects(EvidenceView::memory(graph), outer,
                                 QueryKind::Effects);
    EXPECT_EQ(finish(effects).precision, EvidencePrecision::Unresolved);
    EXPECT_NE(effects.result().issues &
                  static_cast<unsigned>(RecursiveIssue::IncompleteNode),
              0U);
}

TEST(RecursiveSummary,
     MissingBindingsSummariesAndUnsupportedRulesStayUnresolved) {
    for (const auto issue :
         {RecursiveIssue::IncompleteNode, RecursiveIssue::MissingSummary,
          RecursiveIssue::UnboundParameter, RecursiveIssue::ForeignScope,
          RecursiveIssue::UnsupportedRule,
          RecursiveIssue::UnsupportedRelation}) {
        Builder builder;
        const auto selected = builder.reserve_summary(),
                   other = builder.reserve_summary();
        const auto foreign = builder.parameter(other, 0, 1);
        builder.define_summary(other, foreign);
        NodeId root;
        switch (issue) {
        case RecursiveIssue::IncompleteNode:
            root = builder.add(Rule::Incomplete, 2);
            break;
        case RecursiveIssue::MissingSummary:
            root = builder.add(Rule::Call, 2, {}, 999);
            break;
        case RecursiveIssue::UnboundParameter:
            root = builder.call(other, {}, 2);
            break;
        case RecursiveIssue::ForeignScope:
            root = builder.summary_ref(other, 2);
            break;
        case RecursiveIssue::UnsupportedRule:
            root = builder.add(static_cast<Rule>(255), 2);
            break;
        case RecursiveIssue::UnsupportedRelation:
            root = builder.add(Rule::Use, 2,
                               std::array{Edge{builder.add(Rule::Literal, 1),
                                               static_cast<Relation>(255)}});
            break;
        default:
            FAIL();
        }
        builder.define_summary(selected, root);
        const auto graph = std::move(builder).freeze();
        RecursiveSummaryTask task(EvidenceView::memory(graph), selected);
        const auto result = finish(task);
        EXPECT_TRUE(result.fixed_point);
        EXPECT_EQ(result.precision, EvidencePrecision::Unresolved);
        EXPECT_NE(result.issues & static_cast<unsigned>(issue), 0U);
    }
}

TEST(RecursiveSummary, CancellationAtEveryPhasePreservesWorkAndWitnesses) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    const auto x = builder.parameter(summary, 0, 1);
    builder.define_summary(
        summary,
        builder.add(Rule::Strict, 3,
                    std::array{Edge{x},
                               Edge{builder.call(summary, std::array{x}, 2)}}));
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask baseline(EvidenceView::memory(graph), summary);
    const auto expected = finish(baseline, 100000);
    for (std::size_t cut = 0; cut < baseline.stats().steps; ++cut) {
        RecursiveSummaryTask task(EvidenceView::memory(graph), summary);
        EXPECT_EQ(task.advance(0), QueryProgress::Paused);
        EXPECT_EQ(task.stats().steps, 0U);
        (void)task.advance(cut);
        const auto work = task.stats().steps;
        std::stop_source cancellation;
        cancellation.request_stop();
        EXPECT_EQ(task.advance(100000, cancellation.get_token()),
                  QueryProgress::Cancelled);
        EXPECT_EQ(task.stats().steps, work);
        task.cancel();
        EXPECT_EQ(task.advance(), QueryProgress::Cancelled);
        EXPECT_FALSE(task.result().fixed_point);
        EXPECT_EQ(task.result().precision, EvidencePrecision::Unresolved);
        task.resume();
        EXPECT_EQ(finish(task, 7), expected);
        EXPECT_EQ(task.stats().steps, baseline.stats().steps);
        EXPECT_EQ(task.stats().evaluations, baseline.stats().evaluations);
        EXPECT_EQ(task.stats().facts_added, baseline.stats().facts_added);
        EXPECT_EQ(
            std::vector(task.links().begin(), task.links().end()),
            std::vector(baseline.links().begin(), baseline.links().end()));
        EXPECT_EQ(
            std::vector(task.components().begin(), task.components().end()),
            std::vector(baseline.components().begin(),
                        baseline.components().end()));
    }
}

TEST(RecursiveSummary,
     SourceFamiliesFromDifferentCallSitesAreNotConcreteInstances) {
    Builder builder;
    const auto outer = builder.reserve_summary(),
               inner = builder.reserve_summary();
    const auto source = builder.add(Rule::Origin, 1);
    builder.define_summary(inner, source);
    const auto a = builder.call(inner, {}, 2), b = builder.call(inner, {}, 3);
    const auto root =
        builder.add(Rule::Strict, 4, std::array{Edge{a}, Edge{b}});
    builder.define_summary(outer, root);
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask task(EvidenceView::memory(graph), outer);
    const auto result = finish(task);
    EXPECT_EQ(atoms(result), Atoms{family(source)});
    EXPECT_EQ(result.precision, EvidencePrecision::Conservative);
    EXPECT_FALSE(result.concrete_sources_complete);
    const auto concrete = graph.query(root);
    ASSERT_EQ(concrete.sources.size(), 2U);
    EXPECT_NE(concrete.sources[0].context, concrete.sources[1].context);
    // The grammar retains both call sites even when its family set merges them.
    EXPECT_NE(
        std::ranges::find(task.links(),
                          RecursiveLink{a, source, RecursiveLinkKind::Summary}),
        task.links().end());
    EXPECT_NE(
        std::ranges::find(task.links(),
                          RecursiveLink{b, source, RecursiveLinkKind::Summary}),
        task.links().end());
}

TEST(RecursiveSummary, DeepSummaryCyclesDoNotUseTheNativeStack) {
    Builder builder;
    std::vector<SummaryId> summaries;
    for (unsigned i = 0; i < 10000; ++i)
        summaries.push_back(builder.reserve_summary());
    for (unsigned i = 0; i < summaries.size(); ++i)
        builder.define_summary(
            summaries[i],
            builder.summary_ref(summaries[(i + 1) % summaries.size()], i + 1));
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask task(EvidenceView::memory(graph), summaries[0]);
    const auto result = finish(task, 4096);
    EXPECT_TRUE(result.atoms.empty());
    EXPECT_EQ(result.precision, EvidencePrecision::ExactForRecordedModel);
    ASSERT_EQ(task.components().size(), 1U);
    EXPECT_EQ(task.components()[0].summaries.size(), summaries.size());
    EXPECT_TRUE(task.components()[0].cyclic);
}

TEST(RecursiveSummary, SharedInnerCallRetainsBothOuterContextPaths) {
    Builder builder;
    const auto outer = builder.reserve_summary();
    const auto middle = builder.reserve_summary();
    const auto leaf = builder.reserve_summary();
    const auto source = builder.add(Rule::Origin, 1);
    builder.define_summary(leaf, source);
    const auto shared = builder.call(leaf, {}, 2);
    builder.define_summary(middle, shared);
    const auto a = builder.call(middle, {}, 3);
    const auto b = builder.call(middle, {}, 4);
    const auto root =
        builder.add(Rule::Strict, 5, std::array{Edge{a}, Edge{b}});
    builder.define_summary(outer, root);
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask task(EvidenceView::memory(graph), outer);
    const auto result = finish(task);
    EXPECT_EQ(atoms(result), Atoms{family(source)});
    EXPECT_EQ(result.precision, EvidencePrecision::Conservative);
    const auto concrete = graph.query(root);
    ASSERT_EQ(concrete.sources.size(), 2U);
    const auto left = concrete.invocations[concrete.sources[0].context];
    const auto right = concrete.invocations[concrete.sources[1].context];
    EXPECT_EQ(left.call, shared);
    EXPECT_EQ(right.call, shared);
    EXPECT_NE(left.parent, right.parent);
    for (const auto call : {a, b})
        EXPECT_NE(std::ranges::find(
                      task.links(),
                      RecursiveLink{call, shared, RecursiveLinkKind::Summary}),
                  task.links().end());
}

TEST(RecursiveSummary,
     ProjectionUsesRecordedValueControlAndInitializerRelations) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    const auto selected = builder.parameter(summary, 5, 1);
    const auto control = builder.parameter(summary, UINT32_MAX, 2);
    const auto initializer = builder.add(Rule::Effect, 3);
    const auto root =
        builder.add(Rule::Projection, 4,
                    std::array{Edge{selected}, Edge{control, Relation::Control},
                               Edge{initializer, Relation::Initializer}});
    builder.define_summary(summary, root);
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask value(EvidenceView::memory(graph), summary);
    EXPECT_EQ(atoms(finish(value)),
              (Atoms{parameter(summary, 5), parameter(summary, UINT32_MAX)}));
    EXPECT_EQ(value.result().precision,
              EvidencePrecision::ExactForRecordedModel);
    RecursiveSummaryTask effects(EvidenceView::memory(graph), summary,
                                 QueryKind::Effects);
    EXPECT_EQ(atoms(finish(effects)),
              (Atoms{parameter(summary, 5), parameter(summary, UINT32_MAX),
                     family(initializer)}));
    EXPECT_EQ(effects.result().precision,
              EvidencePrecision::ExactForRecordedModel);
}

TEST(RecursiveSummary, SegmentedEvidenceAndReadFailuresAreExplicit) {
    Builder builder;
    const auto summary = builder.reserve_summary();
    const auto p = builder.parameter(summary, 0, 1);
    const auto body = builder.add(
        Rule::Strict, 3,
        std::array{Edge{p}, Edge{builder.call(summary, std::array{p}, 2)}});
    builder.define_summary(summary, body);
    const auto graph = std::move(builder).freeze();
    RecursiveSummaryTask baseline(EvidenceView::memory(graph), summary);
    const auto expected = finish(baseline);
    for (const bool fail : {false, true}) {
        std::size_t reads = 0;
        std::vector<EvidenceBlockRef> directory;
        for (unsigned i = 1; i <= graph.stats().nodes; ++i)
            directory.push_back(
                {{i}, 1, {.kind = pagos::cache::ObjectKind::EvidenceBlock}});
        auto view = EvidenceView::segmented(
            std::move(directory),
            {graph.summaries().begin(), graph.summaries().end()},
            [&, graph](std::size_t index)
                -> std::expected<std::shared_ptr<const EvidenceBlock>,
                                 EvidenceError> {
                ++reads;
                if (fail && index == p.value - 1)
                    return std::unexpected(
                        EvidenceError{EvidenceErrorCode::MissingBlock, {}, {}});
                auto block = std::make_shared<EvidenceBlock>();
                const NodeId id{static_cast<std::uint32_t>(index + 1)};
                auto node = graph.node(id);
                node.edge_begin = 0;
                block->nodes.push_back(node);
                const auto edges = graph.edges(id);
                block->edges.assign(edges.begin(), edges.end());
                return block;
            },
            0);
        ASSERT_TRUE(view);
        RecursiveSummaryTask task(*view, summary);
        while (task.progress() == QueryProgress::Running ||
               task.progress() == QueryProgress::Paused)
            (void)task.advance(1);
        EXPECT_GT(reads, 0U);
        EXPECT_EQ((*view)->memory_graph(), nullptr);
        if (fail) {
            EXPECT_EQ(task.progress(), QueryProgress::Failed);
            ASSERT_TRUE(task.failure());
            EXPECT_EQ(task.failure()->code, EvidenceErrorCode::MissingBlock);
            EXPECT_FALSE(task.result().fixed_point);
            EXPECT_EQ(task.result().precision, EvidencePrecision::Unresolved);
            EXPECT_NE(
                task.result().issues &
                    static_cast<unsigned>(RecursiveIssue::UnavailableEvidence),
                0U);
            task.resume();
            EXPECT_EQ(task.advance(), QueryProgress::Failed);
        } else
            EXPECT_EQ(task.result(), expected);
    }
    EXPECT_THROW(RecursiveSummaryTask(nullptr, summary), std::out_of_range);
    EXPECT_THROW(RecursiveSummaryTask(EvidenceView::memory(graph), {}),
                 std::out_of_range);
}
} // namespace
