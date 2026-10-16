#include "../../lib/cache/wire.h"
#include "cache_test_input.h"
#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/stage_analyzer.h"

#include <atomic>
#include <fstream>
#include <thread>

namespace {
using namespace pagos;
using namespace pagos::stage;
using namespace pagos::stage::provenance;
namespace fs = std::filesystem;
const std::string program =
    "fn inner(x: u32) -> u32 { x + external_input() } "
    "fn outer(x: u32) -> u32 { inner(x) } "
    "fn recur(x: u32) -> u32 { if x == 0 { x } else { recur(x - 1) } } "
    "let a = external_input(); let b = external_input(); "
    "let aa = outer(a) + outer(b); let rr = recur(a);";
const auto task_id = cache::fingerprint("test.logical-query.1", {});
class QueryCheckpointTest : public ::testing::Test {
  protected:
    QueryCheckpointTest() {
        static std::atomic<unsigned> sequence{};
        directory =
            fs::temp_directory_path() /
            ("pagos-query-checkpoint-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    }
    ~QueryCheckpointTest() override {
        std::error_code error;
        fs::remove_all(directory, error);
    }
    void publish(std::string text = program) {
        source_text = std::move(text);
        cache_test::Input input(source_text);
        ASSERT_TRUE(input.module);
        auto built = input.identity();
        ASSERT_TRUE(built);
        identity = std::move(*built);
        StageAnalyzer analyzer(input.diagnostics, input.checker.types(), {},
                               cache_test::context());
        const auto module = analyzer.analyze(*input.module);
        ASSERT_TRUE(module);
        ASSERT_FALSE(input.diagnostics.has_error());
        ASSERT_TRUE(FunctionEvidence::publish(
            cache::FileStore(directory / "evidence"), *module->provenance,
            identity, cache_test::context()));
    }
    FunctionEvidenceView open() {
        auto closure =
            FunctionEvidence::load(cache::FileStore(directory / "evidence"),
                                   identity, cache_test::context());
        if (!closure)
            throw std::runtime_error("closure missing");
        auto view = FunctionEvidenceView::open(std::move(*closure), 0);
        if (!view)
            throw std::runtime_error("view missing");
        return std::move(*view);
    }
    NodeId root(const FunctionEvidenceView& view,
                std::string_view name = "aa") {
        for (const auto& item : view.roots())
            if (item.name == name)
                return item.node;
        throw std::runtime_error("root missing");
    }
    QueryCheckpointBinding binding(const FunctionEvidenceView& view) {
        auto value = view.checkpoint_binding(source_text);
        if (!value)
            throw std::runtime_error("binding missing");
        return *value;
    }
    fs::path directory;
    std::string source_text;
    CacheIdentity identity;
};
void equal_result(const QueryTask& left, const QueryTask& right) {
    const auto a = left.result(), b = right.result();
    EXPECT_EQ(a.sources, b.sources);
    EXPECT_EQ(a.witness, b.witness);
    EXPECT_EQ(a.invocations, b.invocations);
    EXPECT_EQ(a.complete, b.complete);
    EXPECT_EQ(a.recursive_call_cut, b.recursive_call_cut);
    EXPECT_EQ(a.visited_states, b.visited_states);
    EXPECT_EQ(a.visited_edges, b.visited_edges);
    EXPECT_EQ(left.stats().admitted_states, right.stats().admitted_states);
    EXPECT_EQ(left.stats().scanned_edges, right.stats().scanned_edges);
    EXPECT_EQ(left.stats().bindings, right.stats().bindings);
    EXPECT_EQ(left.stats().witness_steps, right.stats().witness_steps);
    EXPECT_EQ(left.stats().queue_peak, right.stats().queue_peak);
    EXPECT_EQ(left.coverage().issues, right.coverage().issues);
    EXPECT_EQ(left.coverage().all_sources, right.coverage().all_sources);
}
TEST_F(QueryCheckpointTest,
       EverySliceRestoresCallsRecursionAndShortestWitness) {
    publish();
    for (const auto name : {"aa", "rr"}) {
        for (const auto kind : {QueryKind::Value, QueryKind::Effects}) {
            for (const auto witness : {false, true}) {
                auto view = open();
                const auto selected = root(view, name);
                QueryTask oracle(view.view(), selected, kind, witness);
                (void)oracle.advance(QuerySlice::unlimited());
                QueryTask task(binding(view), task_id, selected, kind, witness);
                std::size_t deliveries = 0, count = 0;
                bool done = false;
                while (!done && count++ < 10000) {
                    // Reopen cold evidence each time, never retain an AST or
                    // an active NodeLease in the serialized state.
                    auto bytes = task.checkpoint();
                    ASSERT_TRUE(bytes);
                    view = open();
                    auto restored =
                        QueryTask::restore(binding(view), task_id, selected,
                                           kind, witness, *bytes);
                    ASSERT_TRUE(restored)
                        << static_cast<int>(restored.error().code);
                    EXPECT_EQ(restored->stats().slices, task.stats().slices);
                    task = std::move(*restored);
                    const auto page = task.advance({1, 1, 1, 1});
                    ASSERT_NE(page.progress, QueryProgress::Failed);
                    if (page.sequence) {
                        EXPECT_EQ(page.task, task_id);
                        EXPECT_FALSE(task.acknowledge(page.sequence + 1));
                        deliveries += page.sources.size() + page.witness.size();
                        ASSERT_TRUE(task.acknowledge(page.sequence));
                        EXPECT_TRUE(task.acknowledge(page.sequence));
                    }
                    done = page.progress == QueryProgress::Completed &&
                           !page.pending_sources && !page.pending_witness;
                }
                EXPECT_LT(count, 10000U);
                equal_result(task, oracle);
                EXPECT_EQ(deliveries, task.result().sources.size() +
                                          task.result().witness.size());
            }
        }
    }
}
TEST_F(QueryCheckpointTest,
       ReplaysUnconfirmedPagesWithoutAdvancingOrDoubleCharging) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected, QueryKind::Value, true);
    const auto page = task.advance({1000, 1000, 1000, 2});
    ASSERT_TRUE(page.sequence);
    const auto bytes = task.checkpoint();
    ASSERT_TRUE(bytes);
    auto restored = QueryTask::restore(binding(open()), task_id, selected,
                                       QueryKind::Value, true, *bytes);
    ASSERT_TRUE(restored);
    const auto stats = restored->stats();
    for (unsigned i = 0; i < 3; ++i) {
        const auto replay = restored->advance({0, 0, 0, 0});
        EXPECT_EQ(replay.sequence, page.sequence);
        EXPECT_EQ(replay.task, page.task);
        EXPECT_EQ(replay.sources, page.sources);
        EXPECT_EQ(replay.witness, page.witness);
        EXPECT_EQ(replay.progress, page.progress);
        EXPECT_EQ(restored->stats().slices, stats.slices);
    }
    ASSERT_TRUE(restored->acknowledge(page.sequence));
    const auto acknowledged = restored->checkpoint();
    ASSERT_TRUE(acknowledged);
    auto again = QueryTask::restore(binding(open()), task_id, selected,
                                    QueryKind::Value, true, *acknowledged);
    ASSERT_TRUE(again);
    const auto next = again->advance({0, 0, 0, 1});
    EXPECT_EQ(next.sequence, page.sequence + 1);
    EXPECT_EQ(next.sources.size() + next.witness.size(), 1U);
}
TEST_F(QueryCheckpointTest,
       WideEdgeCursorAndCancelledTaskSurviveDifferentSliceBudgets) {
    std::string text = "let a = external_input(); let aa = [a";
    for (unsigned i = 0; i < 2048; ++i)
        text += ", a";
    publish(text + "];");
    auto view = open();
    auto selected = root(view);
    // Select the physical aggregate below the root's Use wrapper.
    while (view.view()->pin(selected)->edges().size() == 1)
        selected = view.view()->pin(selected)->edges().front().target;
    ASSERT_GT(view.view()->pin(selected)->edges().size(), 2048U);
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({1, 7, 1, 0});
    EXPECT_EQ(task.stats().scanned_edges, 7U);
    task.cancel();
    const auto bytes = task.checkpoint();
    ASSERT_TRUE(bytes);
    auto restored = QueryTask::restore(binding(open()), task_id, selected,
                                       QueryKind::Value, false, *bytes);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->progress(), QueryProgress::Cancelled);
    const auto paused = restored->advance();
    EXPECT_EQ(paused.progress, QueryProgress::Cancelled);
    EXPECT_EQ(restored->stats().scanned_edges, 7U);
    restored->resume();
    const auto page = restored->advance(QuerySlice::unlimited());
    EXPECT_EQ(page.progress, QueryProgress::Completed);
    equal_result(*restored, oracle);
}
TEST_F(QueryCheckpointTest, CancellationRetainsOriginalUnconfirmedPage) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    QueryPage page;
    for (unsigned i = 0; i < 1000 && !page.sequence; ++i)
        page = task.advance({1, 1, 1, 1});
    ASSERT_TRUE(page.sequence);
    ASSERT_EQ(page.progress, QueryProgress::Paused);
    std::stop_source stop;
    stop.request_stop();
    const auto repeated = task.advance({}, stop.get_token());
    EXPECT_EQ(task.progress(), QueryProgress::Cancelled);
    EXPECT_EQ(repeated.progress, page.progress);
    EXPECT_EQ(repeated.sources, page.sources);
    const auto bytes = task.checkpoint();
    ASSERT_TRUE(bytes);
    auto restored = QueryTask::restore(binding(open()), task_id, selected,
                                       QueryKind::Value, false, *bytes);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->progress(), QueryProgress::Cancelled);
    EXPECT_EQ(restored->advance().sources, page.sources);
    ASSERT_TRUE(restored->acknowledge(page.sequence));
    EXPECT_EQ(restored->advance().progress, QueryProgress::Cancelled);
    restored->resume();
    (void)restored->advance(QuerySlice::unlimited());
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    equal_result(*restored, oracle);
}
void reseal(cache::Bytes& bytes) {
    const auto checksum = cache::fingerprint(
        "pagos.query-checkpoint.v1", std::span(bytes).first(bytes.size() - 32));
    std::ranges::copy(checksum.bytes, bytes.end() - 32);
}
TEST_F(QueryCheckpointTest,
       RejectsWrongBindingTargetVersionCorruptionAndStateIndices) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({1, 1, 1, 0});
    const auto encoded = task.checkpoint();
    ASSERT_TRUE(encoded);
    const auto restore = [&](const cache::Bytes& bytes) {
        return QueryTask::restore(binding(view), task_id, selected,
                                  QueryKind::Value, false, bytes);
    };
    EXPECT_EQ(QueryTask::restore(*view.checkpoint_binding(source_text + " "),
                                 task_id, selected, QueryKind::Value, false,
                                 *encoded)
                  .error()
                  .code,
              CheckpointErrorCode::BindingMismatch);
    EXPECT_EQ(QueryTask::restore(binding(view), task_id, root(view, "rr"),
                                 QueryKind::Value, false, *encoded)
                  .error()
                  .code,
              CheckpointErrorCode::BindingMismatch);
    EXPECT_EQ(QueryTask::restore(binding(view), task_id, selected,
                                 QueryKind::Effects, false, *encoded)
                  .error()
                  .code,
              CheckpointErrorCode::BindingMismatch);
    EXPECT_EQ(QueryTask::restore(binding(view), cache::Digest{}, selected,
                                 QueryKind::Value, false, *encoded)
                  .error()
                  .code,
              CheckpointErrorCode::BindingMismatch);
    for (std::size_t i = 0; i < encoded->size(); ++i) {
        auto changed = *encoded;
        changed[i] ^= 1;
        EXPECT_FALSE(restore(changed));
    }
    for (const auto length : {0U, 31U, 200U}) {
        auto changed = *encoded;
        changed.resize(length);
        EXPECT_FALSE(restore(changed));
    }
    for (const auto offset : {9U, 13U, 17U}) {
        auto changed = *encoded;
        changed[offset] = 99; // Format, engine and record model versions.
        reseal(changed);
        EXPECT_EQ(restore(changed).error().code,
                  CheckpointErrorCode::IncompatibleVersion);
    }
    for (const auto offset : {21U, 53U, 85U}) {
        auto changed = *encoded;
        changed[offset] ^= 1; // Closure, position binding and source revision.
        reseal(changed);
        EXPECT_EQ(restore(changed).error().code,
                  CheckpointErrorCode::BindingMismatch);
    }
    auto changed = *encoded;
    // Queue cursor follows header/version/binding/spec/flags/issues/counters.
    constexpr std::size_t cursor_offset = 9 + 12 + 128 + 6 + 5 + 4 + 80;
    std::fill_n(changed.begin() + cursor_offset, 8, 0xff);
    reseal(changed);
    EXPECT_EQ(restore(changed).error().code, CheckpointErrorCode::Corrupt);
    EXPECT_EQ(task.checkpoint({.bytes = 64}).error().code,
              CheckpointErrorCode::LimitExceeded);
    EXPECT_EQ(QueryTask::restore(binding(view), task_id, selected,
                                 QueryKind::Value, false, *encoded,
                                 {.elements = 0})
                  .error()
                  .code,
              CheckpointErrorCode::LimitExceeded);
    QueryTask ordinary(view.view(), selected);
    EXPECT_EQ(ordinary.checkpoint().error().code,
              CheckpointErrorCode::InvalidState);
}
TEST_F(QueryCheckpointTest,
       ResealedMalformedStatesNeverExposeUncheckedCursors) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({5, 7, 3, 0});
    const auto encoded = task.checkpoint();
    ASSERT_TRUE(encoded);
    // Checksum validation is tested above. Here malformed producers supply a
    // matching checksum, exercising internal bounds and phase validation too.
    for (std::size_t offset = 155; offset + 32 < encoded->size(); ++offset) {
        auto changed = *encoded;
        changed[offset] = 0xff;
        reseal(changed);
        auto restored = QueryTask::restore(binding(view), task_id, selected,
                                           QueryKind::Value, false, changed);
        if (!restored)
            continue;
        restored->resume();
        for (unsigned step = 0; step < 1000; ++step) {
            const auto page = restored->advance({17, 19, 23, 1});
            if (page.sequence) {
                ASSERT_TRUE(restored->acknowledge(page.sequence));
            }
            if (page.progress == QueryProgress::Completed ||
                page.progress == QueryProgress::Failed)
                break;
            ASSERT_LT(step, 999U) << offset;
        }
    }
}
TEST_F(QueryCheckpointTest,
       AtomicLeafPublicationFailuresPreserveLiveCursorAndConcurrentReaders) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({1, 1, 1, 0});
    auto encoded = task.checkpoint();
    ASSERT_TRUE(encoded);
    cache::StoreOptions options;
    options.fault = [](cache::IoOperation operation, const fs::path&) {
        return operation == cache::IoOperation::WriteTemporary
                   ? std::make_error_code(std::errc::no_space_on_device)
                   : std::error_code{};
    };
    EXPECT_FALSE(
        cache::FileStore(directory / "tasks", options)
            .write_object(cache::ObjectKind::QueryCheckpoint, *encoded));
    EXPECT_EQ(task.progress(), QueryProgress::Paused);
    cache::FileStore store(directory / "tasks");
    const auto object =
        store.write_object(cache::ObjectKind::QueryCheckpoint, *encoded);
    ASSERT_TRUE(object);
    EXPECT_FALSE(fs::exists(directory / "tasks" / "queries"));
    std::atomic<unsigned> successes{};
    std::vector<std::jthread> readers;
    for (unsigned i = 0; i < 4; ++i)
        readers.emplace_back([&] {
            auto bytes = store.read_object(*object);
            if (!bytes)
                return;
            auto restored = QueryTask::restore(binding(view), task_id, selected,
                                               QueryKind::Value, false, *bytes);
            if (restored &&
                restored->advance(QuerySlice::unlimited()).progress ==
                    QueryProgress::Completed)
                ++successes;
        });
    readers.clear();
    EXPECT_EQ(successes, 4U);
    (void)task.advance(QuerySlice::unlimited());
    EXPECT_EQ(task.progress(), QueryProgress::Completed);
}
TEST_F(QueryCheckpointTest,
       MissingActiveDetailCannotResumeAndMissingFrontierFailsLazily) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({1, 0, 0, 0});
    const auto bytes = task.checkpoint();
    ASSERT_TRUE(bytes);
    auto closure =
        FunctionEvidence::load(cache::FileStore(directory / "evidence"),
                               identity, cache_test::context());
    ASSERT_TRUE(closure);
    for (const auto& group : closure->groups()) {
        const auto hash = group.detail.digest.hex();
        fs::remove(directory / "evidence" / "objects" / hash.substr(0, 2) /
                   hash);
    }
    const auto restored = QueryTask::restore(binding(open()), task_id, selected,
                                             QueryKind::Value, false, *bytes);
    ASSERT_FALSE(restored);
    EXPECT_EQ(restored.error().code, CheckpointErrorCode::UnavailableEvidence);
    // An unstarted task restores without eagerly loading its frontier.
    QueryTask unstarted(binding(view), task_id, selected);
    const auto initial = unstarted.checkpoint();
    ASSERT_TRUE(initial);
    auto lazy = QueryTask::restore(binding(open()), task_id, selected,
                                   QueryKind::Value, false, *initial);
    ASSERT_TRUE(lazy);
    EXPECT_EQ(lazy->advance().progress, QueryProgress::Failed);
    EXPECT_EQ(lazy->coverage().precision, EvidencePrecision::Unresolved);
}
} // namespace
