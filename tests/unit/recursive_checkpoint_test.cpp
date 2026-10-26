#include "../../lib/cache/wire.h"
#include "cache_test_input.h"
#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/provenance_recursive.h"
#include "pagos/stage/stage_analyzer.h"

#include <atomic>
#include <fstream>
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
} // namespace
