#include "../../lib/stage/scalar_replay.h"
#include "cache_test_input.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/provenance_analysis.h"
#include "pagos/stage/stage_analyzer.h"

#include <array>
#include <atomic>
#include <fstream>
#include <sstream>

namespace {
using namespace pagos;
using namespace pagos::stage;
namespace fs = std::filesystem;
class ScalarReplay : public ::testing::Test {
  protected:
    ScalarReplay() {
        static std::atomic<unsigned> sequence{};
        root =
            fs::temp_directory_path() /
            ("pagos-scalar-replay-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
    }
    ~ScalarReplay() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    struct Outcome {
        std::array<std::size_t, 11> stats;
        CacheTelemetry cache;
        std::string text;
    };
    Outcome run(const std::string& source,
                CacheMode mode = CacheMode::ReadWrite,
                CacheEvidence evidence = CacheEvidence::Value,
                AnalysisLimits limits = {}, unsigned bits = 32) {
        cache_test::Input input(source, bits);
        StageAnalyzer analyzer(
            input.diagnostics, input.checker.types(), limits,
            cache_test::context(bits),
            {.mode = mode, .directory = root, .evidence = evidence});
        auto module = analyzer.analyze(*input.module);
        std::ostringstream text;
        hir::print(*module, text);
        explain_provenance(*module->provenance, text);
        input.diagnostics.render(text);
        if (!input.diagnostics.has_error()) {
            const auto lowered = mir::lower(*module);
            EXPECT_TRUE(lowered);
            if (lowered)
                mir::print(*lowered, text);
        }
        const auto& s = analyzer.stats();
        return {{s.fuel_consumed, s.specializations, s.cache_hits,
                 s.maximum_recursion_depth, s.array_elements_reserved,
                 s.array_bytes_reserved, s.aggregate_constructions,
                 s.aggregate_members_reserved, s.aggregate_bytes_reserved,
                 s.residual_specializations, s.residual_cache_hits},
                analyzer.cache_telemetry(),
                text.str()};
    }
    cache::QueryKey key(cache_test::Input& input,
                        std::string_view name = "square",
                        std::uint64_t value = 7) {
        const auto identity = input.identity();
        const auto* definition =
            identity->find(cache::DefinitionKind::Function, name);
        if (!definition)
            throw std::runtime_error("missing definition");
        std::vector<cache::StageArgument> arguments;
        for (const auto& type : definition->parameters)
            arguments.push_back({type, cache::ArgumentStage::Comptime,
                                 type.kind == sema::TypeKind::Bool
                                     ? Constant(true)
                                     : Constant(*IntegerValue::create(
                                           type.integer_type, value))});
        return *make_query_key(
            *definition,
            cache_test::context(input.checker.types().pointer_bits),
            std::move(arguments));
    }
    auto load(cache_test::Input& input, std::string_view name = "square",
              std::uint64_t value = 7) {
        return load_scalar_replay(cache::FileStore(root), *input.module,
                                  input.checker.types(),
                                  key(input, name, value));
    }
    void erase_manifest(const cache::QueryKey& pure,
                        const CacheIdentity& identity) {
        const auto hash =
            cache::query_fingerprint(scalar_replay_key(pure))->hex();
        fs::remove(root / "queries" / hash.substr(0, 2) / hash /
                   (identity.module_fingerprint.hex() + ".manifest"));
    }
    fs::path root;
    const std::string program = "fn square(x: u32) -> u32 { x * x } let a = "
                                "square(7); let b = square(7);";
};
TEST_F(ScalarReplay, RealStagePublishesOwnedRecipeAndWarmHitKeepsAllSemantics) {
    const auto off = run(program, CacheMode::Off);
    EXPECT_FALSE(fs::exists(root));
    const auto cold = run(program);
    const auto hot =
        run(program, CacheMode::ReadOnly, CacheEvidence::ScalarReplay);
    EXPECT_EQ(off.text, cold.text);
    EXPECT_EQ(off.text, hot.text);
    EXPECT_EQ(off.stats, cold.stats);
    EXPECT_EQ(off.stats, hot.stats);
    EXPECT_EQ(cold.cache.writes, 1U);
    EXPECT_EQ(cold.cache.replay_writes, 1U);
    EXPECT_EQ(hot.cache.hits, 1U);
    EXPECT_EQ(hot.cache.replay_hits, 1U);
    EXPECT_EQ(hot.cache.body_evaluations, 0U);
    std::optional<ScalarReplayRecord> owned;
    {
        cache_test::Input input(program);
        auto record = load(input);
        ASSERT_TRUE(record);
        EXPECT_EQ(std::get<IntegerValue>(record->value.value).bits(), 49U);
        EXPECT_EQ(record->descriptor.nodes.size(), 3U);
        EXPECT_EQ(record->descriptor.nodes.back().operation,
                  ScalarReplayOp::Multiply);
        EXPECT_EQ(record->descriptor.nodes.back().operands,
                  (std::vector<std::uint32_t>{0, 1}));
        EXPECT_EQ(record->descriptor.nodes[0].input, 0U);
        EXPECT_NE(record->descriptor.nodes[0].position,
                  record->descriptor.nodes[1].position);
        owned = std::move(*record);
    }
    ASSERT_TRUE(owned);
    EXPECT_TRUE(encode_replay_descriptor(owned->descriptor));
    EXPECT_EQ(owned->descriptor.expected_result,
              *replay_value_fingerprint(owned->value));
}
TEST_F(ScalarReplay,
       MissingLegacyEvidenceFallsBackAndReadWriteUpgradesSeparateIndex) {
    run(program);
    cache_test::Input input(program);
    erase_manifest(key(input), *input.identity());
    ASSERT_FALSE(load(input));
    EXPECT_EQ(load(input).error().code,
              ScalarReplayErrorCode::MissingDescriptor);
    const auto old = run(program, CacheMode::ReadOnly);
    EXPECT_EQ(old.cache.hits, 1U);
    const auto required =
        run(program, CacheMode::ReadOnly, CacheEvidence::ScalarReplay);
    EXPECT_EQ(required.cache.hits, 0U);
    EXPECT_EQ(required.cache.replay_rejected, 1U);
    EXPECT_EQ(required.cache.body_evaluations, 1U);
    EXPECT_EQ(old.text, required.text);
    EXPECT_EQ(old.stats, required.stats);
    const auto upgraded =
        run(program, CacheMode::ReadWrite, CacheEvidence::ScalarReplay);
    EXPECT_EQ(upgraded.cache.replay_writes, 1U);
    EXPECT_EQ(upgraded.cache.writes, 0U); // Existing value index is unchanged.
    EXPECT_TRUE(load(input));
    EXPECT_EQ(run(program, CacheMode::ReadOnly, CacheEvidence::ScalarReplay)
                  .cache.hits,
              1U);
}
TEST_F(ScalarReplay, MissingOrCorruptDescriptorCannotClaimReplayCapability) {
    run(program);
    cache_test::Input input(program);
    auto record = load(input);
    ASSERT_TRUE(record);
    const auto hash = record->object.digest.hex();
    const auto path = root / "objects" / hash.substr(0, 2) / hash;
    std::ifstream stream(path, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(stream)), {});
    stream.close();
    for (bool missing : {false, true}) {
        if (missing)
            fs::remove(path);
        else {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "corrupt";
        }
        EXPECT_FALSE(load(input));
        const auto hot =
            run(program, CacheMode::ReadOnly, CacheEvidence::ScalarReplay);
        EXPECT_EQ(hot.cache.hits, 0U);
        EXPECT_EQ(hot.cache.body_evaluations, 1U);
        EXPECT_EQ(run(program, CacheMode::ReadOnly).cache.hits, 1U);
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << original;
        }
    }
    EXPECT_TRUE(load(input));
}
TEST_F(ScalarReplay, RehashedWrongSourceRecipeOrResultBindingIsRejected) {
    run(program);
    cache_test::Input input(program);
    const auto pure = key(input);
    const auto identity = *input.identity();
    const auto record = load(input);
    ASSERT_TRUE(record);
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        auto descriptor = record->descriptor;
        auto value = record->value;
        if (mutation == 0)
            descriptor.nodes.back().operation = ScalarReplayOp::Add;
        if (mutation == 1)
            descriptor.nodes[0].position = descriptor.nodes[1].position;
        if (mutation == 2)
            value.value = IntegerValue(50);
        if (mutation == 3)
            descriptor.verified_inputs.bytes[0] ^= 1;
        auto replay = encode_replay_descriptor(descriptor);
        const auto encoded_value = cache::encode_value(value);
        ASSERT_TRUE(replay);
        ASSERT_TRUE(encoded_value);
        if (mutation == 4)
            (*replay)[9] = 2; // A future descriptor version, with valid CAS.
        erase_manifest(pure, identity);
        std::vector<cache::ArtifactInput> artifacts{
            cache::ArtifactInput{"value", cache::ObjectKind::Value,
                                 *encoded_value},
            cache::ArtifactInput{"replay", cache::ObjectKind::ReplayDescriptor,
                                 *replay}};
        if (mutation == 5)
            artifacts.pop_back();
        if (mutation == 6)
            artifacts.back().required = false;
        ASSERT_TRUE(cache::FileStore(root).publish(
            scalar_replay_key(pure), identity.module_fingerprint,
            replay_detail::semantic(*encoded_value, *replay), artifacts));
        const auto rejected = load(input);
        EXPECT_FALSE(rejected) << mutation;
        EXPECT_EQ(run(program, CacheMode::ReadOnly, CacheEvidence::ScalarReplay)
                      .cache.hits,
                  0U);
    }
}
TEST_F(ScalarReplay, UnsupportedBodiesAndCastsNeverGetSyntheticLiteralRecipes) {
    for (const auto& source :
         {"fn f(x: u32) -> u32 { x as u32 } let a = f(7);",
          "fn f(x: u32) -> u32 { if x == 0 { 1 } else { x } } let a = f(7);",
          "fn f(x: bool) -> bool { x && true } let a = f(true);",
          "fn f(x: u32) -> u32 { let a = x * x; a } let a = f(7);",
          "fn g(x: u32) -> u32 { x } fn f(x: u32) -> u32 { g(x) } let a = "
          "f(7);",
          "fn f(x: u32) -> [u32; 2] { [x, x] } let a = f(7);",
          "static mut state: u32 = 1; fn f(x: u32) -> u32 { state + x } let a "
          "= f(7);"}) {
        fs::remove_all(root);
        const auto off = run(source, CacheMode::Off);
        const auto cold = run(source);
        EXPECT_EQ(off.text, cold.text);
        EXPECT_EQ(off.stats, cold.stats);
        cache_test::Input input(source);
        const auto record = load(input, "f");
        ASSERT_FALSE(record) << source;
        EXPECT_EQ(record.error().code,
                  ScalarReplayErrorCode::UnsupportedSource);
    }
}
TEST_F(ScalarReplay,
       RealTypedLiteralsBoolAndTargetSizedArgumentsRecordExactly) {
    for (const auto& source :
         {"fn f(x: u32) -> u32 { 7 } let a = f(7);",
          "fn f(x: u8) -> u8 { (~x) + 2u8 } let a = f(7u8);",
          "fn f(x: i64) -> bool { -x < 1i64 } let a = f(7i64);",
          "fn f(x: bool) -> bool { !x } let a = f(true);",
          "fn f(x: usize) -> usize { x * 2usize } let a = f(7usize);"}) {
        for (unsigned bits : {32U, 64U}) {
            fs::remove_all(root);
            const auto off =
                run(source, CacheMode::Off, CacheEvidence::Value, {}, bits);
            const auto cold = run(source, CacheMode::ReadWrite,
                                  CacheEvidence::Value, {}, bits);
            const auto hot = run(source, CacheMode::ReadOnly,
                                 CacheEvidence::ScalarReplay, {}, bits);
            EXPECT_EQ(off.text, cold.text);
            EXPECT_EQ(off.text, hot.text);
            EXPECT_EQ(off.stats, hot.stats);
            EXPECT_EQ(cold.cache.replay_writes, 1U);
            EXPECT_EQ(hot.cache.replay_hits, 1U);
            cache_test::Input input(source, bits);
            auto record = load(input, "f");
            ASSERT_TRUE(record) << source;
            EXPECT_TRUE(validate_replay_descriptor(
                &record->descriptor, *input.identity(), key(input, "f"),
                input.identity()->module_fingerprint, record->value));
        }
    }
}
TEST_F(ScalarReplay,
       SourceRebindingAndTargetInputOrCompilerChangesAreIsolated) {
    run(program);
    cache_test::Input moved("// moved\n" + program);
    EXPECT_TRUE(load(moved));
    cache_test::Input edited(
        "fn square(x: u32) -> u32 { x + x } let a = square(7);");
    EXPECT_FALSE(load(edited));
    EXPECT_FALSE(load(moved, "square", 8));
    auto changed = key(moved);
    changed.context.compiler_build = "different-build";
    EXPECT_FALSE(load_scalar_replay(cache::FileStore(root), *moved.module,
                                    moved.checker.types(), changed));
    cache_test::Input other(program, 64);
    EXPECT_FALSE(load(other));
    const auto encoded = cache::encode_query_key(scalar_replay_key(key(moved)));
    ASSERT_TRUE(encoded);
    EXPECT_EQ(cache::decode_query_key(*encoded)->kind,
              cache::QueryKind::FreezeScalarReplay);
    auto runtime = scalar_replay_key(key(moved));
    runtime.arguments[0].stage = cache::ArgumentStage::Runtime;
    runtime.arguments[0].value.reset();
    EXPECT_FALSE(cache::encode_query_key(runtime));
}
TEST_F(ScalarReplay,
       FuelErrorsAndWarmBudgetFallbackDoNotChangeDiagnosticsOrCharges) {
    const auto warm = run(program);
    for (std::size_t fuel = 0; fuel <= warm.stats[0] + 1; ++fuel) {
        const auto off =
            run(program, CacheMode::Off, CacheEvidence::Value, {.fuel = fuel});
        const auto hot = run(program, CacheMode::ReadOnly,
                             CacheEvidence::ScalarReplay, {.fuel = fuel});
        EXPECT_EQ(off.text, hot.text) << fuel;
        EXPECT_EQ(off.stats, hot.stats) << fuel;
    }
    fs::remove_all(root);
    const std::string failure =
        "fn square(x: u32) -> u32 { x / 0 } let a = square(7);";
    const auto failed = run(failure);
    EXPECT_EQ(failed.cache.replay_writes, 0U);
    cache_test::Input input(failure);
    EXPECT_FALSE(load(input));
}
} // namespace
