#include "../../lib/cache/wire.h"
#include "cache_test_input.h"
#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/provenance_recursive.h"
#include "pagos/stage/stage_analyzer.h"

#include <atomic>
#include <fstream>
#include <iostream>
#include <set>

namespace {
using namespace pagos;
using namespace pagos::stage;
using namespace pagos::stage::provenance;
namespace fs = std::filesystem;
const auto task_id = cache::fingerprint("test.recursive-checkpoint", {});
const std::string program =
    "fn rotate(x: u32, y: u32) -> u32 { if x == 0 { y } else { rotate(y, x - "
    "1) } } "
    "fn read(x: u32) -> u32 { if x == 0 { external_input() } else { read(x - "
    "1) } } "
    "fn known(x: u32) -> u32 { if x == 0 { 0 } else { known(x - 1) }; 7 } "
    "fn even(x: u32) -> u32 { if x == 0 { 1 } else { odd(x - 1) } } "
    "fn odd(x: u32) -> u32 { if x == 0 { 0 } else { even(x - 1) } } "
    "let a = external_input(); let b = external_input(); let aa = [rotate(a, "
    "b), read(a), known(b), even(a)];";
class RecursiveCheckpointTest : public ::testing::Test {
  protected:
    RecursiveCheckpointTest() {
        static std::atomic<unsigned> sequence{};
        directory =
            fs::temp_directory_path() /
            ("pagos-recursive-checkpoint-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    }
    ~RecursiveCheckpointTest() override {
        std::error_code error;
        fs::remove_all(directory, error);
    }
    void SetUp() override {
        cache_test::Input input(program);
        identity = *input.identity();
        StageAnalyzer analyzer(input.diagnostics, input.checker.types(), {},
                               cache_test::context());
        auto module = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        ASSERT_TRUE(FunctionEvidence::publish(cache::FileStore(directory),
                                              *module->provenance, identity,
                                              cache_test::context()));
    }
    FunctionEvidenceView open(std::size_t cache = 1024 * 1024) {
        auto closure = FunctionEvidence::load(cache::FileStore(directory),
                                              identity, cache_test::context());
        if (!closure)
            throw std::runtime_error("missing closure");
        auto view = FunctionEvidenceView::open(std::move(*closure), cache);
        if (!view)
            throw std::runtime_error("missing view");
        return std::move(*view);
    }
    QueryCheckpointBinding binding(const FunctionEvidenceView& view,
                                   std::string text = program) {
        return *view.checkpoint_binding(text);
    }
    SummaryId summary(const FunctionEvidenceView& view,
                      std::string_view name = "rotate") {
        for (const auto& entry : view.summary_catalog())
            if (view.scopes()[entry.id.value].name == name)
                return entry.id;
        throw std::runtime_error("missing summary");
    }
    fs::path directory;
    CacheIdentity identity;
};
void finish_task(RecursiveSummaryTask& task) {
    for (std::size_t i = 0;
         i < 10000 && (task.progress() == QueryProgress::Running ||
                       task.progress() == QueryProgress::Paused);
         ++i)
        (void)task.advance(4096);
    ASSERT_EQ(task.progress(), QueryProgress::Completed);
}
void same_work(const RecursiveStats& left, const RecursiveStats& right) {
    EXPECT_EQ(left.steps, right.steps);
    EXPECT_EQ(left.nodes, right.nodes);
    EXPECT_EQ(left.links, right.links);
    EXPECT_EQ(left.components, right.components);
    EXPECT_EQ(left.cyclic_components, right.cyclic_components);
    EXPECT_EQ(left.evaluations, right.evaluations);
    EXPECT_EQ(left.facts_added, right.facts_added);
    EXPECT_EQ(left.queue_peak, right.queue_peak);
}
std::uint8_t phase(const cache::Bytes& bytes) {
    // Wire header, four versions, four digests, summary, channel, progress.
    return bytes.at(9 + 16 + 128 + 4 + 1 + 1);
}
void resign(cache::Bytes& bytes) {
    bytes.resize(bytes.size() - 32);
    const auto hash = cache::fingerprint("pagos.recursive-task.v1", bytes);
    bytes.insert(bytes.end(), hash.bytes.begin(), hash.bytes.end());
}
struct Field {
    std::size_t offset, width;
    std::string kind;
};
// Inspect v5 reference fields independently of the runtime's private Impl.
std::vector<Field> references(const cache::Bytes& bytes) {
    const auto payload = std::span(bytes).first(bytes.size() - 32);
    cache::wire::Reader reader(payload, {});
    std::vector<Field> fields;
    const auto at = [&] { return payload.size() - reader.remaining(); };
    const auto u64 = [&](std::string name) {
        fields.push_back({at(), 8, std::move(name)});
        (void)reader.u64();
    };
    const auto node = [&](std::string name) {
        fields.push_back({at(), 4, std::move(name)});
        (void)reader.u32();
    };
    (void)reader.raw(160 + 9 * 8); // Binding, progress/phase, work counters.
    u64("phase-cursor");
    (void)reader.boolean();
    u64("physical-edge");
    (void)reader.boolean();
    auto count = reader.count(14);
    for (std::uint32_t i = 0; i < count; ++i) {
        node("equation");
        (void)reader.u32();
        (void)reader.boolean();
        (void)reader.boolean();
        const auto facts = reader.count(13);
        (void)reader.raw(facts * 13);
    }
    count = reader.count(14);
    for (std::uint32_t i = 0; i < count; ++i) {
        node("link-from");
        node("link-to");
        (void)reader.raw(6);
    }
    count = reader.count(5);
    for (std::uint32_t i = 0; i < count; ++i) {
        (void)reader.boolean();
        const auto nodes = reader.count(4);
        for (std::uint32_t j = 0; j < nodes; ++j)
            node("component-node");
    }
    count = reader.count();
    (void)reader.raw(count);
    count = reader.count(16);
    for (std::uint32_t i = 0; i < count; ++i) {
        u64("dfs-node");
        u64("dfs-edge");
    }
    count = reader.count(8);
    for (std::uint32_t i = 0; i < count; ++i)
        u64("finish-order");
    u64("component-cursor");
    u64("member-cursor");
    count = reader.count(8);
    for (std::uint32_t i = 0; i < count; ++i)
        u64("worklist");
    if (reader.boolean()) {
        u64("transfer-node");
        u64("transfer-input");
        u64("notify-user");
        const auto bound_at = at();
        const auto bound = reader.u64();
        if (bound != UINT64_MAX)
            fields.push_back({bound_at, 8, "bound-argument"});
        const bool started = reader.boolean();
        (void)reader.boolean();
        const bool notifying = reader.boolean();
        const auto iterator = [&](std::string name) {
            if (reader.boolean()) {
                (void)reader.raw(5); // Kind and owner; slot follows.
                node(std::move(name));
                (void)reader.u32();
            }
        };
        if (started && !notifying)
            iterator("source-iterator");
        if (bound != UINT64_MAX)
            iterator("argument-iterator");
    }
    EXPECT_TRUE(reader.finish());
    return fields;
}
TEST_F(RecursiveCheckpointTest,
       EveryPrimitiveBoundaryRestoresFactsCyclesAndWork) {
    auto view = open();
    std::set<unsigned> phases;
    for (const auto& entry : view.summary_catalog())
        for (const auto channel : {QueryKind::Value, QueryKind::Effects}) {
            RecursiveSummaryTask baseline(binding(view), task_id, entry.id,
                                          channel);
            finish_task(baseline);
            RecursiveSummaryTask task(binding(view), task_id, entry.id,
                                      channel);
            std::size_t turns = 0;
            while (turns++ < 10000) {
                const auto progress = task.progress();
                const bool cancelled =
                    turns % 3 == 0 && progress != QueryProgress::Completed;
                if (cancelled)
                    task.cancel();
                const auto before = task.result();
                const auto bytes = task.checkpoint();
                ASSERT_TRUE(bytes);
                phases.insert(phase(*bytes));
                auto restored = RecursiveSummaryTask::restore(
                    binding(view), task_id, entry.id, channel, *bytes);
                ASSERT_TRUE(restored)
                    << static_cast<int>(restored.error().code)
                    << " phase=" << static_cast<unsigned>(phase(*bytes))
                    << " step=" << task.stats().steps;
                EXPECT_EQ(restored->result(), before);
                same_work(restored->stats(), task.stats());
                EXPECT_EQ(restored->stats().slices, task.stats().slices);
                EXPECT_EQ(restored->progress(), task.progress());
                EXPECT_EQ(
                    std::vector(restored->links().begin(),
                                restored->links().end()),
                    std::vector(task.links().begin(), task.links().end()));
                EXPECT_EQ(std::vector(restored->components().begin(),
                                      restored->components().end()),
                          std::vector(task.components().begin(),
                                      task.components().end()));
                ASSERT_TRUE(restored->checkpoint());
                EXPECT_EQ(*restored->checkpoint(), *bytes);
                task = std::move(*restored);
                if (cancelled) {
                    EXPECT_EQ(task.advance(), QueryProgress::Cancelled);
                    task.resume();
                }
                if (task.progress() == QueryProgress::Completed)
                    break;
                (void)task.advance(1);
            }
            ASSERT_LT(turns, 10000U);
            EXPECT_EQ(task.result(), baseline.result());
            same_work(task.stats(), baseline.stats());
        }
    EXPECT_EQ(phases, (std::set<unsigned>{0, 1, 2, 3, 4, 5, 6}));
}

TEST_F(RecursiveCheckpointTest,
       BindingVersionsAndCodecLimitsRejectWithoutMutation) {
    auto view = open();
    const auto id = summary(view);
    RecursiveSummaryTask task(binding(view), task_id, id);
    (void)task.advance(60);
    const auto bytes = task.checkpoint();
    ASSERT_TRUE(bytes);
    const auto restore = [&](const cache::Bytes& data,
                             cache::Limits limits = {}) {
        return RecursiveSummaryTask::restore(binding(view), task_id, id,
                                             QueryKind::Value, data, limits);
    };
    auto wrong = RecursiveSummaryTask::restore(
        binding(view, program + " "), task_id, id, QueryKind::Value, *bytes);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, CheckpointErrorCode::BindingMismatch);
    wrong = RecursiveSummaryTask::restore(binding(view), {}, id,
                                          QueryKind::Value, *bytes);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, CheckpointErrorCode::BindingMismatch);
    wrong = RecursiveSummaryTask::restore(binding(view), task_id,
                                          summary(view, "read"),
                                          QueryKind::Value, *bytes);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, CheckpointErrorCode::BindingMismatch);
    wrong = RecursiveSummaryTask::restore(binding(view), task_id, id,
                                          QueryKind::Effects, *bytes);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, CheckpointErrorCode::BindingMismatch);
    for (const auto offset : {25U, 57U, 89U, 121U}) {
        auto corrupt = *bytes;
        corrupt[offset] ^= 1;
        resign(corrupt);
        auto result = restore(corrupt);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, CheckpointErrorCode::BindingMismatch);
    }
    for (const auto offset : {9U, 13U, 17U, 21U}) {
        auto corrupt = *bytes;
        corrupt[offset] = 255;
        resign(corrupt);
        auto result = restore(corrupt);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code,
                  CheckpointErrorCode::IncompatibleVersion);
    }
    for (const auto offset : {158U, 159U, 232U, 240U, 241U, 250U, 254U}) {
        auto corrupt = *bytes;
        corrupt[offset] = 255;
        resign(corrupt);
        auto result = restore(corrupt);
        EXPECT_FALSE(result) << offset;
    }
    for (std::size_t length : {0U, 32U, 156U}) {
        auto cut = *bytes;
        cut.resize(length);
        EXPECT_FALSE(restore(cut));
    }
    auto corrupt = *bytes;
    corrupt.back() ^= 1;
    EXPECT_FALSE(restore(corrupt));
    corrupt = *bytes;
    corrupt.insert(corrupt.end() - 32, 0);
    resign(corrupt);
    EXPECT_FALSE(restore(corrupt));
    auto limited = restore(*bytes, {.bytes = bytes->size() - 1});
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().code, CheckpointErrorCode::LimitExceeded);
    limited = restore(*bytes, {.elements = 1});
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().code, CheckpointErrorCode::LimitExceeded);
    EXPECT_TRUE(restore(*bytes, {.bytes = bytes->size()}));
    EXPECT_FALSE(task.checkpoint({.bytes = bytes->size() - 1}));
    EXPECT_TRUE(task.checkpoint({.bytes = bytes->size()}));
    EXPECT_EQ(*task.checkpoint(), *bytes);
    RecursiveSummaryTask unbound(view.view(), id);
    auto invalid = unbound.checkpoint();
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, CheckpointErrorCode::InvalidState);
}

TEST_F(RecursiveCheckpointTest,
       LoadedEvidenceIsValidatedAndUnloadedFrontierStaysLazy) {
    auto view = open(0);
    const auto id = summary(view, "read");
    RecursiveSummaryTask fresh(binding(view), task_id, id);
    const auto empty = fresh.checkpoint();
    ASSERT_TRUE(empty);
    EXPECT_EQ(view.view()->read_stats().objects_read, 0U);
    (void)fresh.advance(1);
    const auto active = fresh.checkpoint();
    ASSERT_TRUE(active);
    auto closure = FunctionEvidence::load(cache::FileStore(directory), identity,
                                          cache_test::context());
    ASSERT_TRUE(closure);
    for (const auto& group : closure->groups()) {
        const auto hash = group.detail.digest.hex();
        fs::remove(directory / "objects" / hash.substr(0, 2) / hash);
    }
    view.view()->evict();
    fresh = RecursiveSummaryTask(binding(view), task_id,
                                 id); // Release active lease.
    auto restored = RecursiveSummaryTask::restore(binding(view), task_id, id,
                                                  QueryKind::Value, *active);
    ASSERT_FALSE(restored);
    EXPECT_EQ(restored.error().code, CheckpointErrorCode::UnavailableEvidence);
    restored = RecursiveSummaryTask::restore(binding(view), task_id, id,
                                             QueryKind::Value, *empty);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->advance(1), QueryProgress::Failed);
    auto failed = restored->checkpoint();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, CheckpointErrorCode::InvalidState);
}

TEST_F(RecursiveCheckpointTest, InvalidReferencesAndIteratorValuesAreRejected) {
    auto view = open();
    const auto id = summary(view);
    RecursiveSummaryTask task(binding(view), task_id, id);
    std::set<std::string> covered;
    for (std::size_t turn = 0; turn < 10000; ++turn) {
        auto bytes = task.checkpoint();
        ASSERT_TRUE(bytes);
        for (const auto& field : references(*bytes)) {
            if (field.kind == "phase-cursor" || field.kind == "physical-edge" ||
                field.kind == "component-cursor" ||
                field.kind == "member-cursor")
                continue; // Some unused controls are intentionally canonical
                          // zero.
            if (covered.contains(field.kind))
                continue;
            auto corrupt = *bytes;
            std::fill_n(corrupt.begin() +
                            static_cast<std::ptrdiff_t>(field.offset),
                        field.width, 255);
            resign(corrupt);
            auto result = RecursiveSummaryTask::restore(
                binding(view), task_id, id, QueryKind::Value, corrupt);
            EXPECT_FALSE(result) << field.kind;
            if (!result) {
                EXPECT_EQ(result.error().code, CheckpointErrorCode::Corrupt)
                    << field.kind;
            }
            covered.insert(field.kind);
        }
        if (task.progress() == QueryProgress::Completed)
            break;
        (void)task.advance(1);
    }
    for (const auto* field :
         {"equation", "link-from", "link-to", "component-node", "dfs-node",
          "dfs-edge", "finish-order", "worklist", "transfer-node",
          "transfer-input", "notify-user", "bound-argument", "source-iterator",
          "argument-iterator"})
        EXPECT_TRUE(covered.contains(field)) << field;
}
std::size_t payload(const RecursiveStateMemory& memory) {
    return memory.equations + memory.facts + memory.relations + memory.indexes +
           memory.scc + memory.worklist;
}
TEST_F(RecursiveCheckpointTest,
       SegmentedPhaseAndTransferBoundariesPreserveResidentWireState) {
    auto view = open();
    for (const auto* name : {"rotate", "read"})
        for (const auto channel : {QueryKind::Value, QueryKind::Effects}) {
            const auto id = summary(view, name);
            const auto parent =
                directory /
                (std::string(name) + std::to_string(static_cast<int>(channel)));
            auto store = cache::TaskStore::open(parent, task_id,
                                                cache::TaskAccess::ReadWrite);
            ASSERT_TRUE(store);
            RecursiveSummaryTask baseline(binding(view), task_id, id, channel);
            finish_task(baseline);
            RecursiveSummaryTask task(binding(view), task_id, id, channel);
            std::uint64_t generation = 0;
            std::set<std::string> shapes;
            for (std::size_t turn = 0; turn < 10000; ++turn) {
                const auto before = task.checkpoint();
                ASSERT_TRUE(before);
                std::string shape = std::to_string(phase(*before));
                for (const auto& field : references(*before))
                    if (field.kind.ends_with("iterator") ||
                        field.kind == "bound-argument")
                        shape += field.kind;
                if (shapes.insert(shape).second) {
                    EXPECT_GT(payload(task.state_memory()), 0U);
                    auto saved = task.offload(*store, generation, 128);
                    ASSERT_TRUE(saved);
                    generation = saved->generation;
                    EXPECT_TRUE(task.state_memory().offloaded);
                    EXPECT_EQ(payload(task.state_memory()), 0U);
                    const auto writes = task.storage_stats().segment_writes;
                    EXPECT_GT(writes, 0U);
                    EXPECT_EQ(task.progress() == QueryProgress::Completed,
                              saved->status ==
                                  cache::TaskStatus::AwaitingDelivery);
                    auto after = task.checkpoint();
                    ASSERT_TRUE(after);
                    EXPECT_EQ(*before, *after);
                    EXPECT_FALSE(task.state_memory().offloaded);
                    EXPECT_EQ(task.storage_stats().segment_reads, writes);
                    EXPECT_GT(task.storage_stats().validation_payload_bytes,
                              0U);
                }
                if (task.progress() == QueryProgress::Completed)
                    break;
                (void)task.advance(1);
            }
            EXPECT_GE(shapes.size(), 7U);
            EXPECT_EQ(task.result(), baseline.result());
            same_work(task.stats(), baseline.stats());
            auto repeat = task.save_segmented(*store, generation, 128);
            ASSERT_TRUE(repeat);
            EXPECT_GT(task.storage_stats().reused_objects, 0U);
            EXPECT_EQ(task.storage_stats().cas_bytes_added, 0U);
        }
}
TEST_F(RecursiveCheckpointTest,
       SegmentedQuotaPublicationReadonlyAndStaleWritesRetainState) {
    auto view = open();
    const auto id = summary(view);
    RecursiveSummaryTask task(binding(view), task_id, id);
    (void)task.advance(50);
    auto store = cache::TaskStore::open(directory / "sessions", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    auto first = task.save_segmented(*store, 0, 128);
    ASSERT_TRUE(first);
    const auto wire = task.checkpoint();
    ASSERT_TRUE(wire);
    auto stale = task.offload(*store, 0, 128);
    ASSERT_FALSE(stale);
    ASSERT_TRUE(stale.error().storage);
    EXPECT_EQ(stale.error().storage->code,
              cache::StoreErrorCode::StaleGeneration);
    auto readonly = cache::TaskStore::open(directory / "sessions", task_id,
                                           cache::TaskAccess::ReadOnly);
    ASSERT_TRUE(readonly);
    auto failed = task.offload(*readonly, first->generation, 128);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().storage->code, cache::StoreErrorCode::ReadOnly);
    cache::TaskStoreOptions small;
    small.disk_bytes = 256;
    auto limited = cache::TaskStore::open(directory / "small", task_id,
                                          cache::TaskAccess::ReadWrite, small);
    ASSERT_TRUE(limited);
    failed = task.offload(*limited, 0, 128);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, CheckpointErrorCode::LimitExceeded);
    auto enough = cache::TaskStore::open(directory / "small", task_id,
                                         cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(enough);
    EXPECT_TRUE(task.save_segmented(*enough, 0, 128));
    cache::TaskStoreOptions fault;
    fault.store.fault = [](cache::IoOperation operation, const fs::path& path) {
        return operation == cache::IoOperation::PublishManifest &&
                       path.filename() == "head"
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto broken = cache::TaskStore::open(directory / "sessions", task_id,
                                         cache::TaskAccess::ReadWrite, fault);
    ASSERT_TRUE(broken);
    failed = task.offload(*broken, first->generation, 128);
    ASSERT_FALSE(failed);
    EXPECT_EQ(*task.checkpoint(), *wire);
    EXPECT_FALSE(task.state_memory().offloaded);
    EXPECT_EQ(**store->head(), *first);
    EXPECT_FALSE(task.save_segmented(*store, first->generation, 75));
    EXPECT_FALSE(
        task.save_segmented(*store, first->generation, 128, {.elements = 1}));
    const auto other_id = cache::fingerprint("wrong-task", {});
    auto other = cache::TaskStore::open(directory / "sessions", other_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(other);
    EXPECT_EQ(task.save_segmented(*other, 0, 128).error().code,
              CheckpointErrorCode::BindingMismatch);
    RecursiveSummaryTask wrong(binding(view), task_id, summary(view, "read"));
    EXPECT_EQ(wrong.save_segmented(*store, first->generation, 128).error().code,
              CheckpointErrorCode::BindingMismatch);
    auto old = RecursiveSummaryTask::restore(binding(view), *readonly, *first,
                                             id, QueryKind::Value);
    ASSERT_TRUE(old);
    EXPECT_EQ(*old->checkpoint(), *wire);
    EXPECT_TRUE(task.save_segmented(*store, first->generation, 128));
    auto latest = store->head();
    ASSERT_TRUE(latest);
    ASSERT_TRUE(*latest);
    cache::TaskStoreOptions after_commit;
    after_commit.store.fault = [](cache::IoOperation operation,
                                  const fs::path& path) {
        return operation == cache::IoOperation::ManifestPublished &&
                       path.filename() == "head"
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto ambiguous =
        cache::TaskStore::open(directory / "sessions", task_id,
                               cache::TaskAccess::ReadWrite, after_commit);
    ASSERT_TRUE(ambiguous);
    EXPECT_FALSE(task.offload(*ambiguous, (**latest).generation, 128));
    EXPECT_FALSE(task.state_memory().offloaded);
    EXPECT_EQ(*task.checkpoint(), *wire);
    EXPECT_GT((**store->head()).generation, (**latest).generation);
    auto legacy = cache::TaskStore::open(directory / "v5", task_id,
                                         cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(legacy);
    cache::TaskHead resident;
    {
        auto transaction = legacy->begin(0);
        ASSERT_TRUE(transaction);
        auto object =
            transaction->write(cache::ObjectKind::QueryTaskManifest, *wire);
        ASSERT_TRUE(object);
        auto saved = transaction->commit(*object, cache::TaskStatus::Paused);
        ASSERT_TRUE(saved);
        resident = *saved;
    }
    auto restored_v5 = RecursiveSummaryTask::restore(
        binding(view), *legacy, resident, id, QueryKind::Value);
    ASSERT_TRUE(restored_v5);
    EXPECT_EQ(*restored_v5->checkpoint(), *wire);
    EXPECT_TRUE(restored_v5->save_segmented(*legacy, resident.generation, 128));
}
TEST_F(RecursiveCheckpointTest,
       OffloadedReadFailureCanRetryWithoutLogicalWorkOrStateLoss) {
    auto view = open();
    const auto id = summary(view);
    RecursiveSummaryTask task(binding(view), task_id, id);
    (void)task.advance(80);
    const auto before = task.checkpoint();
    ASSERT_TRUE(before);
    bool fail = false;
    cache::TaskStoreOptions options;
    options.store.fault = [&](cache::IoOperation op, const fs::path&) {
        return fail && op == cache::IoOperation::ReadFile
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto store = cache::TaskStore::open(directory / "retry", task_id,
                                        cache::TaskAccess::ReadWrite, options);
    ASSERT_TRUE(store);
    auto saved = task.offload(*store, 0, 128);
    ASSERT_TRUE(saved);
    const auto steps = task.stats().steps;
    fail = true;
    EXPECT_EQ(task.advance(), QueryProgress::Paused);
    EXPECT_TRUE(task.storage_failure());
    EXPECT_FALSE(task.failure());
    EXPECT_TRUE(task.state_memory().offloaded);
    EXPECT_EQ(task.stats().steps, steps);
    EXPECT_THROW((void)task.result(), std::runtime_error);
    task.cancel();
    EXPECT_EQ(task.advance(), QueryProgress::Cancelled);
    fail = false;
    EXPECT_TRUE(task.checkpoint());
    EXPECT_FALSE(task.storage_failure());
    EXPECT_EQ(task.progress(), QueryProgress::Cancelled);
    task.resume();
    auto restored = RecursiveSummaryTask::restore(binding(view), *store, *saved,
                                                  id, QueryKind::Value);
    ASSERT_TRUE(restored);
    EXPECT_EQ(*restored->checkpoint(), *before);
    finish_task(task);
    finish_task(*restored);
    EXPECT_EQ(task.result(), restored->result());
    same_work(task.stats(), restored->stats());
}
TEST_F(RecursiveCheckpointTest,
       CompletedHeadsWaitForDeliveryAndCancelledHeadsAreReclaimable) {
    auto view = open();
    const auto id = summary(view);
    const auto parent = directory / "maintenance";
    {
        auto store = cache::TaskStore::open(parent, task_id,
                                            cache::TaskAccess::ReadWrite);
        ASSERT_TRUE(store);
        RecursiveSummaryTask task(binding(view), task_id, id);
        finish_task(task);
        const auto saved = task.offload(*store, 0, 128);
        ASSERT_TRUE(saved);
        EXPECT_EQ(saved->status, cache::TaskStatus::AwaitingDelivery);
        store = std::unexpected(
            cache::StoreError{cache::StoreErrorCode::NotFound, {}, {}});
        EXPECT_EQ(
            cache::reclaim_tasks(parent, cache::TaskCleanupPolicy::AllInactive)
                ->active,
            1U);
        task.cancel();
        EXPECT_EQ(task.progress(), QueryProgress::Completed);
        EXPECT_TRUE(task.result().fixed_point);
    }
    EXPECT_EQ(cache::reclaim_tasks(parent)->skipped, 1U);
    EXPECT_EQ(
        cache::reclaim_tasks(parent, cache::TaskCleanupPolicy::AllInactive)
            ->reclaimed,
        1U);
    {
        auto store = cache::TaskStore::open(parent, task_id,
                                            cache::TaskAccess::ReadWrite);
        ASSERT_TRUE(store);
        RecursiveSummaryTask task(binding(view), task_id, id);
        task.cancel();
        auto saved = task.offload(*store, 0, 128);
        ASSERT_TRUE(saved);
        EXPECT_EQ(saved->status, cache::TaskStatus::Cancelled);
    }
    EXPECT_EQ(cache::reclaim_tasks(parent)->reclaimed, 1U);
}
TEST_F(RecursiveCheckpointTest,
       MissingSegmentsCanBeRepairedAndMalformedDirectoriesAreRejected) {
    auto view = open();
    const auto id = summary(view);
    RecursiveSummaryTask task(binding(view), task_id, id);
    auto store = cache::TaskStore::open(directory / "segments", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    const auto before = task.checkpoint();
    ASSERT_TRUE(before);
    auto head = task.offload(*store, 0, 128);
    ASSERT_TRUE(head);
    auto manifest = store->read(head->manifest);
    ASSERT_TRUE(manifest);
    // Initial v6 control is 267 bytes; table zero starts with two counts.
    constexpr std::size_t first_segment = 267 + 8;
    cache::wire::Reader reader{std::span(*manifest).subspan(first_segment), {}};
    EXPECT_EQ(reader.u32(), 0U);
    EXPECT_EQ(reader.u32(), 1U);
    cache::ObjectRef leaf{static_cast<cache::ObjectKind>(reader.u8()), {}, 0};
    const auto digest_bytes = reader.raw(32);
    std::ranges::copy(digest_bytes, leaf.digest.bytes.begin());
    leaf.size = reader.u64();
    const auto hash = leaf.digest.hex();
    const auto path = store->directory() / "objects" / hash.substr(0, 2) / hash;
    const auto missing = fs::path(path.string() + ".missing");
    fs::rename(path, missing);
    EXPECT_EQ(task.advance(0), QueryProgress::Paused);
    ASSERT_TRUE(task.storage_failure());
    ASSERT_TRUE(task.storage_failure()->storage);
    EXPECT_EQ(task.storage_failure()->storage->code,
              cache::StoreErrorCode::MissingObject);
    EXPECT_TRUE(task.state_memory().offloaded);
    EXPECT_EQ(task.stats().steps, 0U);
    fs::rename(missing, path);
    EXPECT_EQ(*task.checkpoint(), *before);
    EXPECT_FALSE(task.storage_failure());
    {
        auto transaction = store->begin(head->generation);
        ASSERT_TRUE(transaction);
        auto corrupt = *manifest;
        corrupt[first_segment] = 1;
        resign(corrupt);
        auto object = transaction->write(cache::ObjectKind::QueryTaskManifest,
                                         std::move(corrupt));
        ASSERT_TRUE(object);
        auto bad = transaction->commit(*object, head->status);
        ASSERT_TRUE(bad);
        auto result = RecursiveSummaryTask::restore(binding(view), *store, *bad,
                                                    id, QueryKind::Value);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, CheckpointErrorCode::Corrupt);
    }
    auto old = RecursiveSummaryTask::restore(binding(view), *store, *head, id,
                                             QueryKind::Value);
    ASSERT_TRUE(old);
    EXPECT_EQ(*old->checkpoint(), *before);
    {
        auto current = store->head();
        ASSERT_TRUE(current);
        ASSERT_TRUE(*current);
        auto transaction = store->begin((**current).generation);
        ASSERT_TRUE(transaction);
        auto bytes = store->read(leaf);
        ASSERT_TRUE(bytes);
        (*bytes)[45] =
            8; // A valid CAS payload with the wrong logical table tag.
        auto wrong = transaction->write(cache::ObjectKind::QueryStateSegment,
                                        std::move(*bytes));
        ASSERT_TRUE(wrong);
        auto corrupt = *manifest;
        std::ranges::copy(wrong->digest.bytes,
                          corrupt.begin() + first_segment + 9);
        resign(corrupt);
        auto object = transaction->write(cache::ObjectKind::QueryTaskManifest,
                                         std::move(corrupt));
        ASSERT_TRUE(object);
        auto bad = transaction->commit(*object, head->status);
        ASSERT_TRUE(bad);
        auto result = RecursiveSummaryTask::restore(binding(view), *store, *bad,
                                                    id, QueryKind::Value);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, CheckpointErrorCode::Corrupt);
    }
}
TEST_F(RecursiveCheckpointTest,
       SegmentedStateExceedsSingleObjectLimitAndReportsActualRelease) {
    std::string text = "fn wide(x: u32) -> u32 { let n0 = x + 1; ";
    for (unsigned i = 1; i < 512; ++i)
        text += "let n" + std::to_string(i) + " = n" + std::to_string(i - 1) +
                " + 1; ";
    text += "n511 } let aa = wide(external_input());";
    cache_test::Input input(text);
    auto identity = input.identity();
    ASSERT_TRUE(identity);
    StageAnalyzer analyzer(input.diagnostics, input.checker.types(), {},
                           cache_test::context());
    auto module = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto evidence_dir = directory / "wide-evidence";
    ASSERT_TRUE(FunctionEvidence::publish(cache::FileStore(evidence_dir),
                                          *module->provenance, *identity,
                                          cache_test::context()));
    auto closure = FunctionEvidence::load(cache::FileStore(evidence_dir),
                                          *identity, cache_test::context());
    ASSERT_TRUE(closure);
    auto view = FunctionEvidenceView::open(std::move(*closure));
    ASSERT_TRUE(view);
    auto binding = view->checkpoint_binding(text);
    ASSERT_TRUE(binding);
    ASSERT_EQ(view->summary_catalog().size(), 1U);
    RecursiveSummaryTask task(*binding, task_id, view->summary_catalog()[0].id);
    finish_task(task);
    const auto before = task.checkpoint();
    ASSERT_TRUE(before);
    cache::Limits limits{.bytes = 16 * 1024};
    ASSERT_GT(before->size(), limits.bytes);
    EXPECT_FALSE(task.checkpoint(limits));
    const auto resident = payload(task.state_memory());
    EXPECT_GT(resident, before->size());
    auto store = cache::TaskStore::open(directory / "wide-task", task_id,
                                        cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    auto head = task.offload(*store, 0, 2048, limits);
    ASSERT_TRUE(head);
    const auto written = task.storage_stats();
    EXPECT_EQ(payload(task.state_memory()), 0U);
    EXPECT_GT(written.logical_bytes, limits.bytes);
    EXPECT_LT(written.peak_codec_buffers, before->size());
    EXPECT_LE(written.peak_codec_buffers, 2 * limits.bytes);
    EXPECT_EQ(*task.checkpoint(), *before);
    const auto read = task.storage_stats();
    EXPECT_EQ(read.segment_reads, written.segment_writes);
    EXPECT_GT(read.validation_payload_bytes, 0U);
    EXPECT_GT(read.directory_bytes, 0U);
    std::cout << "recursive-storage rows=" << task.stats().nodes
              << " resident-payload=" << resident
              << " v5-bytes=" << before->size()
              << " logical-bytes=" << written.logical_bytes
              << " submitted=" << written.bytes_submitted
              << " cas-added=" << written.cas_bytes_added
              << " segments=" << written.segment_writes << " codec-peak="
              << std::max(written.peak_codec_buffers, read.peak_codec_buffers)
              << " directory=" << read.directory_bytes
              << " validation-payload=" << read.validation_payload_bytes
              << '\n';
}

TEST_F(RecursiveCheckpointTest,
       FinalReceiptPersistsMonotonicallyAndEnablesCleanup) {
    auto view = open();
    const auto id = summary(view);
    const auto parent = directory / "receipts";
    {
        auto store = cache::TaskStore::open(parent, task_id,
                                            cache::TaskAccess::ReadWrite);
        ASSERT_TRUE(store);
        RecursiveSummaryTask task(binding(view), task_id, id);
        EXPECT_EQ(task.pending_sequence(), 0U);
        EXPECT_FALSE(task.acknowledge_result(1));
        finish_task(task);
        const auto expected = task.result();
        const auto work = task.stats();
        EXPECT_EQ(task.pending_sequence(), 1U);
        EXPECT_FALSE(task.acknowledge_result(0));
        EXPECT_FALSE(task.acknowledge_result(2));
        auto pending = task.offload(*store, 0, 128);
        ASSERT_TRUE(pending);
        EXPECT_EQ(pending->status, cache::TaskStatus::AwaitingDelivery);
        EXPECT_EQ(task.pending_sequence(), 1U); // Does not load the task.
        EXPECT_TRUE(task.state_memory().offloaded);
        auto stale = RecursiveSummaryTask::restore(
            binding(view), *store, *pending, id, QueryKind::Value);
        ASSERT_TRUE(stale);
        EXPECT_TRUE(task.acknowledge_result(1)); // Loads before updating.
        EXPECT_EQ(task.pending_sequence(), 0U);
        EXPECT_EQ(task.acknowledged_sequence(), 1U);
        EXPECT_TRUE(task.acknowledge_result(1)); // Retry is idempotent.
        EXPECT_FALSE(task.checkpoint()); // v5 cannot erase the watermark.
        auto acknowledged = task.offload(*store, pending->generation, 128);
        ASSERT_TRUE(acknowledged);
        EXPECT_EQ(acknowledged->status, cache::TaskStatus::Completed);
        EXPECT_EQ(task.acknowledged_sequence(), 1U);
        EXPECT_TRUE(task.state_memory().offloaded);
        auto restored = RecursiveSummaryTask::restore(
            binding(view), *store, *acknowledged, id, QueryKind::Value);
        ASSERT_TRUE(restored);
        EXPECT_EQ(restored->pending_sequence(), 0U);
        EXPECT_EQ(restored->acknowledged_sequence(), 1U);
        EXPECT_EQ(restored->result(), expected);
        same_work(restored->stats(), work);
        EXPECT_EQ(restored->stats().slices, work.slices);
        EXPECT_EQ(restored->advance(1000), QueryProgress::Completed);
        EXPECT_EQ(restored->stats().slices, work.slices);
        restored->cancel();
        restored->resume();
        EXPECT_EQ(restored->pending_sequence(), 0U);
        const auto rollback =
            stale->save_segmented(*store, acknowledged->generation, 128);
        ASSERT_FALSE(rollback);
        EXPECT_EQ(rollback.error().code, CheckpointErrorCode::InvalidState);
        EXPECT_EQ(**store->head(), *acknowledged);
        // Immutable older heads can still replay their unconfirmed result.
        auto replay = RecursiveSummaryTask::restore(
            binding(view), *store, *pending, id, QueryKind::Value);
        ASSERT_TRUE(replay);
        EXPECT_EQ(replay->pending_sequence(), 1U);
        EXPECT_EQ(replay->result(), expected);
    }
    EXPECT_EQ(cache::reclaim_tasks(parent)->reclaimed, 1U);
}
TEST_F(RecursiveCheckpointTest,
       ReceiptPublicationFailuresAndMalformedWatermarks) {
    auto view = open();
    const auto id = summary(view);
    const auto parent = directory / "receipt-faults";
    auto store =
        cache::TaskStore::open(parent, task_id, cache::TaskAccess::ReadWrite);
    ASSERT_TRUE(store);
    RecursiveSummaryTask task(binding(view), task_id, id);
    finish_task(task);
    auto pending = task.save_segmented(*store, 0, 128);
    ASSERT_TRUE(pending);
    ASSERT_TRUE(task.acknowledge_result(1));
    cache::TaskStoreOptions quota;
    quota.disk_bytes = 256;
    auto small = cache::TaskStore::open(parent, task_id,
                                        cache::TaskAccess::ReadWrite, quota);
    ASSERT_TRUE(small);
    EXPECT_FALSE(task.offload(*small, pending->generation, 128));
    EXPECT_FALSE(task.state_memory().offloaded);
    EXPECT_EQ(**store->head(), *pending);
    auto replay = RecursiveSummaryTask::restore(binding(view), *store, *pending,
                                                id, QueryKind::Value);
    ASSERT_TRUE(replay);
    EXPECT_EQ(replay->pending_sequence(), 1U);
    cache::TaskStoreOptions after_commit;
    after_commit.store.fault = [](cache::IoOperation operation,
                                  const fs::path& path) {
        return operation == cache::IoOperation::ManifestPublished &&
                       path.filename() == "head"
                   ? std::make_error_code(std::errc::io_error)
                   : std::error_code{};
    };
    auto ambiguous = cache::TaskStore::open(
        parent, task_id, cache::TaskAccess::ReadWrite, after_commit);
    ASSERT_TRUE(ambiguous);
    EXPECT_FALSE(task.offload(*ambiguous, pending->generation, 128));
    EXPECT_FALSE(task.state_memory().offloaded);
    auto head = **store->head();
    EXPECT_EQ(head.status, cache::TaskStatus::Completed);
    auto recovered = RecursiveSummaryTask::restore(binding(view), *store, head,
                                                   id, QueryKind::Value);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->acknowledged_sequence(), 1U);
    auto bytes = store->read(head.manifest);
    ASSERT_TRUE(bytes);
    ASSERT_EQ((*bytes)[9], 7U);
    // Done-phase control has no active transfer; watermark follows 267 bytes.
    for (const auto value : {0U, 2U}) {
        auto corrupt = *bytes;
        corrupt[267] = static_cast<std::uint8_t>(value);
        resign(corrupt);
        auto writer = store->begin(head.generation);
        ASSERT_TRUE(writer);
        auto object = writer->write(cache::ObjectKind::QueryTaskManifest,
                                    std::move(corrupt));
        ASSERT_TRUE(object);
        auto invalid = head;
        invalid.manifest = *object;
        EXPECT_FALSE(RecursiveSummaryTask::restore(
            binding(view), *store, invalid, id, QueryKind::Value));
    }
    auto mismatched = head;
    mismatched.status = cache::TaskStatus::AwaitingDelivery;
    EXPECT_FALSE(RecursiveSummaryTask::restore(
        binding(view), *store, mismatched, id, QueryKind::Value));
}
} // namespace
