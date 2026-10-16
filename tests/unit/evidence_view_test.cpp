#include "cache_test_input.h"
#include "pagos/stage/evidence_view.h"
#include "pagos/stage/provenance_query.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

namespace {
using namespace pagos;
using namespace pagos::stage::provenance;
namespace fs = std::filesystem;
struct FixtureGraph {
    FrozenGraph graph;
    NodeId a_end;
    NodeId b_end;
    NodeId root;
};
FixtureGraph graph_fixture() {
    Builder builder;
    const auto inner = builder.reserve_summary();
    const auto outer = builder.reserve_summary();
    const auto param = builder.parameter(inner, 0, 1);
    const auto effect = builder.add(Rule::Effect, 2);
    builder.define_summary(
        inner,
        builder.add(Rule::Sequence, 3,
                    std::array{Edge{param}, Edge{effect, Relation::Effect}}));
    const auto a_end =
        builder.call(inner, std::array{builder.parameter(outer, 0, 4)}, 5);
    builder.define_summary(outer, a_end);
    auto b_end = builder.add(Rule::Origin, 6);
    for (unsigned index = 0; index < 8192; ++index)
        b_end = builder.add(Rule::Strict, 7, std::array{Edge{b_end}});
    const auto a = builder.add(Rule::RuntimeBoundary, 8);
    const auto b = builder.add(Rule::Origin, 9);
    const auto unused_effect = builder.add(Rule::Effect, 10);
    const auto first = builder.call(outer, std::array{a, unused_effect}, 11);
    const auto second = builder.call(outer, std::array{b, unused_effect}, 12);
    const auto root =
        builder.add(Rule::Strict, 13, std::array{Edge{first}, Edge{second}});
    return {std::move(builder).freeze(), a_end, b_end, root};
}
QueryResult finish(QueryTask& task) {
    while (task.progress() != QueryProgress::Completed &&
           task.progress() != QueryProgress::Failed)
        (void)task.advance({1, 1, 1, 1});
    return task.result();
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
class EvidenceViewTest : public ::testing::Test {
  protected:
    EvidenceViewTest() {
        static std::atomic<unsigned> sequence{};
        directory =
            fs::temp_directory_path() /
            ("pagos-evidence-test-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    }
    ~EvidenceViewTest() override {
        std::error_code error;
        fs::remove_all(directory, error);
    }
    void publish(const FixtureGraph& fixture,
                 std::optional<cache::Bytes> a = {}) {
        const std::array artifacts{
            cache::ArtifactInput{
                "a", cache::ObjectKind::EvidenceBlock,
                a ? std::move(*a)
                  : *encode_evidence_block(fixture.graph, {1},
                                           fixture.a_end.value)},
            cache::ArtifactInput{
                "b", cache::ObjectKind::EvidenceBlock,
                *encode_evidence_block(fixture.graph, {fixture.a_end.value + 1},
                                       fixture.b_end.value -
                                           fixture.a_end.value)},
            cache::ArtifactInput{
                "calls", cache::ObjectKind::EvidenceBlock,
                *encode_evidence_block(fixture.graph, {fixture.b_end.value + 1},
                                       fixture.root.value -
                                           fixture.b_end.value)}};
        cache::FileStore store(directory);
        ASSERT_TRUE(store.publish(key, inputs, semantic, artifacts));
        const auto refs = store.lookup_references(key, inputs);
        ASSERT_TRUE(refs);
        blocks = {{{1}, fixture.a_end.value, refs->artifacts[0].object},
                  {{fixture.a_end.value + 1},
                   fixture.b_end.value - fixture.a_end.value,
                   refs->artifacts[1].object},
                  {{fixture.b_end.value + 1},
                   fixture.root.value - fixture.b_end.value,
                   refs->artifacts[2].object}};
    }
    auto open(const FixtureGraph& fixture, std::size_t bytes = 4 * 1024 * 1024,
              cache::StoreOptions options = {}) {
        const auto summaries = fixture.graph.summaries();
        return EvidenceView::disk(
            cache::FileStore(directory, std::move(options)), blocks,
            {summaries.begin(), summaries.end()}, bytes);
    }
    fs::path object(std::size_t index) const {
        const auto hash = blocks[index].object.digest.hex();
        return directory / "objects" / hash.substr(0, 2) / hash;
    }
    fs::path directory;
    cache::QueryKey key{cache_test::key()};
    cache::Digest inputs{cache::fingerprint("inputs", {})};
    cache::Digest semantic{cache::fingerprint("test-block-directory", {})};
    std::vector<EvidenceBlockRef> blocks;
};

TEST_F(EvidenceViewTest, QueryReadsOnlyRelatedBlocksAndReusesLoadedObjects) {
    const auto fixture = graph_fixture();
    publish(fixture);
    auto disk = open(fixture);
    ASSERT_TRUE(disk);
    EXPECT_EQ((*disk)->read_stats().objects_read, 0U);
    QueryTask zero(*disk, fixture.root);
    EXPECT_EQ(zero.advance({0, 0, 0, 0}).progress, QueryProgress::Paused);
    EXPECT_EQ((*disk)->read_stats().objects_read, 0U);
    for (const auto kind : {QueryKind::Value, QueryKind::Effects}) {
        for (bool witness : {false, true}) {
            QueryTask task(*disk, fixture.root, kind, witness);
            equivalent(finish(task),
                       fixture.graph.query(fixture.root, kind, witness));
            EXPECT_EQ(task.progress(), QueryProgress::Completed);
            EXPECT_FALSE(task.failure());
            EXPECT_EQ(task.coverage().issues, 0U);
            EXPECT_EQ(task.evidence().memory_graph(), nullptr);
            EXPECT_THROW((void)task.graph(), std::logic_error);
        }
    }
    const auto stats = (*disk)->read_stats();
    EXPECT_EQ(stats.objects_read, 2U);
    EXPECT_EQ(stats.payload_bytes_read, 334U);
    EXPECT_EQ(stats.payload_bytes_read,
              blocks[0].object.size + blocks[2].object.size);
    EXPECT_LT(stats.payload_bytes_read, blocks[1].object.size / 100);
    EXPECT_GT(stats.cache_hits, 0U);
    fs::remove(object(1)); // Unrelated required block: directory access is NOT
                           // a semantic cache hit.
    QueryTask task(*disk, fixture.root);
    equivalent(finish(task), fixture.graph.query(fixture.root));
    EXPECT_EQ((*disk)->read_stats().objects_read, 2U);
}

TEST_F(EvidenceViewTest, MemoryLeaseKeepsOriginalReferenceAndSpanContract) {
    const auto fixture = graph_fixture();
    auto view = EvidenceView::memory(fixture.graph);
    const auto lease = view->pin(fixture.root);
    ASSERT_TRUE(lease);
    EXPECT_EQ(&lease->node(), &fixture.graph.node(fixture.root));
    EXPECT_EQ(lease->edges().data(), fixture.graph.edges(fixture.root).data());
    auto task = QueryTask(view, fixture.root);
    view.reset();
    equivalent(finish(task), fixture.graph.query(fixture.root));
    EXPECT_EQ(&task.graph().node(fixture.root), &lease->node());
}

TEST_F(EvidenceViewTest, LeaseSurvivesEvictionViewDestructionAndFileDeletion) {
    const auto fixture = graph_fixture();
    publish(fixture);
    auto disk = open(fixture);
    ASSERT_TRUE(disk);
    auto view = *disk;
    auto lease = view->pin(fixture.root);
    ASSERT_TRUE(lease);
    const auto* address = &lease->node();
    view->evict();
    EXPECT_EQ(view->read_stats().cached_bytes, 0U);
    EXPECT_GT(view->read_stats().live_bytes, 0U);
    fs::remove_all(directory);
    const auto again = view->pin(fixture.root);
    ASSERT_TRUE(again);
    EXPECT_EQ(&again->node(), address);
    EXPECT_EQ(view->read_stats().objects_read, 1U);
    disk->reset();
    view.reset();
    EXPECT_EQ(lease->node().rule, Rule::Strict);
    EXPECT_EQ(lease->edges().size(), 2U);
}

TEST_F(EvidenceViewTest,
       ZeroCacheBudgetStillSupportsCrossBlockBindingsAndSmallSlices) {
    const auto fixture = graph_fixture();
    publish(fixture);
    auto disk = open(fixture, 0);
    ASSERT_TRUE(disk);
    QueryTask task(*disk, fixture.root);
    equivalent(finish(task), fixture.graph.query(fixture.root));
    EXPECT_EQ(task.progress(), QueryProgress::Completed);
    EXPECT_EQ((*disk)->read_stats().cached_bytes, 0U);
    EXPECT_EQ((*disk)->read_stats().live_bytes, 0U);
}

TEST_F(EvidenceViewTest, EvictingPausedQueryDoesNotInvalidateItsEdgeCursor) {
    const auto fixture = graph_fixture();
    publish(fixture);
    auto disk = open(fixture, 0);
    ASSERT_TRUE(disk);
    QueryTask task(*disk, fixture.root);
    EXPECT_EQ(task.advance({1, 0, 0, 0}).progress, QueryProgress::Paused);
    (*disk)->evict();
    fs::remove(object(2));
    EXPECT_EQ(task.advance({0, 1, 0, 0}).progress, QueryProgress::Paused);
    EXPECT_EQ(task.stats().visited_edges, 1U);
    EXPECT_FALSE(task.failure());
    // A different node from the evicted block can still borrow the active
    // lease.
    EXPECT_TRUE((*disk)->pin(fixture.root));
}

TEST_F(EvidenceViewTest,
       MissingRelatedBlockFailsWithoutClaimingCompleteCoverage) {
    const auto fixture = graph_fixture();
    publish(fixture);
    fs::remove(object(0));
    auto disk = open(fixture);
    ASSERT_TRUE(disk); // Directory opening never silently scans cold objects.
    QueryTask task(*disk, fixture.root);
    (void)finish(task);
    ASSERT_TRUE(task.failure());
    EXPECT_EQ(task.failure()->code, EvidenceErrorCode::MissingBlock);
    EXPECT_EQ(task.progress(), QueryProgress::Failed);
    EXPECT_FALSE(task.result().complete);
    EXPECT_FALSE(task.coverage().all_sources);
    EXPECT_NE(task.coverage().issues & static_cast<std::uint32_t>(
                                           CoverageIssue::UnavailableEvidence),
              0U);
    EXPECT_EQ(task.advance().details, DetailAvailability::Unavailable);
    task.resume();
    EXPECT_EQ(task.progress(), QueryProgress::Failed);
}

TEST_F(EvidenceViewTest, CorruptCasObjectIsRejectedBeforeDecode) {
    const auto fixture = graph_fixture();
    publish(fixture);
    std::fstream file(object(0),
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(-1, std::ios::end);
    char byte{};
    file.read(&byte, 1);
    byte ^= 1;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
    file.close();
    auto disk = open(fixture);
    ASSERT_TRUE(disk);
    QueryTask task(*disk, fixture.root);
    (void)finish(task);
    ASSERT_TRUE(task.failure());
    EXPECT_EQ(task.failure()->code, EvidenceErrorCode::CorruptBlock);
}

TEST_F(EvidenceViewTest,
       BlockVersionOwnerAndEdgesAreValidatedAfterCasChecksum) {
    const auto fixture = graph_fixture();
    const auto original =
        *encode_evidence_block(fixture.graph, {1}, fixture.a_end.value);
    // Header 9, version/model/first/count 16, node 21. Offset 42 is parameter
    // owner.
    for (const auto [offset, byte] :
         std::array<std::pair<std::size_t, std::uint8_t>, 5>{
             {{9, 2},
              {42, 255},
              {25, 255},
              {fixture.a_end.value * 21 + 29, 255},
              {30, 255}}}) {
        SCOPED_TRACE(offset);
        fs::remove_all(directory);
        auto changed = original;
        ASSERT_LT(offset, changed.size());
        changed[offset] = byte;
        publish(fixture, changed);
        auto disk = open(fixture);
        ASSERT_TRUE(disk);
        const auto result = (*disk)->pin({1});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code,
                  offset == 9 ? EvidenceErrorCode::IncompatibleVersion
                              : EvidenceErrorCode::CorruptBlock);
    }
}

TEST_F(EvidenceViewTest, InvalidDirectoryAndSummaryRootsRequireNoDiskReads) {
    const auto fixture = graph_fixture();
    publish(fixture);
    blocks[1].first.value += 1;
    EXPECT_EQ(open(fixture).error().code, EvidenceErrorCode::InvalidReference);
    blocks[1].first.value -= 1;
    auto summaries = std::vector(fixture.graph.summaries().begin(),
                                 fixture.graph.summaries().end());
    summaries[0].value.value = fixture.root.value + 1;
    EXPECT_EQ(EvidenceView::disk(cache::FileStore(directory), blocks, summaries)
                  .error()
                  .code,
              EvidenceErrorCode::InvalidReference);
    blocks[0].object.kind = cache::ObjectKind::ProvenanceSnapshot;
    EXPECT_EQ(open(fixture).error().code, EvidenceErrorCode::InvalidReference);
}

TEST_F(EvidenceViewTest,
       ConcurrentQueriesAndEvictionSharePinnedImmutableBlocks) {
    const auto fixture = graph_fixture();
    publish(fixture);
    auto disk = open(fixture);
    ASSERT_TRUE(disk);
    const auto oracle = fixture.graph.query(fixture.root);
    std::array<QueryResult, 4> results;
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < results.size(); ++index)
        threads.emplace_back([&, index] {
            QueryTask task(*disk, fixture.root);
            while (task.progress() != QueryProgress::Completed &&
                   task.progress() != QueryProgress::Failed) {
                (void)task.advance({1, 1, 1, 1});
                (*disk)->evict();
            }
            results[index] = task.result();
        });
    for (auto& thread : threads)
        thread.join();
    for (const auto& result : results)
        equivalent(result, oracle);
}

TEST_F(EvidenceViewTest, CancellationBeforeAdmissionDoesNoIoAndCanResume) {
    const auto fixture = graph_fixture();
    publish(fixture);
    auto disk = open(fixture);
    ASSERT_TRUE(disk);
    QueryTask task(*disk, fixture.root);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_EQ(task.advance({}, stop.get_token()).progress,
              QueryProgress::Cancelled);
    EXPECT_EQ((*disk)->read_stats().objects_read, 0U);
    task.resume();
    equivalent(finish(task), fixture.graph.query(fixture.root));
}

TEST_F(EvidenceViewTest,
       ReadOnlyIoFailureIsAnEvidenceFailureAndDoesNotPublish) {
    const auto fixture = graph_fixture();
    publish(fixture);
    cache::StoreOptions options;
    unsigned mutations = 0;
    options.fault = [&](cache::IoOperation operation, const fs::path&) {
        if (operation != cache::IoOperation::ReadFile &&
            operation != cache::IoOperation::LockFile)
            ++mutations;
        return operation == cache::IoOperation::ReadFile
                   ? std::make_error_code(std::errc::permission_denied)
                   : std::error_code{};
    };
    auto disk = open(fixture, 4096, options);
    ASSERT_TRUE(disk);
    QueryTask task(*disk, fixture.root);
    (void)finish(task);
    ASSERT_TRUE(task.failure());
    EXPECT_EQ(task.failure()->code, EvidenceErrorCode::IoFailure);
    EXPECT_EQ(mutations, 0U);
}

TEST_F(EvidenceViewTest, FiniteRecursiveSummaryCycleMatchesRecordedCoverage) {
    Builder builder;
    const auto first = builder.reserve_summary();
    const auto second = builder.reserve_summary();
    const auto parameter = builder.parameter(second, 0, 1);
    const auto back = builder.call(first, std::array{parameter}, 2);
    builder.define_summary(second, back);
    const auto body =
        builder.call(second, std::array{builder.parameter(first, 0, 3)}, 4);
    builder.define_summary(first, body);
    const auto argument = builder.add(Rule::Origin, 5);
    const auto root = builder.call(first, std::array{argument}, 6);
    const FixtureGraph fixture{std::move(builder).freeze(), back, body, root};
    publish(fixture);
    auto disk = open(fixture);
    ASSERT_TRUE(disk);
    for (const auto kind : {QueryKind::Value, QueryKind::Effects}) {
        QueryTask task(*disk, root, kind);
        equivalent(finish(task), fixture.graph.query(root, kind));
        EXPECT_EQ(task.progress(), QueryProgress::Completed);
        EXPECT_FALSE(task.failure());
        EXPECT_EQ(task.coverage().precision, EvidencePrecision::Conservative);
        EXPECT_NE(task.coverage().issues &
                      static_cast<std::uint32_t>(CoverageIssue::RecursiveCall),
                  0U);
    }
}

TEST_F(EvidenceViewTest, DecodingLimitsAndInvalidReferencesAreExplicit) {
    const auto fixture = graph_fixture();
    publish(fixture);
    EXPECT_FALSE(encode_evidence_block(fixture.graph, {0}, 1));
    EXPECT_FALSE(encode_evidence_block(fixture.graph, fixture.root, 2));
    EXPECT_FALSE(encode_evidence_block(fixture.graph, {1}, 1, {.bytes = 9}));
    const auto summaries = std::vector(fixture.graph.summaries().begin(),
                                       fixture.graph.summaries().end());
    auto limited = EvidenceView::disk(cache::FileStore(directory), blocks,
                                      summaries, 0, {.bytes = 16});
    EXPECT_EQ(limited.error().code, EvidenceErrorCode::LimitExceeded);
    limited = EvidenceView::disk(cache::FileStore(directory), blocks, summaries,
                                 0, {.elements = 4});
    ASSERT_TRUE(
        limited); // Three directory entries fit; the five-node block does not.
    EXPECT_EQ((*limited)->pin({1}).error().code,
              EvidenceErrorCode::LimitExceeded);
    EXPECT_EQ((*limited)->pin({0}).error().code,
              EvidenceErrorCode::InvalidReference);
    EXPECT_EQ((*limited)->pin({fixture.root.value + 1}).error().code,
              EvidenceErrorCode::InvalidReference);
    EXPECT_THROW((void)QueryTask(*limited, {0}), std::out_of_range);
}

TEST_F(EvidenceViewTest, ColdBlockTruncationAndTrailingBytesAreRejected) {
    const auto fixture = graph_fixture();
    const auto original =
        *encode_evidence_block(fixture.graph, {1}, fixture.a_end.value);
    // Re-publication gives each malformed payload a valid CAS checksum.
    for (const auto length : {std::size_t{9}, std::size_t{24},
                              original.size() - 1, original.size() + 1}) {
        SCOPED_TRACE(length);
        fs::remove_all(directory);
        auto changed = original;
        changed.resize(length);
        publish(fixture, changed);
        auto disk = open(fixture);
        ASSERT_TRUE(disk);
        const auto result = (*disk)->pin({1});
        ASSERT_FALSE(result);
        EXPECT_TRUE(result.error().code == EvidenceErrorCode::CorruptBlock ||
                    result.error().code ==
                        EvidenceErrorCode::IncompatibleVersion);
    }
}
} // namespace
