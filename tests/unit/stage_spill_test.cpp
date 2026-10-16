#include "cache_test_input.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/function_evidence.h"
#include "pagos/stage/provenance_snapshot.h"
#include "pagos/stage/stage_analyzer.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <tuple>

namespace {
using namespace pagos;
using namespace pagos::stage;
using namespace pagos::stage::provenance;
namespace fs = std::filesystem;
struct Run {
    std::unique_ptr<hir::Module> module;
    AnalysisStats stats;
    CacheTelemetry cache;
    std::string diagnostics, hir;
};
Run analyze(const std::string& text, EvidenceBuildConfig spill = {},
            CacheConfig cache = {}, AnalysisLimits limits = {}) {
    cache_test::Input input(text);
    StageAnalyzer analyzer(input.diagnostics, input.checker.types(), limits,
                           cache_test::context(), cache, spill);
    auto module = analyzer.analyze(*input.module);
    std::ostringstream diagnostics, hir;
    input.diagnostics.render(diagnostics);
    hir::print(*module, hir);
    return {std::move(module), analyzer.stats(), analyzer.cache_telemetry(),
            diagnostics.str(), hir.str()};
}
auto logical(const AnalysisStats& s) {
    return std::tuple{s.fuel_consumed,
                      s.specializations,
                      s.cache_hits,
                      s.maximum_recursion_depth,
                      s.array_elements_reserved,
                      s.array_bytes_reserved,
                      s.aggregate_constructions,
                      s.aggregate_members_reserved,
                      s.aggregate_bytes_reserved,
                      s.residual_specializations,
                      s.residual_cache_hits};
}
QueryResult solve(const ProvenanceAnalysis& analysis, NodeId root,
                  QueryKind channel, bool witness) {
    QueryTask task(analysis.evidence(), root, channel, witness);
    while (task.progress() != QueryProgress::Completed &&
           task.progress() != QueryProgress::Failed)
        (void)task.advance({1, 1, 1, 1});
    EXPECT_EQ(task.progress(), QueryProgress::Completed);
    return task.result();
}
std::string large_function(unsigned count = 256) {
    std::string text = "fn big(x: u32) -> u32 { let n0 = x + 1; ";
    for (unsigned i = 1; i < count; ++i)
        text += "let n" + std::to_string(i) + " = n" + std::to_string(i - 1) +
                " + 1; ";
    return text + "n" + std::to_string(count - 1) +
           " } let aa = big(external_input()); let bb = big(external_input());";
}
class StageSpillTest : public ::testing::Test {
  protected:
    StageSpillTest() {
        static std::atomic<unsigned> serial{};
        directory =
            fs::temp_directory_path() /
            ("pagos-stage-spill-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(serial++));
    }
    ~StageSpillTest() override {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }
    EvidenceBuildConfig config() const {
        EvidenceBuildConfig c;
        c.mode = EvidenceBuildMode::Spill;
        c.directory = directory / "spill";
        c.block_bytes = 256;
        c.resident_bytes = 8192;
        c.cache_bytes = 1024;
        return c;
    }
    CacheConfig cache() const {
        return {CacheMode::ReadWrite, directory / "semantic"};
    }
    fs::path directory;
};
TEST_F(StageSpillTest, CensusTracksMetadataAndPrunesExpiredFoldedHir) {
    const std::string text =
        "let x = external_input(); let fixed = [7, x]; comptime let selected = "
        "fixed[0]; let values = [for i in 0..2048 { comptime let k "
        "= if i == 0 { 7 } else { 9 }; k }];";
    auto measured = config();
    measured.measure_memory = true;
    auto run = analyze(text, measured);
    ASSERT_TRUE(run.diagnostics.empty());
    const auto stats = run.module->provenance->memory_stats();
    EXPECT_TRUE(stats.recorder_measured);
    EXPECT_GT(stats.pruned_memo_entries, 0U);
    EXPECT_GT(stats.memo_peak_entries, stats.memo_entries);
    EXPECT_GT(stats.sites_bytes, 0U);
    EXPECT_GT(stats.roots_bytes, 0U);
    EXPECT_GT(stats.decisions_bytes, 0U);
    EXPECT_GT(stats.scopes_bytes, 0U);
    EXPECT_GT(stats.recorder_maps_bytes, 0U);
    EXPECT_GT(stats.definition_identity_bytes, 0U);
    EXPECT_GT(stats.view_directory_bytes, 0U);
    EXPECT_EQ(stats.graph_bytes, 0U);
    auto memory = analyze(text);
    EXPECT_EQ(logical(run.stats), logical(memory.stats));
    EXPECT_EQ(run.hir, memory.hir);
    // All old contracts survive weak-map retirement; compare frozen bytes,
    // including every local decision/root and concrete dependency edge.
    auto input = cache_test::Input(text);
    auto identity = input.identity();
    ASSERT_TRUE(identity);
    const auto a =
        ProvenanceSnapshot::encode(*run.module->provenance, *identity,
                                   cache_test::context(), input.source);
    const auto b =
        ProvenanceSnapshot::encode(*memory.module->provenance, *identity,
                                   cache_test::context(), input.source);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->graph, b->graph);
    EXPECT_EQ(a->positions, b->positions);
}
TEST_F(StageSpillTest, AnalyzerReleasesWorkingTablesAndCanAnalyzeAgain) {
    cache_test::Input input(
        "fn table(n: u32) -> [u32; 4] { [for i in 0..4 { i + n }] } fn f(x: "
        "u32) -> u32 { if x == 0 { 7 } else { x + 1 } } let a = table(3); let "
        "b = table(4); let aa = f(external_input());");
    auto options = config();
    options.measure_memory = true;
    StageAnalyzer analyzer(input.diagnostics, input.checker.types(), {},
                           cache_test::context(), cache(), options);
    auto first = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto before = analyzer.memory_stats();
    EXPECT_TRUE(before.measured);
    EXPECT_GT(before.before_release.tables_bytes, 0U);
    EXPECT_GT(before.before_release.arguments_bytes, 0U);
    EXPECT_GT(before.before_release.cache_bytes, 0U);
    EXPECT_GT(before.before_release.entries, 0U);
    EXPECT_EQ(before.after_release.tables_bytes, 0U);
    EXPECT_EQ(before.after_release.arguments_bytes, 0U);
    EXPECT_EQ(before.after_release.cache_bytes, 0U);
    EXPECT_EQ(before.after_release.entries, 0U);
    EXPECT_GT(first->provenance->memory_stats().specialization_bytes, 0U);
    const auto root = first->provenance->roots().back().node;
    const auto source = solve(*first->provenance, root, QueryKind::Value, true);
    auto second = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(first->bindings.size(), second->bindings.size());
    EXPECT_EQ(solve(*first->provenance, root, QueryKind::Value, true).sources,
              source.sources);
    EXPECT_EQ(analyzer.memory_stats().after_release.entries, 0U);
}

TEST_F(StageSpillTest,
       RealStageQueriesMatchMemoryForCallsContractsProjectionsAndRecursion) {
    for (const auto& text :
         {large_function(),
          std::string(
              "fn inner(x: u32) -> u32 { let a = [7, x]; comptime let k = "
              "a[0]; k + x } fn outer(x: u32) -> u32 { inner(x) } let aa = "
              "outer(external_input()); let bb = outer(external_input());"),
          std::string("record R { a: u32, b: u32 } let x = external_input(); "
                      "let r = R(a: 7, b: x); comptime let a = r.a; let b = "
                      "r.b; runtime let opaque = r; let c = opaque.a; let arr "
                      "= [7, x]; let indexed = arr[x];"),
          std::string("fn even(x: u32) -> u32 { if x == 0 { 1 } else { odd(x - "
                      "1) } } fn odd(x: u32) -> u32 { if x == 0 { 0 } else { "
                      "even(x - 1) } } let aa = even(external_input());"),
          std::string(
              "fn require(x: u32) -> u32 { comptime let local = if x == 0 { 7 "
              "} else { 7 }; local } let a = require(external_input());")}) {
        auto memory = analyze(text);
        auto spill = analyze(text, config());
        EXPECT_EQ(spill.hir, memory.hir);
        EXPECT_EQ(spill.diagnostics, memory.diagnostics);
        EXPECT_EQ(logical(spill.stats), logical(memory.stats));
        ASSERT_TRUE(spill.module->provenance->available());
        EXPECT_FALSE(spill.module->provenance->graph_materialized());
        const auto& a = *spill.module->provenance;
        const auto& b = *memory.module->provenance;
        ASSERT_EQ(a.roots().size(), b.roots().size());
        ASSERT_EQ(a.decisions().size(), b.decisions().size());
        for (std::size_t i = 0; i < a.decisions().size(); ++i) {
            const auto& x = a.decisions()[i];
            const auto& y = b.decisions()[i];
            EXPECT_EQ(std::tuple(x.node.value, x.rule, x.result, x.reason,
                                 x.member, x.scope.value, x.instantiated),
                      std::tuple(y.node.value, y.rule, y.result, y.reason,
                                 y.member, y.scope.value, y.instantiated));
        }
        for (std::size_t i = 0; i < a.roots().size(); ++i) {
            EXPECT_EQ(a.roots()[i].node, b.roots()[i].node);
            for (auto channel : {QueryKind::Value, QueryKind::Effects})
                for (bool witness : {false, true}) {
                    const auto actual =
                        solve(a, a.roots()[i].node, channel, witness);
                    const auto expected =
                        solve(b, b.roots()[i].node, channel, witness);
                    EXPECT_EQ(actual.sources, expected.sources);
                    EXPECT_EQ(actual.witness, expected.witness);
                    EXPECT_EQ(actual.invocations, expected.invocations);
                    EXPECT_EQ(actual.complete, expected.complete);
                    EXPECT_EQ(actual.recursive_call_cut,
                              expected.recursive_call_cut);
                }
        }
        EXPECT_FALSE(a.graph_materialized());
        if (memory.diagnostics.empty()) {
            EXPECT_TRUE(mir::lower(*spill.module));
        }
    }
}
TEST_F(StageSpillTest,
       ActualLargeFunctionSealsAndReleasesWithoutWholeGraphRestoration) {
    auto run = analyze(large_function(512), config());
    ASSERT_TRUE(run.diagnostics.empty());
    const auto& analysis = *run.module->provenance;
    ASSERT_TRUE(analysis.available());
    const auto& stats = analysis.construction_stats();
    EXPECT_GT(stats.nodes, 1500U);
    EXPECT_GT(stats.written_blocks, 100U);
    EXPECT_GT(stats.released_active_bytes, 50000U);
    EXPECT_EQ(stats.active_bytes, 0U);
    EXPECT_EQ(stats.resident_bytes, 0U);
    EXPECT_LE(stats.peak_owned_bytes, config().resident_bytes);
    EXPECT_FALSE(analysis.graph_materialized());
    EXPECT_EQ(analysis.evidence()->read_stats().objects_read, 0U);
    const auto root = analysis.roots().back().node;
    EXPECT_FALSE(
        solve(analysis, root, QueryKind::Value, false).sources.empty());
    EXPECT_FALSE(analysis.graph_materialized());
}
TEST_F(StageSpillTest,
       CompatibilityGraphMaterializesOnceAndKeepsReferencesStable) {
    auto run = analyze(large_function(32), config());
    const auto analysis = run.module->provenance;
    EXPECT_FALSE(analysis->graph_materialized());
    const FrozenGraph* first = nullptr;
    std::array<const FrozenGraph*, 4> results{};
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < results.size(); ++i)
        threads.emplace_back([&, i] { results[i] = &analysis->graph(); });
    for (auto& thread : threads)
        thread.join();
    first = results[0];
    for (auto* graph : results)
        EXPECT_EQ(graph, first);
    EXPECT_TRUE(analysis->graph_materialized());
    const auto& node = first->node(analysis->roots().back().node);
    analysis->evidence()->evict();
    EXPECT_EQ(&node, &analysis->graph().node(analysis->roots().back().node));
    fs::remove_all(config().directory);
    EXPECT_FALSE(analysis->validate_details());
    EXPECT_EQ(first, &analysis->graph());
    EXPECT_EQ(&node, &analysis->graph().node(analysis->roots().back().node));
}
TEST_F(StageSpillTest,
       AnalysisAndPausedTaskRetainTemporaryStorageAfterAnalyzerAndHirDie) {
    const auto text = large_function(32);
    auto run = analyze(text, config());
    auto analysis = run.module->provenance;
    auto task = std::make_unique<ProvenanceQueryTask>(
        analysis, source::SourceFile::from_text("old.pgs", text),
        analysis->roots().back().node);
    (void)task->query().advance({1, 1, 1, 1});
    run.module.reset();
    analysis.reset();
    EXPECT_FALSE(fs::is_empty(config().directory));
    while (task->query().progress() != QueryProgress::Completed)
        ASSERT_NE(task->query().advance({1, 1, 1, 1}).progress,
                  QueryProgress::Failed);
    task.reset();
    EXPECT_TRUE(fs::is_empty(config().directory));
}
TEST_F(StageSpillTest, WriteFailureFallbackPreservesStageAndQueryOutput) {
    auto c = config();
    c.store.fault = [](auto operation, const auto&) {
        return operation == cache::IoOperation::WriteTemporary
                   ? std::make_error_code(std::errc::no_space_on_device)
                   : std::error_code{};
    };
    const auto text = large_function(32);
    auto memory = analyze(text);
    auto fallback = analyze(text, c);
    EXPECT_EQ(fallback.hir, memory.hir);
    EXPECT_EQ(fallback.diagnostics, memory.diagnostics);
    EXPECT_TRUE(fallback.module->provenance->available());
    EXPECT_GT(fallback.module->provenance->construction_stats().fallback_blocks,
              0U);
    std::ostringstream a, b;
    explain_provenance(*memory.module->provenance, a);
    explain_provenance(*fallback.module->provenance, b);
    EXPECT_EQ(a.str(), b.str());
    EXPECT_FALSE(fallback.module->provenance->graph_materialized());
}
TEST_F(StageSpillTest,
       CapacityAndIoFailuresDoNotChangeStageDiagnosticsBudgetsOrResiduals) {
    for (bool fail_io : {false, true}) {
        auto c = config();
        c.failure_policy = SpillFailurePolicy::Unavailable;
        if (fail_io)
            c.store.fault = [](auto op, const auto&) {
                return op == cache::IoOperation::ObjectPublished
                           ? std::make_error_code(std::errc::io_error)
                           : std::error_code{};
            };
        else
            c.resident_bytes = 0;
        for (const auto& text :
             {large_function(32),
              std::string("let x = external_input(); comptime let required = x "
                          "+ 1;")}) {
            auto memory = analyze(text);
            auto failed = analyze(text, c);
            EXPECT_EQ(failed.hir, memory.hir);
            EXPECT_EQ(failed.diagnostics, memory.diagnostics);
            EXPECT_EQ(logical(failed.stats), logical(memory.stats));
            EXPECT_FALSE(failed.module->provenance->available());
            EXPECT_THROW((void)failed.module->provenance->graph(),
                         EvidenceUnavailable);
            QueryTask task(failed.module->provenance->evidence(), {1});
            EXPECT_EQ(task.advance().progress, QueryProgress::Failed);
            EXPECT_EQ(task.advance().details, DetailAvailability::Unavailable);
            if (memory.diagnostics.empty()) {
                EXPECT_TRUE(mir::lower(*failed.module));
            }
        }
    }
    AnalysisLimits limits;
    limits.fuel = 3;
    auto memory = analyze(large_function(16), {}, {}, limits);
    auto c = config();
    c.resident_bytes = 0;
    auto failed = analyze(large_function(16), c, {}, limits);
    EXPECT_EQ(failed.diagnostics, memory.diagnostics);
    EXPECT_EQ(logical(failed.stats), logical(memory.stats));
}
TEST_F(StageSpillTest,
       LateFailureAndFailedAnalysisBlockEarlierScalarAndResidualPublications) {
    for (const auto& prefix :
         {std::string("fn square(x: u32) -> u32 { x * x } comptime let n = "
                      "square(7); "),
          std::string("fn plus(x: u32) -> u32 { x + 1 } let v = "
                      "plus(external_input()); ")}) {
        fs::remove_all(directory);
        auto c = config();
        c.disk_bytes = 100;
        c.failure_policy = SpillFailurePolicy::Unavailable;
        auto failed = analyze(prefix + large_function(32), c, cache());
        EXPECT_FALSE(failed.module->provenance->available());
        EXPECT_EQ(failed.cache.writes, 0U);
        EXPECT_GT(failed.cache.blocked_writes, 0U);
        EXPECT_FALSE(fs::exists(cache().directory));
        EXPECT_GT(failed.cache.pending_payload_bytes_peak, 0U);
    }
    fs::remove_all(directory);
    auto failed = analyze("fn square(x: u32) -> u32 { x * x } comptime let n = "
                          "square(7); comptime let bad = external_input();",
                          config(), cache());
    EXPECT_FALSE(failed.diagnostics.empty());
    EXPECT_EQ(failed.cache.writes, 0U);
    EXPECT_GT(failed.cache.blocked_writes, 0U);
    EXPECT_FALSE(fs::exists(cache().directory));
}
TEST_F(StageSpillTest,
       SuccessfulSpillPublishesAndReadOnlyAndOffKeepSemanticIoContract) {
    const std::string text =
        "fn square(x: u32) -> u32 { x * x } comptime let n = square(7); fn "
        "plus(x: u32) -> u32 { x + 1 } let a = plus(external_input());";
    auto cold = analyze(text, config(), cache());
    ASSERT_TRUE(cold.module->provenance->available());
    EXPECT_GT(cold.cache.writes, 0U);
    EXPECT_GT(cold.cache.residual_writes, 0U);
    EXPECT_FALSE(cold.module->provenance->graph_materialized());
    auto readonly = cache();
    readonly.mode = CacheMode::ReadOnly;
    auto hot = analyze(text, config(), readonly);
    EXPECT_EQ(hot.hir, cold.hir);
    EXPECT_GT(hot.cache.hits, 0U);
    EXPECT_EQ(hot.cache.writes, 0U);
    const auto absent = directory / "absent";
    readonly.directory = absent;
    auto miss = analyze(text, config(), readonly);
    EXPECT_EQ(miss.cache.writes, 0U);
    EXPECT_FALSE(fs::exists(absent));
    auto off = readonly;
    off.mode = CacheMode::Off;
    auto disabled = analyze(text, config(), off);
    EXPECT_EQ(disabled.cache.lookups, 0U);
    EXPECT_FALSE(fs::exists(absent));
}
TEST_F(StageSpillTest,
       MissingFrozenDetailsFailQueriesAndRejectPersistentEvidenceExports) {
    const auto text = large_function(32);
    auto run = analyze(text, config());
    const auto analysis = run.module->provenance;
    fs::remove_all(config().directory);
    analysis->evidence()->evict();
    QueryTask task(analysis->evidence(), analysis->roots().back().node);
    EXPECT_EQ(task.advance().progress, QueryProgress::Failed);
    ASSERT_TRUE(task.failure());
    EXPECT_EQ(task.failure()->code, EvidenceErrorCode::MissingBlock);
    EXPECT_FALSE(analysis->validate_details());
    EXPECT_THROW((void)analysis->graph(), EvidenceUnavailable);
    cache_test::Input current(text);
    const auto identity = current.identity();
    ASSERT_TRUE(identity);
    EXPECT_FALSE(
        FunctionEvidence::publish(cache::FileStore(directory / "export"),
                                  *analysis, *identity, cache_test::context()));
    EXPECT_FALSE(ProvenanceSnapshot::encode(
        *analysis, *identity, cache_test::context(), current.source));
    EXPECT_FALSE(fs::exists(directory / "export"));
}
TEST_F(
    StageSpillTest,
    SpillRenderingUsesNativeSourcesAndTopologyButExplicitCompressionRestores) {
    auto run = analyze(large_function(16), config());
    for (auto mode : {ProvenanceQueryOptions::Mode::Legacy,
                      ProvenanceQueryOptions::Mode::Resumable,
                      ProvenanceQueryOptions::Mode::Topology}) {
        std::ostringstream output, stats;
        explain_provenance_experimental(*run.module->provenance, output, stats,
                                        {mode, 1, true});
        EXPECT_FALSE(output.str().empty());
        EXPECT_FALSE(run.module->provenance->graph_materialized());
    }
    std::ostringstream output, stats;
    explain_provenance_experimental(
        *run.module->provenance, output, stats,
        {ProvenanceQueryOptions::Mode::Compressed, 1, true});
    EXPECT_TRUE(run.module->provenance->graph_materialized());
}
TEST_F(StageSpillTest,
       OldBlockReadFailuresDoNotReplaceStageContractDiagnostics) {
    for (const auto& text :
         {std::string("fn f(x: u32) -> u32 { let a = [7, x]; comptime let k = "
                      "a[0]; k + x } let y = f(external_input());"),
          std::string("fn f(x: u32) -> u32 { comptime let k = x + 1; k } let y "
                      "= f(external_input());")}) {
        auto c = config();
        c.block_bytes = 24;
        c.cache_bytes = 0;
        fs::path latest;
        c.store.fault = [&](auto operation, const fs::path& path) {
            if (operation == cache::IoOperation::ObjectPublished)
                latest = path;
            if (operation == cache::IoOperation::ReadFile && path != latest &&
                fs::exists(path))
                return std::make_error_code(std::errc::permission_denied);
            return std::error_code{};
        };
        auto memory = analyze(text);
        auto failed = analyze(text, c);
        EXPECT_EQ(failed.diagnostics, memory.diagnostics);
        EXPECT_EQ(failed.hir, memory.hir);
        EXPECT_EQ(logical(failed.stats), logical(memory.stats));
        ASSERT_TRUE(failed.module->provenance->failure());
        EXPECT_EQ(failed.module->provenance->failure()->code,
                  EvidenceErrorCode::IoFailure);
    }
}
TEST_F(StageSpillTest, ExplicitSnapshotAndFunctionExportsMatchMemoryEncoding) {
    const std::string text =
        "fn scale(k: u32, x: u32) -> u32 { k * x } let a = scale(4, "
        "external_input()); let b = scale(5, external_input());";
    auto memory = analyze(text), spill = analyze(text, config());
    cache_test::Input input(text);
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    auto a = ProvenanceSnapshot::encode(*memory.module->provenance, *identity,
                                        cache_test::context(), input.source);
    auto b = ProvenanceSnapshot::encode(*spill.module->provenance, *identity,
                                        cache_test::context(), input.source);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->graph, b->graph);
    EXPECT_EQ(a->positions, b->positions);
    EXPECT_TRUE(spill.module->provenance->graph_materialized());
    for (auto [name, analysis] :
         {std::pair{"memory", memory.module->provenance},
          std::pair{"disk", spill.module->provenance}})
        EXPECT_TRUE(FunctionEvidence::publish(
            cache::FileStore(directory / name), *analysis, *identity,
            cache_test::context()));
    for (const auto& file :
         fs::recursive_directory_iterator(directory / "memory" / "objects")) {
        if (!file.is_regular_file())
            continue;
        const auto relative = fs::relative(file.path(), directory / "memory");
        const auto other = directory / "disk" / relative;
        ASSERT_TRUE(fs::exists(other));
        std::ifstream first(file.path(), std::ios::binary),
            second(other, std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(first), {}),
                  std::string(std::istreambuf_iterator<char>(second), {}));
    }
}
} // namespace
