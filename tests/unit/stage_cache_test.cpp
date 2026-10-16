#include "cache_test_input.h"
#include "pagos/cache/store.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/provenance_analysis.h"
#include "pagos/stage/provenance_snapshot.h"
#include "pagos/stage/stage_analyzer.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {
using namespace pagos;
namespace fs = std::filesystem;

struct Outcome {
    stage::AnalysisStats stats;
    stage::CacheTelemetry telemetry;
    std::string hir;
    std::string provenance;
    std::string diagnostics;
    std::unique_ptr<hir::Module> module;
};

class StageCache : public ::testing::Test {
  protected:
    StageCache() {
        static std::atomic<unsigned> sequence{};
        root =
            fs::temp_directory_path() /
            ("pagos-stage-cache-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    }
    ~StageCache() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    Outcome run(std::string text, stage::CacheMode mode,
                stage::AnalysisLimits limits = {},
                cache::QueryContext context = cache_test::context()) {
        cache_test::Input input(std::move(text), context.target.pointer_bits);
        stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types(),
                                      limits, context, {mode, root});
        auto module = analyzer.analyze(*input.module);
        std::ostringstream hir, provenance, diagnostics;
        hir::print(*module, hir);
        stage::explain_provenance(*module->provenance, provenance);
        input.diagnostics.render(diagnostics);
        return {analyzer.stats(),  analyzer.cache_telemetry(),
                hir.str(),         provenance.str(),
                diagnostics.str(), std::move(module)};
    }
    void equivalent(const Outcome& a, const Outcome& b) {
        EXPECT_EQ(a.hir, b.hir);
        EXPECT_EQ(a.provenance, b.provenance);
        EXPECT_EQ(a.diagnostics, b.diagnostics);
        EXPECT_EQ(a.stats.fuel_consumed, b.stats.fuel_consumed);
        EXPECT_EQ(a.stats.specializations, b.stats.specializations);
        EXPECT_EQ(a.stats.cache_hits, b.stats.cache_hits);
        EXPECT_EQ(a.stats.maximum_recursion_depth,
                  b.stats.maximum_recursion_depth);
        EXPECT_EQ(a.stats.array_elements_reserved,
                  b.stats.array_elements_reserved);
        EXPECT_EQ(a.stats.array_bytes_reserved, b.stats.array_bytes_reserved);
        EXPECT_EQ(a.stats.aggregate_constructions,
                  b.stats.aggregate_constructions);
        EXPECT_EQ(a.stats.aggregate_members_reserved,
                  b.stats.aggregate_members_reserved);
        EXPECT_EQ(a.stats.aggregate_bytes_reserved,
                  b.stats.aggregate_bytes_reserved);
        EXPECT_EQ(a.stats.residual_specializations,
                  b.stats.residual_specializations);
        EXPECT_EQ(a.stats.residual_cache_hits, b.stats.residual_cache_hits);
    }
    fs::path root;
    const std::string square = "fn square(x: u32) -> u32 { x * x } let a = "
                               "square(7); let b = square(7);";
};

TEST_F(StageCache, ColdAndHotPreserveStatsProvenanceAndSessionPriority) {
    const auto off = run(square, stage::CacheMode::Off);
    EXPECT_FALSE(fs::exists(root));
    const auto cold = run(square, stage::CacheMode::ReadWrite);
    const auto hot = run(square, stage::CacheMode::ReadOnly);
    equivalent(off, cold);
    equivalent(off, hot);
    EXPECT_EQ(off.stats.specializations, 1U);
    EXPECT_EQ(off.stats.cache_hits, 1U);
    EXPECT_EQ(cold.telemetry.lookups, 1U);
    EXPECT_EQ(cold.telemetry.writes, 1U);
    EXPECT_EQ(cold.telemetry.body_evaluations, 1U);
    EXPECT_EQ(hot.telemetry.hits, 1U);
    EXPECT_EQ(hot.telemetry.body_evaluations, 0U);
    EXPECT_EQ(hot.telemetry.bodies_skipped, 1U);
}

TEST_F(StageCache, LowerFuelReplaysOrFallsBackWithoutDuplicateCharges) {
    const auto warm = run(square, stage::CacheMode::ReadWrite);
    for (std::size_t fuel = 0; fuel <= warm.stats.fuel_consumed + 1; ++fuel) {
        SCOPED_TRACE(fuel);
        stage::AnalysisLimits limits;
        limits.fuel = fuel;
        const auto off = run(square, stage::CacheMode::Off, limits);
        const auto hot = run(square, stage::CacheMode::ReadOnly, limits);
        equivalent(off, hot);
        if (hot.telemetry.budget_fallbacks) {
            EXPECT_EQ(hot.telemetry.hits, 0U);
            EXPECT_NE(hot.diagnostics.find("E4004"), std::string::npos);
        }
    }
}

TEST_F(StageCache, EntryBudgetsAndNestedDepthKeepOriginalDiagnostics) {
    const std::string text = "fn square(x: u32) -> u32 { x * x } "
                             "fn outer(x: u32) -> u32 { square(x) } "
                             "let a = outer(7); let b = square(8);";
    run(text, stage::CacheMode::ReadWrite);
    for (std::size_t depth = 0; depth <= 3; ++depth) {
        for (std::size_t count = 0; count <= 4; ++count) {
            SCOPED_TRACE(std::to_string(depth) + "/" + std::to_string(count));
            stage::AnalysisLimits limits;
            limits.recursion_depth = depth;
            limits.specializations = count;
            equivalent(run(text, stage::CacheMode::Off, limits),
                       run(text, stage::CacheMode::ReadOnly, limits));
        }
    }
    const auto hot = run(text, stage::CacheMode::ReadOnly);
    EXPECT_EQ(hot.telemetry.hits, 2U);
    EXPECT_EQ(hot.telemetry.body_evaluations, 1U);
    EXPECT_EQ(hot.stats.maximum_recursion_depth, 2U);
}

TEST_F(StageCache, WhitespaceHitsUseCurrentDefinitionAndParameterLocations) {
    const std::string first =
        "fn id(x: i8) -> i8 { x } fn neg(x: i8) -> i8 { -x } "
        "let a = id(-7i8); let b = neg(-3i8);";
    const std::string changed =
        "// current source\n\nfn id( x : i8 ) -> i8 {\n x\n}\n"
        "fn neg(x: i8) -> i8 {\n -x\n}\n\nlet a = id(-7i8); let b = neg(-3i8);";
    run(first, stage::CacheMode::ReadWrite);
    const auto off = run(changed, stage::CacheMode::Off);
    const auto hot = run(changed, stage::CacheMode::ReadOnly);
    equivalent(off, hot);
    EXPECT_EQ(hot.telemetry.hits, 2U);
    ASSERT_EQ(off.module->bindings.size(), hot.module->bindings.size());
    for (std::size_t i = 0; i < off.module->bindings.size(); ++i) {
        EXPECT_EQ(off.module->bindings[i].value->span.begin,
                  hot.module->bindings[i].value->span.begin);
        EXPECT_EQ(off.module->bindings[i].value->span.end,
                  hot.module->bindings[i].value->span.end);
    }
}

TEST_F(StageCache, EveryScalarWidthBoolAndCastRoundTrip) {
    for (const auto* type :
         {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "usize"}) {
        const std::string t = type;
        const std::string text = "fn calc(x: " + t + ") -> " + t +
                                 " { ((~x) & 15" + t + ") as " + t +
                                 " } let result = calc(7" + t + ");";
        const auto off = run(text, stage::CacheMode::Off);
        equivalent(off, run(text, stage::CacheMode::ReadWrite));
        const auto hot = run(text, stage::CacheMode::ReadOnly);
        equivalent(off, hot);
        EXPECT_EQ(hot.telemetry.hits, 1U);
    }
    const std::string text =
        "fn compare(x: i8) -> bool { !(x < 0i8) } let result = compare(-7i8);";
    run(text, stage::CacheMode::ReadWrite);
    const auto hot = run(text, stage::CacheMode::ReadOnly);
    equivalent(run(text, stage::CacheMode::Off), hot);
    EXPECT_EQ(hot.telemetry.hits, 1U);
}

TEST_F(StageCache, KnownArgumentsKeepTheirResidualWorkOnDiskAndSessionHits) {
    const std::string text =
        "fn mark(n: u32) -> u32 { external_input(); n } "
        "fn ignore(n: u32) -> u32 { 9 } "
        "let a = ignore(mark(5)); let b = ignore(mark(5));";
    const auto off = run(text, stage::CacheMode::Off);
    equivalent(off, run(text, stage::CacheMode::ReadWrite));
    const auto hot = run(text, stage::CacheMode::ReadOnly);
    equivalent(off, hot);
    EXPECT_EQ(hot.telemetry.hits, 1U);
    EXPECT_EQ(hot.stats.cache_hits, 1U);
    EXPECT_EQ(hot.telemetry.body_evaluations, 2U);
}

TEST_F(StageCache, DependenciesArgumentsAndContextInvalidate) {
    run(square, stage::CacheMode::ReadWrite);
    for (const auto& text :
         {"fn square(x: u32) -> u32 { x + x } let a = "
          "square(7); let b = square(7);",
          "fn square(x: u32) -> u32 { x * x } let a = "
          "square(8); let b = square(8);",
          "fn square(x: u32) -> u32 { x * x } fn other() -> "
          "u32 { 1 } let a = square(7); let b = square(7);",
          "fn square(x: u32) -> u32 { x * x } runtime let x = "
          "7; let a = square(x);"}) {
        const auto hot = run(text, stage::CacheMode::ReadOnly);
        equivalent(run(text, stage::CacheMode::Off), hot);
        EXPECT_EQ(hot.telemetry.hits, 0U);
    }
    for (int field = 0; field < 7; ++field) {
        auto context = cache_test::context();
        if (field == 0)
            context.compiler_build += "-changed";
        if (field == 1)
            context.semantic_options += "-changed";
        if (field == 2)
            context.module.relative_path = "other.pgs";
        if (field == 3)
            context.target.cpu = "other-cpu";
        if (field == 4)
            context = cache_test::context(64);
        if (field == 5)
            ++context.query_version;
        if (field == 6)
            ++context.budget_version;
        const auto hot = run(square, stage::CacheMode::ReadOnly, {}, context);
        equivalent(run(square, stage::CacheMode::Off, {}, context), hot);
        EXPECT_EQ(hot.telemetry.hits, 0U);
    }
}

TEST_F(StageCache, UnsupportedBodiesStayOnNormalPath) {
    for (const auto& text :
         {"fn f(x: u32) -> u32 { let y = x; y } let a = f(7);",
          "fn f(x: u32) -> u32 { comptime let y = x; y } let a = f(7);",
          "fn f(x: u32) -> u32 { if x == 7 { 9 } else { 1 } } let a = f(7);",
          "fn f(x: bool) -> bool { x && false } let a = f(true);",
          "fn f(x: u32) -> [u32; 2] { [x, external_input()] } let a = f(7);",
          "fn f(x: u32) -> u32 { f(x) } let a = f(7);",
          "static const global: u32 = 1; fn f(x: u32) -> u32 { global + x } "
          "let a = f(7);",
          "fn f(p: *const u32) -> usize { p as usize } let a = f(4096usize as "
          "*const u32);",
          "record R { value: u32 } fn f(x: R) -> u32 { x.value } let a = "
          "f(R(value: 7));",
          "fn f(x: u32) -> u32 { if x == 0 { 7 } else { x } } runtime let x = "
          "7; let a = f(x);"}) {
        const auto off = run(text, stage::CacheMode::Off);
        equivalent(off, run(text, stage::CacheMode::ReadWrite));
        const auto hot = run(text, stage::CacheMode::ReadOnly);
        equivalent(off, hot);
        EXPECT_EQ(hot.telemetry.lookups, 0U);
    }
    EXPECT_FALSE(fs::exists(root));
}

TEST_F(StageCache, FailedEvaluationDoesNotPublish) {
    const std::string text =
        "fn fail(x: u32) -> u32 { x / 0 } let a = fail(7);";
    const auto off = run(text, stage::CacheMode::Off);
    const auto cold = run(text, stage::CacheMode::ReadWrite);
    equivalent(off, cold);
    EXPECT_FALSE(cold.diagnostics.empty());
    EXPECT_EQ(cold.telemetry.writes, 0U);
    EXPECT_FALSE(fs::exists(root));
}

TEST_F(StageCache, ReadOnlyMissAndStorageFailureAreNonSemantic) {
    const auto off = run(square, stage::CacheMode::Off);
    equivalent(off, run(square, stage::CacheMode::ReadOnly));
    EXPECT_FALSE(fs::exists(root));
    std::ofstream(root) << "a regular file, not a cache directory";
    const auto broken = run(square, stage::CacheMode::ReadWrite);
    equivalent(off, broken);
    EXPECT_GT(broken.telemetry.errors, 0U);
    EXPECT_EQ(broken.telemetry.hits, 0U);
}

TEST_F(StageCache, CorruptOrMissingRequiredObjectsFallBack) {
    run(square, stage::CacheMode::ReadWrite);
    for (const auto& entry :
         fs::recursive_directory_iterator(root / "objects")) {
        if (entry.is_regular_file()) {
            std::ofstream(entry.path(), std::ios::binary | std::ios::trunc)
                << "corrupt";
            break;
        }
    }
    const auto hot = run(square, stage::CacheMode::ReadOnly);
    equivalent(run(square, stage::CacheMode::Off), hot);
    EXPECT_EQ(hot.telemetry.hits, 0U);
    EXPECT_GT(hot.telemetry.errors, 0U);
    fs::remove_all(root / "objects");
    equivalent(run(square, stage::CacheMode::Off),
               run(square, stage::CacheMode::ReadOnly));
}

TEST_F(StageCache, RequiredRecipeWithWrongCostOrVersionIsRejected) {
    run(square, stage::CacheMode::ReadWrite);
    cache_test::Input input(square);
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    const auto key = stage::make_query_key(
        identity->definitions.front(), cache_test::context(),
        {{sema::Type::integer(32), cache::ArgumentStage::Comptime,
          IntegerValue(7)}});
    ASSERT_TRUE(key);
    const auto candidate =
        cache::FileStore(root).lookup(*key, identity->module_fingerprint);
    ASSERT_TRUE(candidate);
    for (const auto mutation : {0U, 1U, 2U, 3U, 4U, 5U}) {
        fs::remove_all(root);
        std::vector<cache::ArtifactInput> artifacts;
        for (const auto& artifact : candidate->artifacts) {
            ASSERT_TRUE(artifact.bytes);
            artifacts.push_back(
                {artifact.name, artifact.object.kind, *artifact.bytes});
            if (artifact.name == "resources") {
                auto& bytes = artifacts.back().bytes;
                ASSERT_GT(bytes.size(), 17U);
                if (mutation < 3)
                    ++bytes[9 + mutation * 4];
                else if (mutation == 3)
                    ++bytes.back(); // Invalid structural fuel anchor.
                else if (mutation == 4)
                    bytes.push_back(0); // Trailing data.
                else
                    bytes.resize(9); // Complete header, truncated recipe.
            }
        }
        ASSERT_TRUE(cache::FileStore(root).publish(
            *key, identity->module_fingerprint, candidate->semantic_result,
            artifacts));
        const auto hot = run(square, stage::CacheMode::ReadOnly);
        equivalent(run(square, stage::CacheMode::Off), hot);
        EXPECT_EQ(hot.telemetry.hits, 0U);
        EXPECT_EQ(hot.telemetry.rejected, 1U);
    }
}

TEST_F(StageCache, WrongTypedOrOversizedValueAndMissingRecipeAreRejected) {
    run(square, stage::CacheMode::ReadWrite);
    cache_test::Input input(square);
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    const auto key = stage::make_query_key(
        identity->definitions.front(), cache_test::context(),
        {{sema::Type::integer(32), cache::ArgumentStage::Comptime,
          IntegerValue(7)}});
    ASSERT_TRUE(key);
    const auto candidate =
        cache::FileStore(root).lookup(*key, identity->module_fingerprint);
    ASSERT_TRUE(candidate);
    for (int mutation = 0; mutation < 5; ++mutation) {
        fs::remove_all(root);
        std::vector<cache::ArtifactInput> artifacts;
        for (const auto& artifact : candidate->artifacts) {
            ASSERT_TRUE(artifact.bytes);
            if (artifact.name == "resources" && mutation == 2)
                continue;
            artifacts.push_back(
                {artifact.name, artifact.object.kind, *artifact.bytes});
            auto& current = artifacts.back();
            if (current.name == "value" && mutation == 0)
                current.bytes =
                    *cache::encode_value({sema::TypeKind::Bool, true});
            if (current.name == "value" && mutation == 1)
                current.bytes.resize(1024 * 1024, 0);
            if (current.name == "resources" && mutation == 3)
                current.required = false;
        }
        if (mutation == 4)
            artifacts.push_back(
                {"future-evidence", cache::ObjectKind::Value,
                 *cache::encode_value({sema::TypeKind::Bool, true})});
        ASSERT_TRUE(cache::FileStore(root).publish(
            *key, identity->module_fingerprint, candidate->semantic_result,
            artifacts));
        const auto hot = run(square, stage::CacheMode::ReadOnly);
        equivalent(run(square, stage::CacheMode::Off), hot);
        EXPECT_EQ(hot.telemetry.hits, 0U);
        EXPECT_EQ(hot.telemetry.rejected, 1U);
    }
}

TEST_F(StageCache, AnalyzerReuseResetsDiskAndPhysicalState) {
    cache_test::Input input(square);
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types(), {},
                                  cache_test::context(),
                                  {stage::CacheMode::ReadWrite, root});
    const auto cold = analyzer.analyze(*input.module);
    const auto stats = analyzer.stats();
    EXPECT_EQ(analyzer.cache_telemetry().writes, 1U);
    const auto hot = analyzer.analyze(*input.module);
    EXPECT_EQ(analyzer.cache_telemetry().hits, 1U);
    EXPECT_EQ(analyzer.cache_telemetry().body_evaluations, 0U);
    EXPECT_EQ(analyzer.stats().fuel_consumed, stats.fuel_consumed);
    EXPECT_EQ(analyzer.stats().specializations, stats.specializations);
    EXPECT_EQ(analyzer.stats().cache_hits, stats.cache_hits);
    EXPECT_FALSE(input.diagnostics.has_error());
}

TEST_F(StageCache, ScalarArraysAndComputedGeneratorsRoundTripEveryWidth) {
    for (const auto* type :
         {"u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "usize"}) {
        for (bool generator : {false, true}) {
            SCOPED_TRACE(std::string(type) +
                         (generator ? "/generator" : "/literal"));
            fs::remove_all(root);
            const std::string t = type;
            const std::string body =
                generator
                    ? "[for i in (1 + 1)..(2 + 4) { (i as " + t + ") + x }]"
                    : "[x, x + 1" + t + ", x + 2" + t + ", x + 3" + t + "]";
            const std::string text = "fn table(x: " + t + ") -> [" + t +
                                     "; 4] { " + body + " } let a = table(7" +
                                     t + "); let b = table(7" + t +
                                     "); let result = b[external_input()];";
            const auto off = run(text, stage::CacheMode::Off);
            ASSERT_TRUE(off.diagnostics.empty()) << off.diagnostics;
            const auto cold = run(text, stage::CacheMode::ReadWrite);
            const auto hot = run(text, stage::CacheMode::ReadOnly);
            equivalent(off, cold);
            equivalent(off, hot);
            EXPECT_EQ(cold.telemetry.writes, 1U);
            EXPECT_EQ(hot.telemetry.hits, 1U);
            EXPECT_EQ(hot.telemetry.body_evaluations, 0U);
            EXPECT_EQ(hot.stats.cache_hits, 1U);
            EXPECT_EQ(hot.stats.array_elements_reserved, 4U);
            EXPECT_EQ(hot.stats.aggregate_constructions, 1U);
        }
    }
    for (const auto* body :
         {"[true, false, true]", "[for i in 1..4 { !(i == 2) }]"}) {
        fs::remove_all(root);
        const std::string text = "fn table() -> [bool; 3] { " +
                                 std::string(body) +
                                 " } let result = table()[external_input()];";
        const auto off = run(text, stage::CacheMode::Off);
        equivalent(off, run(text, stage::CacheMode::ReadWrite));
        const auto hot = run(text, stage::CacheMode::ReadOnly);
        equivalent(off, hot);
        EXPECT_EQ(hot.telemetry.hits, 1U);
        EXPECT_EQ(hot.stats.array_bytes_reserved, 3U);
        for (std::size_t amount : {0U, 2U, 3U}) {
            stage::AnalysisLimits limits;
            limits.array_elements = limits.array_bytes = amount;
            limits.aggregate_members = limits.aggregate_bytes = amount;
            equivalent(run(text, stage::CacheMode::Off, limits),
                       run(text, stage::CacheMode::ReadOnly, limits));
        }
    }
}

TEST_F(StageCache, ArrayResourceOrderAndEveryFuelBoundaryMatchNormalAnalysis) {
    const std::string text =
        "record R { flag: bool } let prefix = R(flag: true); "
        "let before = [9u16, 8u16]; "
        "fn table() -> [u16; 3] { [for i in 1..(1 + 3) { (i * 2) as u16 }] } "
        "let a = table(); let b = table(); let after = [7u16];";
    const auto warm = run(text, stage::CacheMode::ReadWrite);
    ASSERT_TRUE(warm.diagnostics.empty()) << warm.diagnostics;
    ASSERT_EQ(warm.telemetry.writes, 1U);
    for (std::size_t fuel = 0; fuel <= warm.stats.fuel_consumed + 1; ++fuel) {
        for (bool no_construction : {false, true}) {
            SCOPED_TRACE(std::to_string(fuel) + "/" +
                         std::to_string(no_construction));
            stage::AnalysisLimits limits;
            limits.fuel = fuel;
            if (no_construction) {
                limits.array_elements = 2;
                limits.aggregate_members = 3;
            }
            equivalent(run(text, stage::CacheMode::Off, limits),
                       run(text, stage::CacheMode::ReadOnly, limits));
        }
    }
    const std::array quotas{&stage::AnalysisLimits::array_elements,
                            &stage::AnalysisLimits::array_bytes,
                            &stage::AnalysisLimits::aggregate_members,
                            &stage::AnalysisLimits::aggregate_bytes};
    const std::array<std::size_t, 4> exact{6, 12, 7, 13};
    for (std::size_t i = 0; i < quotas.size(); ++i) {
        for (const auto amount :
             {std::size_t{0}, exact[i] - 1, exact[i], exact[i] + 1}) {
            SCOPED_TRACE(std::to_string(i) + "/" + std::to_string(amount));
            stage::AnalysisLimits limits;
            limits.*quotas[i] = amount;
            equivalent(run(text, stage::CacheMode::Off, limits),
                       run(text, stage::CacheMode::ReadOnly, limits));
        }
    }
    stage::AnalysisLimits limits;
    limits.array_elements = 4;
    limits.aggregate_members = 3;
    const auto off = run(text, stage::CacheMode::Off, limits);
    const auto hot = run(text, stage::CacheMode::ReadOnly, limits);
    equivalent(off, hot);
    EXPECT_NE(hot.diagnostics.find("E4008"), std::string::npos);
    EXPECT_EQ(hot.diagnostics.find("E4010"), std::string::npos);
    EXPECT_GE(hot.telemetry.budget_fallbacks, 1U);
}

TEST_F(StageCache, ArrayArgumentConstructionStillChargesOnSessionHits) {
    const std::string text =
        "fn table() -> [u32; 3] { [for i in 0..3 { i * 2 }] } "
        "fn last(a: [u32; 3]) -> u32 { a[2] } "
        "let a = table(); let b = table(); "
        "let result = last([1, 2, 3]) + last([1, 2, 3]) + a[external_input()];";
    const auto warm = run(text, stage::CacheMode::ReadWrite);
    ASSERT_TRUE(warm.diagnostics.empty()) << warm.diagnostics;
    EXPECT_EQ(warm.stats.array_elements_reserved, 9U);
    EXPECT_EQ(warm.stats.aggregate_constructions, 3U);
    const auto hot = run(text, stage::CacheMode::ReadOnly);
    equivalent(run(text, stage::CacheMode::Off), hot);
    EXPECT_EQ(hot.telemetry.hits, 1U);
    EXPECT_EQ(hot.stats.cache_hits, 2U);
    stage::AnalysisLimits limits;
    limits.array_elements = 8;
    equivalent(run(text, stage::CacheMode::Off, limits),
               run(text, stage::CacheMode::ReadOnly, limits));
    for (std::size_t depth = 0; depth < 3; ++depth) {
        for (std::size_t count = 0; count < 4; ++count) {
            limits = {};
            limits.recursion_depth = depth;
            limits.specializations = count;
            equivalent(run(text, stage::CacheMode::Off, limits),
                       run(text, stage::CacheMode::ReadOnly, limits));
        }
    }
}

TEST_F(StageCache, ArrayFailuresAndUnsupportedGeneratorsDoNotPublish) {
    for (const auto* body :
         {"[for i in 3..3 { i }]", "[for i in 3..1 { i }]",
          "[for i in 0..(1 + 1) { i }]", "[for i in 0..3 { 7 / (2 - i) }]"}) {
        fs::remove_all(root);
        const std::string text = "fn table() -> [u32; 3] { " +
                                 std::string(body) + " } let a = table();";
        const auto off = run(text, stage::CacheMode::Off);
        const auto cold = run(text, stage::CacheMode::ReadWrite);
        equivalent(off, cold);
        EXPECT_FALSE(cold.diagnostics.empty());
        EXPECT_EQ(cold.telemetry.writes, 0U);
        EXPECT_FALSE(fs::exists(root));
    }
    for (const auto* text :
         {"fn table() -> [u32; 3] { [for i in 0..3 { let x = i; x }] } let a = "
          "table();",
          "fn table() -> [u32; 3] { [for i in 0..3 { if i == 1 { 7 } else { i "
          "} }] } let a = table();",
          "fn table() -> [u32; 3] { [for i in 0..3 { external_input() }] } let "
          "a = table();",
          "fn table(x: u32) -> [u32; 3] { [x, x, x] } runtime let x = 7; let a "
          "= table(x);",
          "fn mark() -> u32 { external_input(); 7 } fn table(x: u32) -> [u32; "
          "3] { [x, x, x] } let a = table(mark());",
          "fn table(a: [u32; 3]) -> [u32; 3] { a } let a = table([1, 2, "
          "3]);"}) {
        fs::remove_all(root);
        const auto off = run(text, stage::CacheMode::Off);
        const auto cold = run(text, stage::CacheMode::ReadWrite);
        equivalent(off, cold);
        equivalent(off, run(text, stage::CacheMode::ReadOnly));
        EXPECT_EQ(cold.telemetry.lookups, 0U);
        EXPECT_EQ(cold.telemetry.writes, 0U);
        EXPECT_FALSE(fs::exists(root));
    }
}

TEST_F(StageCache, ArrayHitsRemapLocationsAndPreserveShadowedIndices) {
    const std::string text =
        "fn table(i: u32) -> [u32; 3] { [for i in i..(i + 3) { i * i }] } "
        "let a = table(2); let result = a[external_input()];";
    run(text, stage::CacheMode::ReadWrite);
    const std::string changed =
        "// shifted\n\n\nfn table(i: u32) -> [u32; 3] {\n"
        "[for i in i..(i + 3) {\n i * i\n }]\n }\n"
        "let a = table(2); let result = a[external_input()];";
    const auto off = run(changed, stage::CacheMode::Off);
    const auto hot = run(changed, stage::CacheMode::ReadOnly);
    equivalent(off, hot);
    EXPECT_EQ(hot.telemetry.hits, 1U);
    for (std::size_t fuel = 0; fuel <= off.stats.fuel_consumed; ++fuel) {
        stage::AnalysisLimits limits;
        limits.fuel = fuel;
        equivalent(run(changed, stage::CacheMode::Off, limits),
                   run(changed, stage::CacheMode::ReadOnly, limits));
    }
}

TEST_F(StageCache, GeneratorRecipeSizeDoesNotGrowWithIterationCount) {
    std::size_t small_recipe = 0;
    for (const auto length : {3, 1024}) {
        fs::remove_all(root);
        const std::string text = "fn table() -> [u32; " +
                                 std::to_string(length) + "] { [for i in 0.." +
                                 std::to_string(length) +
                                 " { (i * 37) ^ (i >> 3) }] } let a = table();";
        const auto cold = run(text, stage::CacheMode::ReadWrite);
        ASSERT_EQ(cold.telemetry.writes, 1U);
        const auto hot = run(text, stage::CacheMode::ReadOnly);
        equivalent(run(text, stage::CacheMode::Off), hot);
        EXPECT_EQ(hot.telemetry.hits, 1U);
        cache_test::Input input(text);
        const auto identity = input.identity();
        ASSERT_TRUE(identity);
        const auto key = stage::make_query_key(identity->definitions.front(),
                                               cache_test::context(), {});
        ASSERT_TRUE(key);
        const auto candidate =
            cache::FileStore(root).lookup(*key, identity->module_fingerprint);
        ASSERT_TRUE(candidate);
        for (const auto& artifact : candidate->artifacts) {
            if (artifact.name != "resources")
                continue;
            ASSERT_TRUE(artifact.bytes);
            EXPECT_LT(artifact.bytes->size(), 256U);
            if (length == 3)
                small_recipe = artifact.bytes->size();
            else
                EXPECT_EQ(artifact.bytes->size(), small_recipe);
        }
    }
    EXPECT_GT(small_recipe, 0U);
}

TEST_F(StageCache, InvalidArrayShapePayloadAndResourceOrderAreRejected) {
    const std::string text =
        "fn table() -> [u32; 3] { [for i in 0..3 { i * 2 }] } let a = table();";
    run(text, stage::CacheMode::ReadWrite);
    cache_test::Input input(text);
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    const auto key = stage::make_query_key(identity->definitions.front(),
                                           cache_test::context(), {});
    ASSERT_TRUE(key);
    const auto candidate =
        cache::FileStore(root).lookup(*key, identity->module_fingerprint);
    ASSERT_TRUE(candidate);
    for (int mutation = 0; mutation < 8; ++mutation) {
        fs::remove_all(root);
        std::vector<cache::ArtifactInput> artifacts;
        for (const auto& artifact : candidate->artifacts) {
            ASSERT_TRUE(artifact.bytes);
            artifacts.push_back(
                {artifact.name, artifact.object.kind, *artifact.bytes});
            auto& bytes = artifacts.back().bytes;
            if (artifact.name == "value") {
                if (mutation == 0)
                    bytes[10] = 255; // Wrong array shape.
                if (mutation == 1)
                    bytes.resize(19); // Truncated elements.
                if (mutation == 2)
                    bytes.push_back(0); // Extra payload.
                if (mutation == 3)
                    bytes = *cache::encode_value(
                        {sema::Type::array(sema::TypeKind::Bool, 3),
                         std::vector<bool>{true, false, true}});
            } else {
                if (mutation == 4)
                    ++bytes[bytes.size() - 20]; // Reservation order.
                if (mutation == 5)
                    ++bytes[bytes.size() - 16]; // Element count.
                if (mutation == 6)
                    ++bytes[bytes.size() - 8]; // Repeat start.
                if (mutation == 7)
                    ++bytes.back(); // Repeat count.
            }
        }
        ASSERT_TRUE(cache::FileStore(root).publish(
            *key, identity->module_fingerprint, candidate->semantic_result,
            artifacts));
        const auto hot = run(text, stage::CacheMode::ReadOnly);
        equivalent(run(text, stage::CacheMode::Off), hot);
        EXPECT_EQ(hot.telemetry.hits, 0U);
        EXPECT_EQ(hot.telemetry.rejected, 1U);
    }
}

TEST_F(StageCache,
       ResidualScalarDefinitionsReuseWithCurrentArgumentsAndLogicalStats) {
    const std::string text =
        "fn scale(k: u32, x: u32) -> u32 { k * x } "
        "let a = external_input(); let b = external_input(); let aa = scale(4, "
        "a); let bb = scale(4, b);";
    const auto off = run(text, stage::CacheMode::Off);
    const auto cold = run(text, stage::CacheMode::ReadWrite);
    const auto hot = run(text, stage::CacheMode::ReadOnly);
    equivalent(off, cold);
    equivalent(off, hot);
    EXPECT_EQ(cold.telemetry.residual_writes, 1U);
    EXPECT_EQ(hot.telemetry.residual_hits, 1U);
    EXPECT_EQ(hot.telemetry.body_evaluations, 0U);
    EXPECT_EQ(hot.telemetry.bodies_skipped, 1U);
    EXPECT_EQ(hot.stats.residual_specializations, 1U);
    EXPECT_EQ(hot.stats.residual_cache_hits, 1U);
    ASSERT_EQ(hot.module->residual_functions.size(), 1U);
    EXPECT_TRUE(mir::lower(*hot.module));
}
TEST_F(StageCache, ResidualWidthsCastsBooleansAndRuntimeTrapEvidenceRoundTrip) {
    for (const auto text :
         {"fn f(k: i8, x: i8) -> i8 { -(k + x) } let r = f(-3i8, "
          "external_input() as i8);",
          "fn f(x: bool) -> bool { !x } let r = f(external_input() == 0);",
          "fn f(x: u64) -> u16 { (x + 3u64) as u16 } let r = "
          "f(external_input() as u64);",
          "fn f(k: u32, x: u32) -> u32 { (k + 2) * x } let r = f(4, "
          "external_input());",
          "fn f(x: u32, y: u32) -> u32 { x / y } let r = f(external_input(), "
          "external_input());",
          "fn f(x: u32) -> u32 { x << 2 } let r = f(external_input());"}) {
        fs::remove_all(root);
        const auto off = run(text, stage::CacheMode::Off);
        equivalent(off, run(text, stage::CacheMode::ReadWrite));
        const auto hot = run(text, stage::CacheMode::ReadOnly);
        equivalent(off, hot);
        EXPECT_EQ(hot.telemetry.residual_hits, 1U) << text;
        EXPECT_EQ(hot.telemetry.body_evaluations, 0U) << text;
        EXPECT_TRUE(mir::lower(*hot.module));
    }
}
TEST_F(StageCache,
       ResidualBudgetsEveryFuelBoundaryAndFailedComponentsDoNotPublish) {
    const std::string text =
        "fn f(k: u32, x: u32) -> u32 { (k + 2) * x } "
        "let a = external_input(); let aa = f(4, a); let bb = f(5, a);";
    const auto cold = run(text, stage::CacheMode::ReadWrite);
    ASSERT_TRUE(cold.diagnostics.empty());
    for (std::size_t fuel = 0; fuel <= cold.stats.fuel_consumed + 1; ++fuel) {
        stage::AnalysisLimits limits;
        limits.fuel = fuel;
        equivalent(run(text, stage::CacheMode::Off, limits),
                   run(text, stage::CacheMode::ReadOnly, limits));
    }
    for (unsigned limit = 0; limit <= 3; ++limit) {
        stage::AnalysisLimits limits;
        limits.residual_specializations = limit;
        equivalent(run(text, stage::CacheMode::Off, limits),
                   run(text, stage::CacheMode::ReadOnly, limits));
        limits.recursion_depth = limit;
        equivalent(run(text, stage::CacheMode::Off, limits),
                   run(text, stage::CacheMode::ReadOnly, limits));
    }
    fs::remove_all(root);
    stage::AnalysisLimits limits;
    limits.residual_specializations = 0;
    const auto failed = run(text, stage::CacheMode::ReadWrite, limits);
    EXPECT_FALSE(failed.diagnostics.empty());
    EXPECT_EQ(failed.telemetry.writes, 0U);
    EXPECT_FALSE(fs::exists(root));
}
TEST_F(StageCache, ResidualCacheHasNoCallerIdentityAndMapsWhitespace) {
    const std::string text =
        "fn scale(k: u32, x: u32) -> u32 { k * x } "
        "let a = external_input(); let b = external_input(); let aa = scale(4, "
        "a); let bb = scale(4, b);";
    run(text, stage::CacheMode::ReadWrite);
    const std::string shifted = "// current revision\n\n" + text;
    const auto hot = run(shifted, stage::CacheMode::ReadOnly);
    equivalent(run(shifted, stage::CacheMode::Off), hot);
    EXPECT_EQ(hot.telemetry.residual_hits, 1U);
    cache_test::Input input(text);
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    const auto key = make_query_key(
        *identity->find(cache::DefinitionKind::Function, "scale"),
        cache_test::context(),
        {{sema::Type::integer(32), cache::ArgumentStage::Comptime,
          IntegerValue(4)},
         {sema::Type::integer(32), cache::ArgumentStage::Runtime,
          std::nullopt}},
        cache::QueryKind::SpecializeResidual);
    ASSERT_TRUE(key);
    const auto candidate =
        cache::FileStore(root).lookup(*key, identity->module_fingerprint);
    ASSERT_TRUE(candidate);
    for (const auto& artifact : candidate->artifacts) {
        if (artifact.name != "evidence")
            continue;
        const auto snapshot = stage::ProvenanceSnapshot::open(*artifact.bytes);
        ASSERT_TRUE(snapshot);
        const auto restored =
            snapshot->restore(*identity, cache_test::context());
        ASSERT_TRUE(restored);
        EXPECT_EQ((*restored)->roots().size(), 0U);
        for (const auto& site : (*restored)->sites())
            EXPECT_NE(site.label, "external_input");
    }
}
TEST_F(StageCache, ResidualMissingEvidenceRecipeAndInvalidTemplateFailClosed) {
    const std::string text =
        "fn f(x: u32) -> u32 { x + 1 } let r = f(external_input());";
    run(text, stage::CacheMode::ReadWrite);
    cache_test::Input input(text);
    const auto identity = input.identity();
    ASSERT_TRUE(identity);
    const auto key =
        make_query_key(*identity->find(cache::DefinitionKind::Function, "f"),
                       cache_test::context(),
                       {{sema::Type::integer(32), cache::ArgumentStage::Runtime,
                         std::nullopt}},
                       cache::QueryKind::SpecializeResidual);
    ASSERT_TRUE(key);
    const auto stored =
        cache::FileStore(root).lookup(*key, identity->module_fingerprint);
    ASSERT_TRUE(stored);
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        fs::remove_all(root);
        std::vector<cache::ArtifactInput> artifacts;
        for (const auto& artifact : stored->artifacts) {
            if ((mutation == 0 && artifact.name == "evidence") ||
                (mutation == 1 && artifact.name == "resources"))
                continue;
            artifacts.push_back({artifact.name, artifact.object.kind,
                                 *artifact.bytes, artifact.required});
            if (mutation == 2 && artifact.name == "evidence")
                artifacts.back().required = false;
            if (mutation == 3 && artifact.name == "template")
                artifacts.back().bytes.back() ^= 0x80;
            if (mutation == 4 && artifact.name == "resources")
                artifacts.back().bytes.back() ^= 0x80;
            if (mutation == 5 && artifact.name == "evidence")
                artifacts.back().bytes.back() ^= 0x80;
        }
        ASSERT_TRUE(
            cache::FileStore(root).publish(*key, identity->module_fingerprint,
                                           stored->semantic_result, artifacts));
        const auto hot = run(text, stage::CacheMode::ReadOnly);
        equivalent(run(text, stage::CacheMode::Off), hot);
        EXPECT_EQ(hot.telemetry.residual_hits, 0U);
        EXPECT_EQ(hot.telemetry.body_evaluations, 1U);
    }
}

} // namespace
