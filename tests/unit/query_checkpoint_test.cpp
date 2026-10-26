#include "../../lib/cache/wire.h"
#include "cache_test_input.h"
#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/provenance_topology.h"
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
    FunctionEvidenceView open(std::size_t cache_bytes = 0) {
        auto closure =
            FunctionEvidence::load(cache::FileStore(directory / "evidence"),
                                   identity, cache_test::context());
        if (!closure)
            throw std::runtime_error("closure missing");
        auto view =
            FunctionEvidenceView::open(std::move(*closure), cache_bytes);
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
TEST_F(QueryCheckpointTest, SegmentedSnapshotsResumeEveryPhaseWithTinyBlocks) {
    publish();
    auto view = open();
    for (const auto kind : {QueryKind::Value, QueryKind::Effects}) {
        for (const bool witness : {false, true}) {
            auto id = task_id;
            id.bytes[0] = static_cast<std::uint8_t>(
                static_cast<unsigned>(kind) * 2 + (witness ? 1 : 0));
            auto opened = cache::TaskStore::open(directory / "tasks", id,
                                                 cache::TaskAccess::ReadWrite);
            ASSERT_TRUE(opened);
            auto store = *opened;
            const auto selected = root(view);
            QueryTask oracle(view.view(), selected, kind, witness);
            (void)oracle.advance(QuerySlice::unlimited());
            QueryTask task(binding(view), id, selected, kind, witness);
            std::uint64_t generation{};
            std::size_t deliveries{};
            for (unsigned step = 0; step < 1000; ++step) {
                auto head = task.save(store, generation, 70);
                ASSERT_TRUE(head) << static_cast<int>(head.error().code);
                generation = head->generation;
                auto reader = cache::TaskStore::open(
                    directory / "tasks", id, cache::TaskAccess::ReadOnly);
                ASSERT_TRUE(reader);
                auto restored = QueryTask::restore(
                    binding(open()), *reader, *head, selected, kind, witness);
                ASSERT_TRUE(restored)
                    << static_cast<int>(restored.error().code);
                EXPECT_EQ(restored->stats().slices, task.stats().slices);
                task = std::move(*restored);
                const auto page = task.advance({1, 1, 1, 1});
                ASSERT_NE(page.progress, QueryProgress::Failed);
                if (page.sequence) {
                    // Persist unconfirmed ranges in their own generation.
                    const auto unconfirmed =
                        task.offload(store, generation, 70);
                    ASSERT_TRUE(unconfirmed);
                    generation = unconfirmed->generation;
                    const auto replay = task.advance({0, 0, 0, 0});
                    EXPECT_EQ(replay.sources, page.sources);
                    EXPECT_EQ(replay.witness, page.witness);
                    EXPECT_EQ(replay.sequence, page.sequence);
                    deliveries += page.sources.size() + page.witness.size();
                    ASSERT_TRUE(task.acknowledge(page.sequence));
                }
                if (page.progress == QueryProgress::Completed &&
                    !page.pending_sources && !page.pending_witness)
                    break;
                ASSERT_LT(step, 999U);
            }
            equal_result(task, oracle);
            EXPECT_EQ(deliveries, oracle.result().sources.size() +
                                      oracle.result().witness.size());
        }
    }
}
std::size_t state_bytes(const QueryStateMemory& memory) {
    return memory.queue + memory.visited + memory.contexts +
           memory.context_index + memory.sources + memory.witness;
}
TEST_F(QueryCheckpointTest,
       OffloadActuallyReleasesWideFrontierAndVisitedCapacity) {
    std::string text;
    for (unsigned i = 0; i < 2049; ++i)
        text += "let a" + std::to_string(i) + " = external_input(); ";
    text += "let aa = [a0";
    for (unsigned i = 1; i < 2049; ++i)
        text += ", a" + std::to_string(i);
    publish(text + "];");
    auto view = open(8 * 1024 * 1024);
    auto selected = root(view);
    while (view.view()->pin(selected)->edges().size() == 1)
        selected = view.view()->pin(selected)->edges().front().target;
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({1, 1024, 1, 0});
    const auto work = task.stats();
    const auto before = task.state_memory();
    EXPECT_GT(before.queue, 16'000U);
    EXPECT_GT(before.visited, 16'000U);
    std::optional<cache::TaskStore> store;
    store = *cache::TaskStore::open(directory / "tasks", task_id,
                                    cache::TaskAccess::ReadWrite);
    const auto head = task.offload(*store, 0, 256);
    ASSERT_TRUE(head);
    EXPECT_TRUE(task.state_memory().offloaded);
    EXPECT_EQ(state_bytes(task.state_memory()), 0U);
    EXPECT_EQ(task.stats().scanned_edges, work.scanned_edges);
    const auto path = store->directory();
    store.reset();
    EXPECT_EQ(cache::reclaim_tasks(directory / "tasks",
                                   cache::TaskCleanupPolicy::AllInactive)
                  ->active,
              1U);
    task.cancel();
    EXPECT_EQ(task.progress(), QueryProgress::Cancelled);
    const auto paused = task.advance(); // Loads tables but does no query work.
    EXPECT_EQ(paused.progress, QueryProgress::Cancelled);
    EXPECT_EQ(task.stats().scanned_edges, work.scanned_edges);
    EXPECT_FALSE(task.state_memory().offloaded);
    task.resume();
    (void)task.advance(QuerySlice::unlimited());
    equal_result(task, oracle);
    EXPECT_TRUE(
        fs::exists(path)); // Unloading/closing never deletes checkpoints.
}
TEST_F(QueryCheckpointTest,
       OffloadReleasesContextsSourcesAndWitnessThenReplaysDelivery) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected, QueryKind::Value, true);
    const auto page = task.advance({1000, 1000, 1000, 1});
    ASSERT_EQ(page.progress, QueryProgress::Completed);
    ASSERT_TRUE(page.sequence);
    const auto before = task.state_memory();
    EXPECT_GT(before.contexts, 0U);
    EXPECT_GT(before.context_index, 0U);
    EXPECT_GT(before.sources, 0U);
    EXPECT_GT(before.witness, 0U);
    auto store = cache::TaskStore::open(directory / "tasks", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    const auto head = task.offload(*store, 0, 128);
    ASSERT_TRUE(head);
    EXPECT_EQ(head->status, cache::TaskStatus::AwaitingDelivery);
    EXPECT_EQ(state_bytes(task.state_memory()), 0U);
    EXPECT_TRUE(task.coverage().witness_found);
    const auto replay = task.advance({0, 0, 0, 0});
    EXPECT_EQ(replay.sources, page.sources);
    EXPECT_EQ(replay.sequence, page.sequence);
    EXPECT_EQ(task.state_memory().contexts, before.contexts);
    QueryTask oracle(view.view(), selected, QueryKind::Value, true);
    (void)oracle.advance(QuerySlice::unlimited());
    equal_result(task, oracle);
}
TEST_F(QueryCheckpointTest,
       FailedPublicationQuotaReadOnlyAndStaleHeadDoNotDiscardCursor) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({5, 7, 5, 0});
    const auto before = state_bytes(task.state_memory());
    auto bounded = cache::TaskStore::open(directory / "bounded", task_id,
                                          cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(bounded);
    EXPECT_EQ(task.save(*bounded, 0, 70, {.bytes = 400}).error().code,
              CheckpointErrorCode::LimitExceeded);
    EXPECT_FALSE(fs::exists(bounded->directory() / "objects"));
    auto store = cache::TaskStore::open(directory / "tasks", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    auto head = task.save(*store, 0, 70);
    ASSERT_TRUE(head);
    const auto stale = task.offload(*store, 0, 70);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().storage->code,
              cache::StoreErrorCode::StaleGeneration);
    auto reader = cache::TaskStore::open(directory / "tasks", task_id,
                                         cache::TaskAccess::ReadOnly);
    ASSERT_TRUE(reader);
    const auto readonly = task.offload(*reader, 1, 70);
    ASSERT_FALSE(readonly);
    EXPECT_EQ(readonly.error().storage->code, cache::StoreErrorCode::ReadOnly);
    cache::TaskStoreOptions quota;
    quota.disk_bytes = 256;
    auto limited = cache::TaskStore::open(directory / "limited", task_id,
                                          cache::TaskAccess::ReadWrite, quota);
    ASSERT_TRUE(limited);
    EXPECT_EQ(task.offload(*limited, 0, 70).error().code,
              CheckpointErrorCode::LimitExceeded);
    cache::TaskStoreOptions failed;
    failed.store.fault = [](cache::IoOperation operation,
                            const fs::path& path) {
        return operation == cache::IoOperation::PublishManifest &&
                       path.filename() == "head"
                   ? std::make_error_code(std::errc::no_space_on_device)
                   : std::error_code{};
    };
    auto faulty = cache::TaskStore::open(directory / "faulty", task_id,
                                         cache::TaskAccess::ReadWrite, failed);
    ASSERT_TRUE(faulty);
    EXPECT_FALSE(task.offload(*faulty, 0, 70));
    EXPECT_FALSE(*faulty->head());
    EXPECT_EQ(state_bytes(task.state_memory()), before);
    EXPECT_FALSE(task.state_memory().offloaded);
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    (void)task.advance(QuerySlice::unlimited());
    equal_result(task, oracle);
}
TEST_F(QueryCheckpointTest,
       MissingSegmentFailsWithoutPublishingPartialResidentState) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({5, 7, 5, 0});
    auto store = cache::TaskStore::open(directory / "tasks", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    const auto head = task.offload(*store, 0, 70);
    ASSERT_TRUE(head);
    for (const auto& entry :
         fs::recursive_directory_iterator(store->directory() / "objects")) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream input(entry.path(), std::ios::binary);
        std::array<char, 9> bytes{};
        input.read(bytes.data(), bytes.size());
        if (static_cast<unsigned char>(bytes[8]) ==
            static_cast<unsigned>(cache::ObjectKind::QueryStateSegment))
            fs::remove(entry.path());
    }
    const auto page = task.advance();
    EXPECT_EQ(page.progress, QueryProgress::Failed);
    EXPECT_EQ(page.coverage.precision, EvidencePrecision::Unresolved);
    EXPECT_FALSE(page.coverage.all_sources);
    ASSERT_TRUE(task.failure());
    EXPECT_EQ(task.failure()->storage->code,
              cache::StoreErrorCode::MissingObject);
    EXPECT_EQ(state_bytes(task.state_memory()), 0U);
    EXPECT_THROW((void)task.result(), std::runtime_error);
    EXPECT_FALSE(QueryTask::restore(binding(open()), *store, *head, selected,
                                    QueryKind::Value, false));
}
TEST_F(QueryCheckpointTest,
       RejectsMalformedSegmentDirectoryAndBindingBeforeAcceptingTask) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({1, 1, 1, 0});
    auto store = cache::TaskStore::open(directory / "tasks", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    auto head = task.save(*store, 0, 70);
    ASSERT_TRUE(head);
    EXPECT_EQ(QueryTask::restore(*view.checkpoint_binding(source_text + " "),
                                 *store, *head, selected, QueryKind::Value,
                                 false)
                  .error()
                  .code,
              CheckpointErrorCode::BindingMismatch);
    auto bytes = store->read(head->manifest);
    ASSERT_TRUE(bytes);
    // Unconfirmed=false control prefix is 301 bytes after header+directory
    // version.
    constexpr unsigned first_segment = 9 + 4 + 301 + 8;
    (*bytes)[first_segment] = 1; // First segment must start at record zero.
    auto transaction = store->begin(1);
    ASSERT_TRUE(transaction);
    const auto bad = transaction->write(cache::ObjectKind::QueryTaskManifest,
                                        std::move(*bytes));
    ASSERT_TRUE(bad);
    const auto bad_head = transaction->commit(*bad, cache::TaskStatus::Paused);
    ASSERT_TRUE(bad_head);
    const auto restored = QueryTask::restore(binding(open()), *store, *bad_head,
                                             selected, QueryKind::Value, false);
    ASSERT_FALSE(restored);
    EXPECT_EQ(restored.error().code, CheckpointErrorCode::Corrupt);
}
TEST_F(QueryCheckpointTest,
       ReusesUnchangedImmutableSegmentsAndRootAcrossGenerations) {
    publish();
    auto view = open();
    const auto selected = root(view);
    QueryTask task(binding(view), task_id, selected);
    (void)task.advance({3, 3, 3, 0});
    bool forbid_leaf_writes = false;
    cache::TaskStoreOptions options;
    options.store.fault = [&](cache::IoOperation operation,
                              const fs::path& path) {
        return forbid_leaf_writes &&
                       operation == cache::IoOperation::WriteTemporary &&
                       path.filename() != "head"
                   ? std::make_error_code(std::errc::no_space_on_device)
                   : std::error_code{};
    };
    auto store = cache::TaskStore::open(directory / "tasks", task_id,
                                        cache::TaskAccess::ReadWrite, options);
    ASSERT_TRUE(store);
    const auto first = task.save(*store, 0, 70);
    ASSERT_TRUE(first);
    forbid_leaf_writes = true;
    const auto second = task.save(*store, 1, 70);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->generation, 2U);
    EXPECT_EQ(second->manifest, first->manifest);
    QueryTask other_root(binding(view), task_id, root(view, "rr"));
    EXPECT_EQ(other_root.save(*store, 2, 70).error().code,
              CheckpointErrorCode::BindingMismatch);
    EXPECT_EQ((**store->head()).generation, 2U);
    EXPECT_EQ(QueryTask::restore(binding(view), *store, *second, selected,
                                 QueryKind::Value, false, {.elements = 0})
                  .error()
                  .code,
              CheckpointErrorCode::LimitExceeded);
}
const QueryPagingConfig tiny_pages{
    .segment_bytes = 86, .resident_bytes = 1024, .cache_bytes = 256};
void paging_memory(const QueryTask& task, QueryPagingConfig config) {
    const auto memory = task.state_memory();
    EXPECT_TRUE(memory.paged);
    EXPECT_LE(state_bytes(memory) + memory.cached, config.resident_bytes);
    EXPECT_LE(memory.cached, config.cache_bytes);
    EXPECT_LE(memory.peak_buffers, config.resident_bytes);
}
QueryPage finish_paged(QueryTask& task, QueryPagingConfig config,
                       QuerySlice slice = {1, 1, 1, 1}) {
    for (unsigned step = 0; step < 100000; ++step) {
        auto page = task.advance(slice);
        paging_memory(task, config);
        if (task.storage_failure())
            return page;
        if (page.sequence) {
            EXPECT_TRUE(task.acknowledge(page.sequence));
        }
        if (page.progress == QueryProgress::Completed &&
            !page.pending_sources && !page.pending_witness)
            return page;
        if (page.progress == QueryProgress::Failed)
            return page;
    }
    ADD_FAILURE() << "paged task did not finish";
    return {};
}
TEST_F(QueryCheckpointTest,
       PagedCallsRecursionEffectsAndWitnessMatchResidentQueries) {
    publish();
    auto view = open(1024 * 1024);
    for (const auto name : {"aa", "rr"}) {
        for (const auto kind : {QueryKind::Value, QueryKind::Effects}) {
            for (const bool witness : {false, true}) {
                auto id = task_id;
                id.bytes[0] = static_cast<std::uint8_t>(
                    (name == std::string_view("aa") ? 0 : 4) +
                    static_cast<unsigned>(kind) * 2 + (witness ? 1 : 0));
                auto store = cache::TaskStore::open(
                    directory / "paged", id, cache::TaskAccess::ReadWrite);
                ASSERT_TRUE(store);
                const auto selected = root(view, name);
                QueryTask oracle(view.view(), selected, kind, witness);
                (void)oracle.advance(QuerySlice::unlimited());
                auto task = QueryTask::begin_paged(
                    binding(view), *store, selected, kind, witness, tiny_pages);
                ASSERT_TRUE(task);
                EXPECT_EQ(finish_paged(*task, tiny_pages).progress,
                          QueryProgress::Completed);
                ASSERT_FALSE(task->storage_failure());
                equal_result(*task, oracle);
                paging_memory(*task, tiny_pages);
                EXPECT_GT(task->paging_stats().writes, 0U);
                const auto encoded = task->checkpoint();
                ASSERT_TRUE(encoded);
                auto old = QueryTask::restore(binding(view), id, selected, kind,
                                              witness, *encoded);
                ASSERT_TRUE(old);
                equal_result(*old, oracle);
            }
        }
    }
}
TEST_F(QueryCheckpointTest,
       PagedEverySliceRestoresBoundedTablesAndUnconfirmedPages) {
    publish();
    auto view = open(1024 * 1024);
    const auto selected = root(view);
    auto store = cache::TaskStore::open(directory / "paged", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    QueryTask oracle(view.view(), selected, QueryKind::Value, true);
    (void)oracle.advance(QuerySlice::unlimited());
    auto task = QueryTask::begin_paged(binding(view), *store, selected,
                                       QueryKind::Value, true, tiny_pages);
    ASSERT_TRUE(task);
    std::uint64_t generation{};
    for (unsigned step = 0; step < 1000; ++step) {
        const auto page = task->advance({1, 1, 1, 1});
        ASSERT_FALSE(task->storage_failure());
        const auto work = task->stats();
        const auto saved = task->offload(*store, generation);
        ASSERT_TRUE(saved) << static_cast<int>(saved.error().code);
        generation = saved->generation;
        EXPECT_EQ(state_bytes(task->state_memory()), 0U);
        EXPECT_EQ(task->state_memory().cached, 0U);
        auto restored =
            QueryTask::restore_paged(binding(view), *store, *saved, selected,
                                     QueryKind::Value, true, tiny_pages);
        ASSERT_TRUE(restored) << static_cast<int>(restored.error().code);
        task = std::move(restored);
        paging_memory(*task, tiny_pages);
        EXPECT_EQ(task->stats().scanned_edges, work.scanned_edges);
        EXPECT_EQ(task->stats().slices, work.slices);
        if (page.sequence) {
            const auto repeated = task->advance({0, 0, 0, 0});
            EXPECT_EQ(repeated.sequence, page.sequence);
            EXPECT_EQ(repeated.sources, page.sources);
            EXPECT_EQ(repeated.witness, page.witness);
            ASSERT_TRUE(task->acknowledge(page.sequence));
        }
        if (page.progress == QueryProgress::Completed &&
            !page.pending_sources && !page.pending_witness)
            break;
        ASSERT_LT(step, 999U);
    }
    equal_result(*task, oracle);
    const auto invocations = task->invocations();
    EXPECT_EQ(invocations.size(), task->result().invocations.size());
    EXPECT_GT(task->state_memory().compatibility, 0U);
    (void)task->advance({0, 0, 0, 0});
    EXPECT_EQ(task->state_memory().compatibility, 0U);
}
std::string wide_program(unsigned count, bool repeat = false) {
    std::string text;
    for (unsigned i = 0; i < count; ++i)
        text += "let a" + std::to_string(i) + " = external_input(); ";
    text += "let aa = [a0";
    for (unsigned i = 1; i < count; ++i)
        text += ", a" + std::to_string(i);
    return text + (repeat ? ", a0];" : "];");
}
NodeId aggregate_root(const FunctionEvidenceView& view, NodeId selected) {
    while (view.view()->pin(selected)->edges().size() == 1)
        selected = view.view()->pin(selected)->edges().front().target;
    return selected;
}
TEST_F(QueryCheckpointTest,
       PagedLargeFrontierAndRestoredIndexesStayWithinOwnedBufferBudget) {
    publish(wide_program(257));
    auto view = open(8 * 1024 * 1024);
    const auto selected = aggregate_root(view, root(view));
    const QueryPagingConfig config{
        .segment_bytes = 256, .resident_bytes = 2048, .cache_bytes = 512};
    auto store = cache::TaskStore::open(directory / "paged", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    auto task = QueryTask::begin_paged(binding(view), *store, selected,
                                       QueryKind::Value, false, config);
    ASSERT_TRUE(task);
    (void)task->advance({1, 128, 0, 0});
    ASSERT_FALSE(task->storage_failure());
    EXPECT_EQ(task->stats().admitted_states, 129U);
    paging_memory(*task, config);
    EXPECT_LE(task->state_memory().queue + task->state_memory().visited, 384U);
    EXPECT_GT(task->state_memory().directory, 0U);
    const auto saved = task->offload(*store, 0);
    ASSERT_TRUE(saved);
    const auto progress = task->advance({0, 0, 0, 0});
    ASSERT_FALSE(task->storage_failure());
    EXPECT_EQ(progress.progress, QueryProgress::Paused);
    paging_memory(*task, config);
    EXPECT_EQ(task->stats().admitted_states, 129U);
    EXPECT_EQ(finish_paged(*task, config, {17, 23, 19, 7}).progress,
              QueryProgress::Completed);
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    equal_result(*task, oracle);
    EXPECT_GT(task->paging_stats().reads, 10U);
}
TEST_F(QueryCheckpointTest,
       PagedReadFailureRollsBackEdgeCursorAndWorkBeforeRetry) {
    publish(wide_program(64, true));
    auto view = open(4 * 1024 * 1024);
    const auto selected = aggregate_root(view, root(view));
    const QueryPagingConfig config{
        .segment_bytes = 256, .resident_bytes = 2048, .cache_bytes = 512};
    bool fail = false;
    cache::TaskStoreOptions options;
    options.store.fault = [&](cache::IoOperation operation,
                              const fs::path& path) {
        return fail && operation == cache::IoOperation::ReadFile &&
                       path.filename() != "head"
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto store = cache::TaskStore::open(directory / "paged", task_id,
                                        cache::TaskAccess::ReadWrite, options);
    ASSERT_TRUE(store);
    auto task = QueryTask::begin_paged(binding(view), *store, selected,
                                       QueryKind::Value, false, config);
    ASSERT_TRUE(task);
    (void)task->advance({1, 64, 0, 0});
    ASSERT_FALSE(task->storage_failure());
    EXPECT_EQ(task->stats().scanned_edges, 64U);
    const auto admitted = task->stats().admitted_states;
    task->evict_state_cache();
    fail = true;
    const auto paused = task->advance({0, 1, 0, 0});
    EXPECT_EQ(paused.progress, QueryProgress::Paused);
    ASSERT_TRUE(task->storage_failure());
    EXPECT_EQ(task->stats().scanned_edges, 64U);
    EXPECT_EQ(task->stats().admitted_states, admitted);
    EXPECT_FALSE(paused.coverage.all_sources);
    fail = false;
    EXPECT_EQ(finish_paged(*task, config, {17, 23, 19, 7}).progress,
              QueryProgress::Completed);
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    equal_result(*task, oracle);
}
TEST_F(QueryCheckpointTest,
       PagedQuotaPauseRetainsDurableHeadAndResumesWithLargerQuota) {
    publish(wide_program(64));
    auto view = open(4 * 1024 * 1024);
    const auto selected = aggregate_root(view, root(view));
    cache::TaskStoreOptions limited;
    limited.disk_bytes = 2048;
    auto store = cache::TaskStore::open(directory / "paged", task_id,
                                        cache::TaskAccess::ReadWrite, limited);
    ASSERT_TRUE(store);
    auto task = QueryTask::begin_paged(binding(view), *store, selected,
                                       QueryKind::Value, false, tiny_pages);
    ASSERT_TRUE(task);
    const auto first = task->save(*store, 0);
    ASSERT_TRUE(first);
    const auto pause = task->advance(QuerySlice::unlimited());
    EXPECT_EQ(pause.progress, QueryProgress::Paused);
    ASSERT_TRUE(task->storage_failure());
    EXPECT_EQ(task->storage_failure()->code,
              CheckpointErrorCode::LimitExceeded);
    EXPECT_EQ((**store->head()).generation, 1U);
    auto larger = cache::TaskStore::open(directory / "paged", task_id,
                                         cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(larger);
    ASSERT_TRUE(task->resume_with_storage(*larger));
    EXPECT_EQ(finish_paged(*task, tiny_pages, {17, 23, 19, 7}).progress,
              QueryProgress::Completed);
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    equal_result(*task, oracle);
    const auto saved = task->save(*larger, 1);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->generation, 2U);
}
TEST_F(QueryCheckpointTest,
       PagedReadOnlyRestorePausesWithoutWritesAndAllowsExplicitRebind) {
    publish(wide_program(64));
    auto view = open(4 * 1024 * 1024);
    const auto selected = aggregate_root(view, root(view));
    auto store = cache::TaskStore::open(directory / "paged", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    auto task = QueryTask::begin_paged(binding(view), *store, selected,
                                       QueryKind::Value, false, tiny_pages);
    ASSERT_TRUE(task);
    (void)task->advance({1, 17, 0, 0});
    const auto saved = task->save(*store, 0);
    ASSERT_TRUE(saved);
    unsigned mutations{};
    cache::TaskStoreOptions readonly;
    readonly.store.fault = [&](cache::IoOperation operation, const fs::path&) {
        if (operation != cache::IoOperation::ReadFile)
            ++mutations;
        return std::error_code{};
    };
    auto reader = cache::TaskStore::open(directory / "paged", task_id,
                                         cache::TaskAccess::ReadOnly, readonly);
    ASSERT_TRUE(reader);
    EXPECT_EQ(task->save(*reader, saved->generation).error().storage->code,
              cache::StoreErrorCode::ReadOnly);
    // A failed save through a read-only handle must not rebind the live
    // backend.
    EXPECT_EQ(finish_paged(*task, tiny_pages, {17, 23, 19, 7}).progress,
              QueryProgress::Completed);
    auto restored =
        QueryTask::restore_paged(binding(view), *reader, *saved, selected,
                                 QueryKind::Value, false, tiny_pages);
    ASSERT_TRUE(restored);
    paging_memory(*restored, tiny_pages);
    const auto paused = restored->advance(QuerySlice::unlimited());
    EXPECT_EQ(paused.progress, QueryProgress::Paused);
    ASSERT_TRUE(restored->storage_failure());
    EXPECT_EQ(restored->storage_failure()->storage->code,
              cache::StoreErrorCode::ReadOnly);
    EXPECT_EQ(mutations, 0U);
    restored->cancel();
    EXPECT_EQ(restored->advance().progress, QueryProgress::Cancelled);
    auto wrong = cache::TaskStore::open(directory / "other", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(wrong);
    EXPECT_EQ(restored->resume_with_storage(*wrong).error().code,
              CheckpointErrorCode::BindingMismatch);
    ASSERT_TRUE(restored->resume_with_storage(*store));
    EXPECT_EQ(finish_paged(*restored, tiny_pages, {17, 23, 19, 7}).progress,
              QueryProgress::Completed);
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    equal_result(*restored, oracle);
}
TEST_F(QueryCheckpointTest,
       PagedDeliveryReadFailureRetainsSequenceAndRetriesOffloadedState) {
    publish(wide_program(16));
    auto view = open(4 * 1024 * 1024);
    const auto selected = root(view);
    bool fail = false;
    cache::TaskStoreOptions options;
    options.store.fault = [&](cache::IoOperation operation, const fs::path&) {
        return fail && operation == cache::IoOperation::ReadFile
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto store = cache::TaskStore::open(directory / "paged", task_id,
                                        cache::TaskAccess::ReadWrite, options);
    ASSERT_TRUE(store);
    auto task = QueryTask::begin_paged(binding(view), *store, selected,
                                       QueryKind::Value, false, tiny_pages);
    ASSERT_TRUE(task);
    const auto page = task->advance(QuerySlice::unlimited());
    ASSERT_EQ(page.progress, QueryProgress::Completed);
    ASSERT_TRUE(page.sequence);
    const auto saved = task->save(*store, 0);
    ASSERT_TRUE(saved);
    task->evict_state_cache();
    const auto work = task->stats();
    fail = true;
    const auto unavailable_export = task->checkpoint();
    ASSERT_FALSE(unavailable_export);
    EXPECT_EQ(unavailable_export.error().code,
              CheckpointErrorCode::StorageFailure);
    EXPECT_EQ(task->advance().progress, QueryProgress::Paused);
    ASSERT_TRUE(task->storage_failure());
    EXPECT_FALSE(task->coverage().all_sources);
    EXPECT_EQ(task->stats().scanned_edges, work.scanned_edges);
    fail = false;
    const auto replay = task->advance({0, 0, 0, 0});
    EXPECT_EQ(replay.sequence, page.sequence);
    EXPECT_EQ(replay.sources, page.sources);
    EXPECT_EQ(replay.progress, QueryProgress::Completed);
    EXPECT_FALSE(task->storage_failure());
    ASSERT_TRUE(task->offload(*store, 1));
    fail = true;
    EXPECT_EQ(task->advance().progress, QueryProgress::Paused);
    EXPECT_FALSE(task->coverage().all_sources);
    EXPECT_TRUE(task->state_memory().offloaded);
    EXPECT_EQ(state_bytes(task->state_memory()), 0U);
    fail = false;
    const auto retried = task->advance({0, 0, 0, 0});
    EXPECT_EQ(retried.progress, QueryProgress::Completed);
    EXPECT_EQ(retried.sequence, page.sequence);
    EXPECT_EQ(retried.sources, page.sources);
    EXPECT_FALSE(task->storage_failure());
    EXPECT_EQ(task->stats().scanned_edges, work.scanned_edges);
    ASSERT_TRUE(task->acknowledge(page.sequence));
    QueryTask oracle(view.view(), selected);
    (void)oracle.advance(QuerySlice::unlimited());
    equal_result(*task, oracle);
}
TEST_F(QueryCheckpointTest,
       PagedRejectsResealedIndexBijectionOrderingAndRangeCorruption) {
    publish(wide_program(16));
    auto view = open(4 * 1024 * 1024);
    const auto selected = aggregate_root(view, root(view));
    auto store = cache::TaskStore::open(directory / "paged", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    auto task = QueryTask::begin_paged(binding(view), *store, selected,
                                       QueryKind::Value, false, tiny_pages);
    ASSERT_TRUE(task);
    (void)task->advance({1, 10, 0, 0});
    auto saved = task->save(*store, 0);
    ASSERT_TRUE(saved);
    const auto original = store->read(saved->manifest);
    ASSERT_TRUE(original);
    cache::wire::Reader reader(*original, {});
    // Common control prefix without an unconfirmed delivery.
    (void)reader.raw(9 + 4 + 301);
    for (unsigned table = 0; table < 4; ++table) {
        (void)reader.u32();
        const auto segments = reader.u32();
        (void)reader.raw(segments * 49);
    }
    (void)reader.u32();
    ASSERT_GT(reader.u32(), 0U);
    const auto descriptor = original->size() - reader.remaining();
    EXPECT_EQ(reader.u32(), 0U);
    ASSERT_EQ(reader.u32(), 2U);
    cache::ObjectRef index;
    index.kind = static_cast<cache::ObjectKind>(reader.u8());
    const auto digest = reader.raw(32);
    std::ranges::copy(digest, index.digest.bytes.begin());
    index.size = reader.u64();
    ASSERT_TRUE(reader.good());
    const auto leaf = store->read(index);
    ASSERT_TRUE(leaf);
    for (unsigned corruption = 0; corruption < 3; ++corruption) {
        SCOPED_TRACE(corruption);
        auto manifest = *original;
        auto bytes = *leaf;
        if (corruption == 0) // In-range value, wrong queue/index bijection.
            bytes[54 + 8] ^= 1;
        else if (corruption == 1) // Duplicate key in a supposedly sorted run.
            std::copy_n(bytes.begin() + 54, 8, bytes.begin() + 54 + 16);
        else // Incorrect directory minimum, with authentic CAS checksums.
            manifest[descriptor + 49] ^= 1;
        auto writer = store->begin(saved->generation);
        ASSERT_TRUE(writer);
        auto ref = writer->write(cache::ObjectKind::QueryStateSegment,
                                 std::move(bytes));
        ASSERT_TRUE(ref);
        std::ranges::copy(ref->digest.bytes,
                          manifest.begin() +
                              static_cast<std::ptrdiff_t>(descriptor + 9));
        auto root_ref = writer->write(cache::ObjectKind::QueryTaskManifest,
                                      std::move(manifest));
        ASSERT_TRUE(root_ref);
        const auto committed = writer->commit(*root_ref, saved->status);
        ASSERT_TRUE(committed);
        saved = *committed;
        auto rejected =
            QueryTask::restore_paged(binding(view), *store, *saved, selected,
                                     QueryKind::Value, false, tiny_pages);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code, CheckpointErrorCode::Corrupt);
    }
}
TEST_F(QueryCheckpointTest,
       PagedRejectsInsufficientBuffersBeforeCreatingQueryObjects) {
    publish();
    auto view = open();
    auto store = cache::TaskStore::open(directory / "paged", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    EXPECT_EQ(QueryTask::begin_paged(binding(view), *store, {}).error().code,
              CheckpointErrorCode::InvalidState);
    EXPECT_EQ(
        QueryTask::begin_paged(
            binding(view), *store, root(view), QueryKind::Value, false,
            {.segment_bytes = 86, .resident_bytes = 100, .cache_bytes = 256})
            .error()
            .code,
        CheckpointErrorCode::LimitExceeded);
    EXPECT_FALSE(fs::exists(store->directory() / "objects"));
}
void equal_topology_work(const TopologyStats& a, const TopologyStats& b) {
    EXPECT_EQ(a.nodes, b.nodes);
    EXPECT_EQ(a.edges, b.edges);
    EXPECT_EQ(a.summary_links, b.summary_links);
    EXPECT_EQ(a.formulas, b.formulas);
    EXPECT_EQ(a.formula_terms, b.formula_terms);
    EXPECT_EQ(a.formula_states, b.formula_states);
    EXPECT_EQ(a.formula_edges, b.formula_edges);
    EXPECT_EQ(a.formula_reuses, b.formula_reuses);
    EXPECT_EQ(a.source_families, b.source_families);
}
void equal_formulas(const FormulaIndex& a, const FormulaIndex& b) {
    ASSERT_EQ(a.formulas().size(), b.formulas().size());
    for (std::size_t i = 0; i < a.formulas().size(); ++i) {
        EXPECT_EQ(a.formulas()[i].root, b.formulas()[i].root);
        EXPECT_EQ(a.formulas()[i].kind, b.formulas()[i].kind);
        EXPECT_EQ(a.formulas()[i].terms, b.formulas()[i].terms);
        EXPECT_EQ(a.formulas()[i].fallback, b.formulas()[i].fallback);
    }
}
std::vector<TopologyRecord> finish_topology(TopologyTask& task,
                                            QuerySlice slice = {1, 1, 1, 1}) {
    std::vector<TopologyRecord> records;
    for (unsigned step = 0; step < 100000; ++step) {
        const auto page = task.advance(slice);
        if (page.progress == QueryProgress::Failed) {
            ADD_FAILURE() << "topology failed";
            break;
        }
        records.insert(records.end(), page.records.begin(), page.records.end());
        if (page.task && page.sequence) {
            EXPECT_TRUE(task.acknowledge(page.sequence));
        }
        if (page.progress == QueryProgress::Completed)
            return records;
    }
    ADD_FAILURE() << "topology did not finish";
    return records;
}
TEST_F(QueryCheckpointTest,
       TopologyEverySliceRestoresPhysicalAndFormulaPreparation) {
    publish();
    for (const auto name : {"aa", "rr"}) {
        auto view = open();
        const auto selected = root(view, name);
        TopologyTask oracle(view.view(), selected);
        const auto expected = finish_topology(oracle);
        TopologyTask task(binding(view), task_id, selected);
        std::vector<TopologyRecord> received;
        std::size_t formula_cuts = 0;
        for (unsigned step = 0; step < 10000; ++step) {
            const auto bytes = task.checkpoint();
            ASSERT_TRUE(bytes);
            const auto work = task.stats();
            view = open(); // Rebind to a new cold view, no Stage replay.
            auto restored =
                TopologyTask::restore(binding(view), task_id, selected, *bytes);
            ASSERT_TRUE(restored) << static_cast<int>(restored.error().code);
            equal_topology_work(work, restored->stats());
            task = std::move(*restored);
            const auto page = task.advance({1, 1, 1, 1});
            ASSERT_NE(page.progress, QueryProgress::Failed);
            if (task.stats().formula_states)
                ++formula_cuts;
            if (page.sequence) {
                EXPECT_EQ(page.task, task_id);
                EXPECT_FALSE(task.acknowledge(page.sequence + 1));
                const auto unconfirmed = task.checkpoint();
                ASSERT_TRUE(unconfirmed);
                auto replayed = TopologyTask::restore(binding(open()), task_id,
                                                      selected, *unconfirmed);
                ASSERT_TRUE(replayed);
                const auto before = replayed->stats();
                const auto replay = replayed->advance({0, 0, 0, 0});
                EXPECT_EQ(replay.sequence, page.sequence);
                EXPECT_EQ(replay.records, page.records);
                EXPECT_EQ(replay.progress, page.progress);
                equal_topology_work(before, replayed->stats());
                ASSERT_TRUE(replayed->acknowledge(page.sequence));
                EXPECT_TRUE(replayed->acknowledge(page.sequence));
                task = std::move(*replayed);
                received.insert(received.end(), page.records.begin(),
                                page.records.end());
            }
            if (page.progress == QueryProgress::Completed)
                break;
            ASSERT_LT(step, 9999U);
        }
        EXPECT_GT(formula_cuts, 0U);
        EXPECT_EQ(received, expected);
        equal_topology_work(task.stats(), oracle.stats());
        ASSERT_TRUE(task.index());
        equal_formulas(*task.index(), *oracle.index());
        for (auto kind : {QueryKind::Value, QueryKind::Effects}) {
            auto compressed = task.index()->begin_sources(selected, kind);
            (void)compressed.advance(QuerySlice::unlimited());
            QueryTask raw(view.view(), selected, kind);
            (void)raw.advance(QuerySlice::unlimited());
            EXPECT_EQ(compressed.result().sources, raw.result().sources);
            EXPECT_EQ(compressed.result().complete, raw.result().complete);
        }
        EXPECT_THROW((void)task.graph(), std::logic_error);
        EXPECT_THROW((void)task.index()->graph(), std::logic_error);
    }
}
TEST_F(QueryCheckpointTest,
       TopologyTaskHeadsReadOnlyCancellationAndPendingDelivery) {
    publish();
    auto view = open();
    const auto selected = root(view);
    auto store = cache::TaskStore::open(directory / "topology", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    TopologyTask task(binding(view), task_id, selected);
    const auto page = task.advance({3, 4, 2, 2});
    ASSERT_TRUE(page.sequence);
    task.cancel();
    const auto saved = task.save(*store, 0);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->status, cache::TaskStatus::Cancelled);
    unsigned reads = 0;
    cache::TaskStoreOptions guarded;
    guarded.store.fault = [&](cache::IoOperation operation, const fs::path&) {
        reads += operation == cache::IoOperation::ReadFile ? 1U : 0U;
        return std::error_code{};
    };
    auto bounded_reader = cache::TaskStore::open(
        directory / "topology", task_id, cache::TaskAccess::ReadOnly, guarded);
    ASSERT_TRUE(bounded_reader);
    reads = 0;
    EXPECT_EQ(TopologyTask::restore(
                  binding(view), *bounded_reader, *saved, selected,
                  {.bytes = static_cast<std::size_t>(saved->manifest.size - 1)})
                  .error()
                  .code,
              CheckpointErrorCode::LimitExceeded);
    EXPECT_EQ(reads,
              0U); // Reject the reference size before reading its payload.
    auto reader = cache::TaskStore::open(directory / "topology", task_id,
                                         cache::TaskAccess::ReadOnly);
    ASSERT_TRUE(reader);
    auto restored =
        TopologyTask::restore(binding(open()), *reader, *saved, selected);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->progress(), QueryProgress::Cancelled);
    EXPECT_EQ(restored->advance().records, page.records);
    ASSERT_TRUE(restored->acknowledge(page.sequence));
    EXPECT_EQ(restored->advance().progress, QueryProgress::Cancelled);
    restored->resume();
    (void)finish_topology(*restored, {7, 11, 3, 5});
    EXPECT_EQ(restored->save(*reader, 1).error().storage->code,
              cache::StoreErrorCode::ReadOnly);
    const auto completed = restored->save(*store, 1);
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed->status, cache::TaskStatus::Completed);
    EXPECT_EQ(restored->save(*store, 1).error().storage->code,
              cache::StoreErrorCode::StaleGeneration);
    TopologyTask pending(binding(view), task_id, selected);
    const auto all = pending.advance(QuerySlice::unlimited());
    ASSERT_EQ(all.progress, QueryProgress::Completed);
    ASSERT_TRUE(all.sequence);
    const auto awaiting = pending.save(*store, 2);
    ASSERT_TRUE(awaiting);
    EXPECT_EQ(awaiting->status, cache::TaskStatus::AwaitingDelivery);
    const auto retained = cache::reclaim_tasks(directory / "topology");
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->active, 1U);
    auto bad_status = *awaiting;
    bad_status.status = cache::TaskStatus::Completed;
    EXPECT_EQ(TopologyTask::restore(binding(view), *store, bad_status, selected)
                  .error()
                  .code,
              CheckpointErrorCode::Corrupt);
    // Older authenticated heads remain independently readable.
    ASSERT_TRUE(
        TopologyTask::restore(binding(view), *reader, *saved, selected));
}
TEST_F(QueryCheckpointTest,
       TopologyRejectsWrongBindingKindLimitsAndResealedCursors) {
    publish();
    auto view = open();
    const auto selected = root(view);
    TopologyTask task(binding(view), task_id, selected);
    (void)task.advance({2, 3, 1, 2});
    const auto bytes = task.checkpoint();
    ASSERT_TRUE(bytes);
    auto wrong = task_id;
    ++wrong.bytes[0];
    EXPECT_EQ(TopologyTask::restore(binding(view), wrong, selected, *bytes)
                  .error()
                  .code,
              CheckpointErrorCode::BindingMismatch);
    EXPECT_EQ(
        TopologyTask::restore(binding(view), task_id, root(view, "rr"), *bytes)
            .error()
            .code,
        CheckpointErrorCode::BindingMismatch);
    const auto changed = view.checkpoint_binding(source_text + " ");
    ASSERT_TRUE(changed);
    EXPECT_EQ(
        TopologyTask::restore(*changed, task_id, selected, *bytes).error().code,
        CheckpointErrorCode::BindingMismatch);
    EXPECT_EQ(TopologyTask::restore(binding(view), task_id, selected, *bytes,
                                    {.elements = 0})
                  .error()
                  .code,
              CheckpointErrorCode::LimitExceeded);
    EXPECT_EQ(task.checkpoint({.bytes = 31}).error().code,
              CheckpointErrorCode::LimitExceeded);
    const auto reseal = [](cache::Bytes value) {
        value.resize(value.size() - 32);
        const auto hash = cache::fingerprint("pagos.topology-task.v1", value);
        value.insert(value.end(), hash.bytes.begin(), hash.bytes.end());
        return value;
    };
    for (const auto offset :
         {9U, 13U, 17U, 153U, 242U, 250U, 258U, 266U, 269U, 277U, 285U, 297U}) {
        SCOPED_TRACE(offset);
        auto changed_bytes = *bytes;
        changed_bytes[offset] = 255;
        auto invalid = TopologyTask::restore(binding(view), task_id, selected,
                                             reseal(changed_bytes));
        EXPECT_FALSE(invalid);
    }
    TopologyTask completed(binding(view), task_id, selected);
    (void)completed.advance(QuerySlice::unlimited());
    const auto full = completed.checkpoint();
    ASSERT_TRUE(full);
    const auto payload = std::span(*full).first(full->size() - 32);
    cache::wire::Reader layout(payload, {});
    (void)layout.raw(293); // Fixed control fields, before the physical queue.
    (void)layout.raw(layout.count(4) * 4);
    const auto formulas = layout.count(10);
    ASSERT_GE(formulas, 2U);
    std::vector<std::size_t> formula_offsets;
    std::size_t first_term = 0;
    for (std::uint32_t i = 0; i < formulas; ++i) {
        formula_offsets.push_back(payload.size() - layout.remaining());
        (void)layout.raw(6);
        const auto terms = layout.count(4);
        if (terms && !first_term)
            first_term = payload.size() - layout.remaining();
        (void)layout.raw(terms * 4);
    }
    (void)layout.raw(layout.count(4) * 4); // Calls.
    (void)layout.raw(layout.count(4) * 4); // Local queue (empty at completion).
    ASSERT_TRUE(layout.boolean());
    (void)layout.u8(); // Original delivery progress.
    ASSERT_GT(layout.count(15), 0U);
    const auto record_node = payload.size() - layout.remaining() + 1;
    ASSERT_TRUE(layout.good());
    ASSERT_NE(first_term, 0U);
    for (unsigned damage = 0; damage < 4; ++damage) {
        SCOPED_TRACE(damage);
        auto invalid = *full;
        if (damage == 0)
            invalid[formula_offsets[0] + 5] = 3; // Unknown fallback.
        else if (damage == 1)
            std::fill_n(invalid.begin() +
                            static_cast<std::ptrdiff_t>(first_term),
                        4, 0);
        else if (damage == 2) // Duplicate root/channel inventory entry.
            std::copy_n(invalid.begin() +
                            static_cast<std::ptrdiff_t>(formula_offsets[0]),
                        5,
                        invalid.begin() +
                            static_cast<std::ptrdiff_t>(formula_offsets[1]));
        else
            std::fill_n(invalid.begin() +
                            static_cast<std::ptrdiff_t>(record_node),
                        4, 0);
        EXPECT_FALSE(TopologyTask::restore(binding(view), task_id, selected,
                                           reseal(invalid)));
    }
    auto truncated = *bytes;
    truncated.pop_back();
    EXPECT_FALSE(
        TopologyTask::restore(binding(view), task_id, selected, truncated));
    QueryTask native(binding(view), task_id, selected);
    const auto native_bytes = native.checkpoint();
    ASSERT_TRUE(native_bytes);
    EXPECT_FALSE(
        TopologyTask::restore(binding(view), task_id, selected, *native_bytes));
    EXPECT_FALSE(QueryTask::restore(binding(view), task_id, selected,
                                    QueryKind::Value, false, *bytes));
}
TEST_F(QueryCheckpointTest,
       TopologyPublicationFailureRetainsPageAndNativeHeads) {
    publish();
    auto view = open();
    const auto selected = root(view);
    bool fail = false;
    cache::TaskStoreOptions options;
    options.store.fault = [&](cache::IoOperation operation, const fs::path&) {
        return fail && operation == cache::IoOperation::PublishManifest
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto store = cache::TaskStore::open(directory / "topology", task_id,
                                        cache::TaskAccess::ReadWrite, options);
    ASSERT_TRUE(store);
    TopologyTask task(binding(view), task_id, selected);
    ASSERT_TRUE(task.save(*store, 0));
    const auto page = task.advance({2, 3, 1, 2});
    const auto work = task.stats();
    fail = true;
    EXPECT_FALSE(task.save(*store, 1));
    EXPECT_EQ((**store->head()).generation, 1U);
    EXPECT_EQ(task.advance().records, page.records);
    equal_topology_work(task.stats(), work);
    fail = false;
    ASSERT_TRUE(task.save(*store, 1));
    const auto head = **store->head();
    QueryTask native(binding(view), task_id, selected);
    EXPECT_FALSE(native.save(*store, head.generation));
    EXPECT_EQ((**store->head()).generation, head.generation);
    auto native_store = cache::TaskStore::open(directory / "native", task_id,
                                               cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(native_store);
    ASSERT_TRUE(native.save(*native_store, 0));
    EXPECT_FALSE(task.save(*native_store, 1));
    EXPECT_EQ((**native_store->head()).generation, 1U);
}
TEST_F(QueryCheckpointTest,
       TopologyFailedEvidenceAndMissingActiveBlocksCannotCheckpoint) {
    publish();
    auto view = open();
    const auto selected = root(view);
    TopologyTask task(binding(view), task_id, selected);
    (void)task.advance({1, 0, 0, 1});
    const auto bytes = task.checkpoint();
    ASSERT_TRUE(bytes);
    // Keep the authenticated hot closure but remove all cold detail objects.
    auto closure =
        FunctionEvidence::load(cache::FileStore(directory / "evidence"),
                               identity, cache_test::context());
    ASSERT_TRUE(closure);
    for (const auto& group : closure->groups()) {
        const auto hash = group.detail.digest.hex();
        fs::remove(directory / "evidence" / "objects" / hash.substr(0, 2) /
                   hash);
    }
    auto cold = open();
    const auto rejected =
        TopologyTask::restore(binding(cold), task_id, selected, *bytes);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, CheckpointErrorCode::UnavailableEvidence);
    TopologyTask unavailable(binding(cold), task_id, selected);
    const auto page = unavailable.advance();
    EXPECT_EQ(page.progress, QueryProgress::Failed);
    ASSERT_TRUE(page.failure);
    EXPECT_EQ(unavailable.checkpoint().error().code,
              CheckpointErrorCode::InvalidState);
}
TEST_F(QueryCheckpointTest,
       TopologyRestoredIndexOwnsEvidenceAndSelectedInvocation) {
    publish();
    std::shared_ptr<const FormulaIndex> index;
    std::vector<NodeId> calls;
    {
        auto view = open();
        const auto selected = root(view);
        TopologyTask task(binding(view), task_id, selected);
        (void)finish_topology(task);
        const auto bytes = task.checkpoint();
        ASSERT_TRUE(bytes);
        auto restored =
            TopologyTask::restore(binding(view), task_id, selected, *bytes);
        ASSERT_TRUE(restored);
        index = restored->index();
        ASSERT_TRUE(index);
        EXPECT_GT(index->stats().formula_capacity_bytes, 0U);
        QueryTask raw(view.view(), selected);
        (void)raw.advance(QuerySlice::unlimited());
        const auto result = raw.result();
        for (const auto& source : result.sources) {
            if (!source.context)
                continue;
            for (auto frame = source.context; frame;
                 frame = result.invocations[frame].parent)
                calls.push_back(result.invocations[frame].call);
            break;
        }
        std::ranges::reverse(calls);
    }
    ASSERT_FALSE(calls.empty());
    auto selected = index->begin_invocation(calls);
    (void)selected.advance(QuerySlice::unlimited());
    EXPECT_FALSE(selected.result().sources.empty());
    auto witness = index->begin_invocation(calls, QueryKind::Value, true);
    (void)witness.advance(QuerySlice::unlimited());
    EXPECT_FALSE(witness.result().witness.empty());
    // A disk read failure in this synchronous factory is a public exception,
    // never the private transport exception used by topology advancement.
    const auto root_path = directory / "evidence" / "objects";
    fs::remove_all(root_path);
    selected.evidence().evict();
    EXPECT_THROW((void)index->begin_invocation(calls), std::runtime_error);
}
std::size_t topology_bytes(const TopologyStateMemory& value) {
    return value.queue + value.visited + value.formula_records +
           value.formula_terms + value.formula_index + value.calls +
           value.local_queue + value.local_seen + value.families +
           value.delivery;
}
TEST_F(QueryCheckpointTest,
       TopologySegmentedEveryPhaseOffloadsAndRestoresAcknowledgedPages) {
    publish();
    for (auto name : {"aa", "rr"}) {
        auto view = open();
        const auto selected = root(view, name);
        auto id = task_id;
        id.bytes[0] = name == std::string_view("aa") ? 0 : 1;
        auto store = cache::TaskStore::open(directory / "topology", id,
                                            cache::TaskAccess::ReadWrite);
        ASSERT_TRUE(store);
        TopologyTask task(binding(view), id, selected),
            oracle(view.view(), selected);
        const auto expected = finish_topology(oracle);
        std::vector<TopologyRecord> records;
        std::uint64_t generation = 0;
        for (unsigned step = 0; step < 10000; ++step) {
            const auto page = task.advance({1, 1, 1, 1});
            ASSERT_NE(page.progress, QueryProgress::Failed);
            const auto work = task.stats();
            const auto saved = task.offload(*store, generation, 90);
            ASSERT_TRUE(saved) << static_cast<int>(saved.error().code);
            generation = saved->generation;
            EXPECT_TRUE(task.state_memory().offloaded);
            EXPECT_EQ(topology_bytes(task.state_memory()), 0U);
            EXPECT_GT(task.storage_stats().segment_writes, 0U);
            auto reader = cache::TaskStore::open(directory / "topology", id,
                                                 cache::TaskAccess::ReadOnly);
            ASSERT_TRUE(reader);
            auto restored = TopologyTask::restore(binding(open()), *reader,
                                                  *saved, selected);
            ASSERT_TRUE(restored) << static_cast<int>(restored.error().code);
            equal_topology_work(work, restored->stats());
            EXPECT_GT(restored->storage_stats().segment_reads, 0U);
            task = std::move(*restored);
            if (page.sequence) {
                const auto replay = task.advance({0, 0, 0, 0});
                EXPECT_EQ(replay.sequence, page.sequence);
                EXPECT_EQ(replay.records, page.records);
                ASSERT_TRUE(task.acknowledge(page.sequence));
                records.insert(records.end(), page.records.begin(),
                               page.records.end());
            }
            if (page.progress == QueryProgress::Completed)
                break;
            ASSERT_LT(step, 9999U);
        }
        EXPECT_EQ(records, expected);
        equal_formulas(*task.index(), *oracle.index());
        equal_topology_work(task.stats(), oracle.stats());
        // The byte v3 compatibility encoder still works after a v4 restore.
        const auto old = task.checkpoint();
        ASSERT_TRUE(old);
        ASSERT_TRUE(TopologyTask::restore(binding(view), id, selected, *old));
    }
}
TEST_F(QueryCheckpointTest,
       TopologyOffloadReleasesWideFrontierAndRetriesReadFailure) {
    publish(wide_program(513));
    auto view = open(8 * 1024 * 1024);
    const auto selected = aggregate_root(view, root(view));
    bool fail = false;
    cache::TaskStoreOptions options;
    options.store.fault = [&](cache::IoOperation operation, const fs::path&) {
        return fail && operation == cache::IoOperation::ReadFile
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto store = cache::TaskStore::open(directory / "topology", task_id,
                                        cache::TaskAccess::ReadWrite, options);
    ASSERT_TRUE(store);
    TopologyTask task(binding(view), task_id, selected),
        oracle(view.view(), selected);
    (void)finish_topology(oracle);
    const auto page = task.advance({1, 256, 0, 257});
    const auto before = task.state_memory();
    EXPECT_GE(before.queue, 257 * sizeof(NodeId));
    EXPECT_GT(before.visited, 0U);
    const auto work = task.stats();
    ASSERT_TRUE(task.offload(*store, 0, 256));
    EXPECT_EQ(topology_bytes(task.state_memory()), 0U);
    fail = true;
    EXPECT_EQ(task.advance().progress, QueryProgress::Paused);
    ASSERT_TRUE(task.storage_failure());
    EXPECT_EQ(topology_bytes(task.state_memory()), 0U);
    equal_topology_work(work, task.stats());
    task.cancel();
    EXPECT_EQ(task.advance().progress, QueryProgress::Cancelled);
    fail = false;
    const auto replay = task.advance();
    EXPECT_EQ(replay.sequence, page.sequence);
    EXPECT_EQ(replay.records, page.records);
    EXPECT_EQ(task.progress(), QueryProgress::Cancelled);
    ASSERT_TRUE(task.acknowledge(page.sequence));
    EXPECT_EQ(task.advance().progress, QueryProgress::Cancelled);
    task.resume();
    (void)finish_topology(task, {17, 23, 11, 7});
    ASSERT_FALSE(task.storage_failure());
    equal_formulas(*task.index(), *oracle.index());
    equal_topology_work(task.stats(), oracle.stats());
}
TEST_F(QueryCheckpointTest,
       TopologyOffloadDetachesPublishedIndexWithoutInvalidatingItsQueries) {
    publish();
    auto view = open(1024 * 1024);
    const auto selected = root(view);
    auto store = cache::TaskStore::open(directory / "topology", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    TopologyTask task(binding(view), task_id, selected);
    (void)finish_topology(task);
    auto published = task.index();
    ASSERT_TRUE(published);
    const auto work = task.stats();
    auto selected_query = published->begin_sources(selected);
    ASSERT_TRUE(task.offload(*store, 0, 90));
    EXPECT_EQ(topology_bytes(task.state_memory()), 0U);
    EXPECT_GT(task.state_memory().retained_index, 0U);
    EXPECT_FALSE(published->formulas().empty());
    (void)selected_query.advance(QuerySlice::unlimited());
    EXPECT_FALSE(selected_query.result().sources.empty());
    const auto restored = task.index(); // Explicit materialization.
    ASSERT_TRUE(restored);
    equal_formulas(*restored, *published);
    equal_topology_work(work, task.stats());
    ASSERT_TRUE(task.offload(*store, 1, 90));
    EXPECT_GT(task.state_memory().retained_index, 0U);
}
TEST_F(QueryCheckpointTest,
       TopologySegmentedLimitsReuseAndFailedSavePreserveOldHead) {
    publish();
    auto view = open();
    const auto selected = root(view);
    auto store = cache::TaskStore::open(directory / "topology", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    TopologyTask task(binding(view), task_id, selected);
    (void)task.advance({2, 3, 1, 2});
    EXPECT_EQ(task.save_segmented(*store, 0, 71).error().code,
              CheckpointErrorCode::LimitExceeded);
    EXPECT_EQ(task.save_segmented(*store, 0, 90, {.bytes = 350}).error().code,
              CheckpointErrorCode::LimitExceeded);
    EXPECT_FALSE(fs::exists(store->directory() / "objects"));
    const auto first = task.save_segmented(*store, 0, 90);
    ASSERT_TRUE(first);
    const auto second = task.save_segmented(*store, 1, 90);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->manifest, second->manifest);
    EXPECT_EQ(task.storage_stats().cas_bytes_added, 0U);
    EXPECT_GT(task.storage_stats().reused_objects, 0U);
    const auto retained = topology_bytes(task.state_memory());
    EXPECT_FALSE(task.offload(*store, 1, 90));
    EXPECT_EQ(topology_bytes(task.state_memory()), retained);
    auto reader = cache::TaskStore::open(directory / "topology", task_id,
                                         cache::TaskAccess::ReadOnly);
    ASSERT_TRUE(reader);
    EXPECT_EQ(task.offload(*reader, 2, 90).error().storage->code,
              cache::StoreErrorCode::ReadOnly);
    EXPECT_EQ(topology_bytes(task.state_memory()), retained);
    // Explicit v3 publication may update a v4 topology head and vice versa.
    const auto old = task.save(*store, 2);
    ASSERT_TRUE(old);
    ASSERT_TRUE(task.save_segmented(*store, 3, 90));
    ASSERT_TRUE(TopologyTask::restore(binding(view), *store, *old, selected));
}
TEST_F(QueryCheckpointTest,
       TopologySegmentedTablesSurpassSingleObjectLimitAndValidateDirectory) {
    publish(wide_program(257));
    auto view = open(8 * 1024 * 1024);
    const auto selected = root(view);
    auto store = cache::TaskStore::open(directory / "topology", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    TopologyTask task(binding(view), task_id, selected);
    (void)finish_topology(task, QuerySlice::unlimited());
    EXPECT_FALSE(task.checkpoint({.bytes = 4096}));
    auto saved = task.save_segmented(*store, 0, 1024, {.bytes = 4096});
    ASSERT_TRUE(saved);
    auto restored = TopologyTask::restore(binding(view), *store, *saved,
                                          selected, {.bytes = 4096});
    ASSERT_TRUE(restored);
    equal_formulas(*task.index(), *restored->index());
    auto bytes = store->read(saved->manifest);
    ASSERT_TRUE(bytes);
    // First table descriptor follows the fixed control + delivery fields and
    // two counts.
    constexpr auto descriptor = 295 + 8;
    (*bytes)[descriptor] = 1; // Queue must start at zero.
    bytes->resize(bytes->size() - 32);
    const auto checksum = cache::fingerprint("pagos.topology-task.v1", *bytes);
    bytes->insert(bytes->end(), checksum.bytes.begin(), checksum.bytes.end());
    auto writer = store->begin(saved->generation);
    ASSERT_TRUE(writer);
    auto root_ref =
        writer->write(cache::ObjectKind::QueryTaskManifest, std::move(*bytes));
    ASSERT_TRUE(root_ref);
    auto malformed = writer->commit(*root_ref, saved->status);
    ASSERT_TRUE(malformed);
    EXPECT_EQ(TopologyTask::restore(binding(view), *store, *malformed, selected)
                  .error()
                  .code,
              CheckpointErrorCode::Corrupt);
}
TEST_F(QueryCheckpointTest,
       TopologySegmentedQuotaPublicationFaultAndResealedLeafAreRecoverable) {
    publish(wide_program(64));
    auto view = open(4 * 1024 * 1024);
    const auto selected = aggregate_root(view, root(view));
    cache::TaskStoreOptions quota;
    quota.disk_bytes = 2048;
    auto small = cache::TaskStore::open(directory / "topology", task_id,
                                        cache::TaskAccess::ReadWrite, quota);
    ASSERT_TRUE(small);
    TopologyTask task(binding(view), task_id, selected);
    ASSERT_TRUE(task.save_segmented(*small, 0, 90));
    const auto page = task.advance({1, 64, 0, 65});
    const auto memory = topology_bytes(task.state_memory());
    EXPECT_EQ(task.offload(*small, 1, 90).error().code,
              CheckpointErrorCode::LimitExceeded);
    EXPECT_EQ((**small->head()).generation, 1U);
    EXPECT_EQ(topology_bytes(task.state_memory()), memory);
    bool fail = true;
    cache::TaskStoreOptions options;
    options.store.fault = [&](cache::IoOperation operation,
                              const fs::path& path) {
        return fail && operation == cache::IoOperation::PublishManifest &&
                       path.filename() == "head"
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto larger = cache::TaskStore::open(directory / "topology", task_id,
                                         cache::TaskAccess::ReadWrite, options);
    ASSERT_TRUE(larger);
    EXPECT_FALSE(task.offload(*larger, 1, 90));
    EXPECT_EQ((**larger->head()).generation, 1U);
    EXPECT_EQ(task.advance().records, page.records);
    fail = false;
    const auto saved = task.offload(*larger, 1, 90);
    ASSERT_TRUE(saved);
    auto manifest = larger->read(saved->manifest);
    ASSERT_TRUE(manifest);
    cache::wire::Reader descriptor{std::span(*manifest).subspan(295 + 8), {}};
    EXPECT_EQ(descriptor.u32(), 0U);
    ASSERT_GT(descriptor.u32(), 0U);
    cache::ObjectRef ref;
    ref.kind = static_cast<cache::ObjectKind>(descriptor.u8());
    const auto hash = descriptor.raw(32);
    std::ranges::copy(hash, ref.digest.bytes.begin());
    ref.size = descriptor.u64();
    auto payload = larger->read(ref);
    ASSERT_TRUE(payload);
    (*payload)[13] ^= 1; // Authentic CAS with the wrong segment task identity.
    auto writer = larger->begin(saved->generation);
    ASSERT_TRUE(writer);
    const auto invalid_leaf = writer->write(
        cache::ObjectKind::QueryStateSegment, std::move(*payload));
    ASSERT_TRUE(invalid_leaf);
    std::ranges::copy(invalid_leaf->digest.bytes,
                      manifest->begin() + 295 + 8 + 9);
    manifest->resize(manifest->size() - 32);
    const auto checksum =
        cache::fingerprint("pagos.topology-task.v1", *manifest);
    manifest->insert(manifest->end(), checksum.bytes.begin(),
                     checksum.bytes.end());
    const auto invalid_root = writer->write(
        cache::ObjectKind::QueryTaskManifest, std::move(*manifest));
    ASSERT_TRUE(invalid_root);
    const auto bad = writer->commit(*invalid_root, saved->status);
    ASSERT_TRUE(bad);
    EXPECT_EQ(TopologyTask::restore(binding(view), *larger, *bad, selected)
                  .error()
                  .code,
              CheckpointErrorCode::Corrupt);
    // A retained old head and the offloaded task both still refer to valid
    // leaves.
    ASSERT_TRUE(
        TopologyTask::restore(binding(view), *larger, *saved, selected));
    EXPECT_EQ(task.advance().records, page.records);
    EXPECT_FALSE(task.storage_failure());
}
} // namespace
