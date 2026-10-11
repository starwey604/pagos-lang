#include "pagos/stage/provenance_prototype.h"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>

namespace {
using namespace pagos::stage::prototype;

std::vector<SiteId> sites(const QueryResult& result) {
    std::vector<SiteId> output;
    for (const auto& source : result.sources)
        output.push_back(source.site);
    return output;
}

TEST(Provenance, KeepsAllDataControlAndContractOrigins) {
    for (const auto storage : {Storage::Append, Storage::SelectiveIntern}) {
        Store store(storage);
        const auto revision = store.add(Rule::Origin, 1);
        const auto baud = store.add(Rule::Origin, 2);
        const auto clock = store.add(
            Rule::Select, 3, std::array{Edge{revision, Relation::Control}});
        const auto divisor =
            store.add(Rule::Strict, 4, std::array{Edge{clock}, Edge{baud}});
        const auto required =
            store.add(Rule::RequireComptime, 5,
                      std::array{Edge{divisor, Relation::Contract}});
        const auto result = store.query(required);
        EXPECT_TRUE(result.complete);
        EXPECT_EQ(sites(result), (std::vector<SiteId>{1, 2}));
        const auto witness = store.query(required, QueryKind::Value, true);
        ASSERT_EQ(witness.witness.size(), 3U);
        EXPECT_EQ(witness.witness.back().node, baud);
    }
}

TEST(Provenance, SharesTopologyAndVisitsEachStateOnce) {
    Store store;
    const auto source = store.add(Rule::Origin, 1);
    const auto left = store.add(Rule::Use, 2, std::array{Edge{source}});
    const auto right = store.add(Rule::Use, 3, std::array{Edge{source}});
    const auto joined =
        store.add(Rule::Strict, 4, std::array{Edge{left}, Edge{right}});
    const auto result = store.query(joined);
    EXPECT_EQ(result.sources.size(), 1U);
    EXPECT_EQ(result.visited_states, 4U);
    EXPECT_EQ(result.visited_edges, 4U);
}

TEST(Provenance, RespectsExplicitBoundaryAndMemberPrecision) {
    Store store;
    const auto fixed = store.add(Rule::Literal, 1, {}, 7);
    const auto dynamic = store.add(Rule::Origin, 2);
    const auto record =
        store.add(Rule::Aggregate, 3, std::array{Edge{fixed}, Edge{dynamic}});
    const auto known = store.project(record, 0, 4);
    const auto unknown = store.project(record, 1, 5);
    EXPECT_TRUE(store.query(known).sources.empty());
    EXPECT_EQ(sites(store.query(unknown)), (std::vector<SiteId>{2}));
    const auto alias = store.add(Rule::Use, 8, std::array{Edge{record}});
    EXPECT_TRUE(store.query(store.project(alias, 0, 9)).sources.empty());
    const auto boundary =
        store.add(Rule::RuntimeBoundary, 6,
                  std::array{Edge{record, Relation::Initializer}});
    EXPECT_EQ(sites(store.query(store.project(boundary, 0, 7))),
              (std::vector<SiteId>{6}));
    EXPECT_THROW((void)store.project(record, 2, 8), std::out_of_range);
}

TEST(Provenance, SeparatesKnownValueFromArgumentAndMemberEffects) {
    Store store;
    const auto fixed = store.add(Rule::Literal, 1, {}, 7);
    const auto write = store.add(Rule::Effect, 2);
    const auto sequence =
        store.add(Rule::Sequence, 3,
                  std::array{Edge{fixed}, Edge{write, Relation::Effect}});
    EXPECT_TRUE(store.query(sequence).sources.empty());
    EXPECT_EQ(sites(store.query(sequence, QueryKind::Effects)),
              (std::vector<SiteId>{2}));
    const auto record =
        store.add(Rule::Aggregate, 4, std::array{Edge{fixed}, Edge{sequence}});
    const auto projected = store.project(record, 0, 5);
    EXPECT_TRUE(store.query(projected).sources.empty());
    EXPECT_EQ(sites(store.query(projected, QueryKind::Effects)),
              (std::vector<SiteId>{2}));
    const auto wrapped =
        store.add(Rule::Sequence, 8,
                  std::array{Edge{record}, Edge{write, Relation::Effect}});
    const auto wrapped_projection = store.project(wrapped, 0, 9);
    EXPECT_TRUE(store.query(wrapped_projection).sources.empty());
    EXPECT_EQ(sites(store.query(wrapped_projection, QueryKind::Effects)),
              (std::vector<SiteId>{2}));

    const auto first = store.reserve_summary();
    store.define_summary(first, store.parameter(first, 0, 6));
    const auto invocation = store.call(first, std::array{fixed, sequence}, 7);
    EXPECT_TRUE(store.query(invocation).sources.empty());
    EXPECT_EQ(sites(store.query(invocation, QueryKind::Effects)),
              (std::vector<SiteId>{2}));
}

TEST(Provenance, RebindsCachedSummaryToEachCallAndHandlesNestedCalls) {
    Store store;
    const auto inner = store.reserve_summary();
    store.define_summary(inner, store.parameter(inner, 0, 1));
    const auto outer = store.reserve_summary();
    const auto parameter = store.parameter(outer, 0, 2);
    store.define_summary(outer, store.call(inner, std::array{parameter}, 3));
    const auto first = store.add(Rule::Origin, 4);
    const auto second = store.add(Rule::Origin, 5);
    const auto call_a = store.call(outer, std::array{first}, 6);
    const auto call_b = store.call(outer, std::array{second}, 7);
    EXPECT_EQ(sites(store.query(call_a)), (std::vector<SiteId>{4}));
    EXPECT_EQ(sites(store.query(call_b)), (std::vector<SiteId>{5}));
    const auto both =
        store.add(Rule::Aggregate, 8, std::array{Edge{call_a}, Edge{call_b}});
    const auto result = store.query(both);
    EXPECT_EQ(sites(result), (std::vector<SiteId>{4, 5}));
    EXPECT_EQ(result.contexts, 4U);
    EXPECT_TRUE(result.complete);
}

TEST(Provenance, DoesNotMergeTwoCallsToAnInternalSourceOrEffect) {
    Store store(Storage::SelectiveIntern);
    const auto summary = store.reserve_summary();
    const auto read = store.add(Rule::Origin, 1);
    const auto write = store.add(Rule::Effect, 2);
    store.define_summary(
        summary,
        store.add(Rule::Sequence, 3,
                  std::array{Edge{read}, Edge{write, Relation::Effect}}));
    const auto call_a = store.call(summary, {}, 4);
    const auto call_b = store.call(summary, {}, 5);
    const auto both =
        store.add(Rule::Aggregate, 6, std::array{Edge{call_a}, Edge{call_b}});
    const auto result = store.query(both);
    ASSERT_EQ(result.sources.size(), 2U);
    EXPECT_NE(result.sources[0].context, result.sources[1].context);
    ASSERT_EQ(result.invocations.size(), 3U);
    EXPECT_EQ(result.invocations[result.sources[0].context].call, call_a);
    EXPECT_EQ(result.invocations[result.sources[1].context].call, call_b);
    const auto effects = store.query(both, QueryKind::Effects);
    ASSERT_EQ(effects.sources.size(), 2U);
    EXPECT_NE(effects.sources[0].context, effects.sources[1].context);
}

TEST(Provenance, TerminatesFiniteSummaryCyclesAndMarksRecursiveCallsPartial) {
    Store store;
    const auto first = store.reserve_summary();
    const auto second = store.reserve_summary();
    const auto source_a = store.add(Rule::Origin, 1);
    const auto source_b = store.add(Rule::Origin, 2);
    const auto ref_a = store.summary_ref(first, 3);
    const auto ref_b = store.summary_ref(second, 4);
    store.define_summary(
        first,
        store.add(Rule::Strict, 5, std::array{Edge{source_a}, Edge{ref_b}}));
    store.define_summary(
        second,
        store.add(Rule::Strict, 6, std::array{Edge{source_b}, Edge{ref_a}}));
    const auto result = store.query(ref_a);
    EXPECT_TRUE(result.complete);
    EXPECT_EQ(sites(result), (std::vector<SiteId>{1, 2}));
    EXPECT_EQ(result.visited_states, 6U);

    const auto recursive = store.reserve_summary();
    const auto parameter = store.parameter(recursive, 0, 7);
    const auto backedge = store.call(recursive, std::array{parameter}, 8);
    store.define_summary(
        recursive, store.add(Rule::Strict, 9,
                             std::array{Edge{parameter}, Edge{backedge}}));
    const auto call = store.call(recursive, std::array{source_a}, 10);
    const auto partial = store.query(call);
    EXPECT_FALSE(partial.complete);
    EXPECT_TRUE(partial.recursive_call_cut);
    EXPECT_EQ(sites(partial), (std::vector<SiteId>{1}));
}

TEST(Provenance, ReportsBudgetsUnboundParametersAndUnpublishedSummaries) {
    Store store;
    const auto source = store.add(Rule::Origin, 1);
    const auto use = store.add(Rule::Use, 2, std::array{Edge{source}});
    EXPECT_FALSE(store.query(use, QueryKind::Value, false, 1).complete);
    EXPECT_FALSE(store.query(use, QueryKind::Value, false, 0).complete);
    const auto pending = store.reserve_summary();
    EXPECT_FALSE(store.query(store.summary_ref(pending, 3)).complete);
    EXPECT_FALSE(store.query(store.parameter(pending, 0, 4)).complete);
    store.define_summary(pending, store.parameter(pending, 1, 5));
    EXPECT_FALSE(
        store.query(store.call(pending, std::array{source}, 6)).complete);
    EXPECT_THROW(store.define_summary(pending, source), std::logic_error);
    EXPECT_THROW((void)store.node({}), std::out_of_range);
    EXPECT_THROW((void)store.parameter({}, 0, 7), std::out_of_range);
}

TEST(Provenance, InternsOnlyEligibleEvidenceAndPreservesSitesAndEdgeKinds) {
    Store store(Storage::SelectiveIntern);
    const auto source = store.add(Rule::Origin, 1);
    const std::array edges{Edge{source}};
    const auto first =
        store.add(Rule::Strict, 2, edges, 0, {}, Sharing::Eligible);
    EXPECT_EQ(first,
              store.add(Rule::Strict, 2, edges, 0, {}, Sharing::Eligible));
    EXPECT_NE(first,
              store.add(Rule::Strict, 3, edges, 0, {}, Sharing::Eligible));
    EXPECT_NE(first, store.add(Rule::Strict, 2,
                               std::array{Edge{source, Relation::Control}}, 0,
                               {}, Sharing::Eligible));
    EXPECT_NE(source, store.add(Rule::Origin, 1, {}, 0, {}, Sharing::Eligible));
    EXPECT_EQ(store.stats().intern_hits, 1U);
}

TEST(Provenance, SurvivesVectorGrowthAndOwnsEvidenceIndependently) {
    Store store;
    const auto source = store.add(Rule::Origin, 1);
    auto last = source;
    for (SiteId site = 2; site < 4098; ++site)
        last = store.add(Rule::Use, site, std::array{Edge{last}});
    const auto copy = store.add(Rule::Strict, 4098, store.edges(last));
    EXPECT_EQ(store.query(copy).sources.front().node, source);
    const auto stats = store.stats();
    EXPECT_EQ(stats.nodes, 4098U);
    EXPECT_EQ(stats.edges, 4097U);
    Store next;
    (void)next.add(Rule::Origin, 99);
    EXPECT_EQ(store.query(last).sources.front().site, 1U);
    auto moved = std::move(store);
    EXPECT_EQ(moved.query(last).sources.front().site, 1U);
}

} // namespace
