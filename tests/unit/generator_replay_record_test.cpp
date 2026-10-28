#include "../../lib/stage/generator_replay_record.h"
#include "cache_test_input.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/generator_replay_execution.h"
#include "pagos/stage/provenance_analysis.h"
#include "pagos/stage/stage_analyzer.h"

#include <array>
#include <atomic>
#include <fstream>
#include <sstream>

namespace {
using namespace pagos;
using namespace pagos::stage;
using Code = GeneratorReplayExecutionErrorCode;
namespace fs = std::filesystem;
class GeneratorRecord : public ::testing::Test {
  protected:
    GeneratorRecord() {
        static std::atomic<unsigned> serial{};
        root =
            fs::temp_directory_path() /
            ("pagos-generator-record-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(serial++));
    }
    ~GeneratorRecord() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    struct Outcome {
        std::array<std::size_t, 11> stats;
        CacheTelemetry telemetry;
        std::string text;
    };
    Outcome run(const std::string& text, CacheMode mode = CacheMode::ReadWrite,
                CacheEvidence evidence = CacheEvidence::Value,
                AnalysisLimits limits = {}, unsigned bits = 32,
                provenance::EvidenceBuildConfig spill = {}) {
        cache_test::Input input(text, bits);
        StageAnalyzer analyzer(
            input.diagnostics, input.checker.types(), limits,
            cache_test::context(bits),
            {.mode = mode, .directory = root, .evidence = evidence}, spill);
        auto module = analyzer.analyze(*input.module);
        std::ostringstream out;
        hir::print(*module, out);
        explain_provenance(*module->provenance, out);
        input.diagnostics.render(out);
        if (!input.diagnostics.has_error()) {
            const auto lowered = mir::lower(*module);
            EXPECT_TRUE(lowered);
            if (lowered)
                mir::print(*lowered, out);
        }
        const auto& s = analyzer.stats();
        return {{s.fuel_consumed, s.specializations, s.cache_hits,
                 s.maximum_recursion_depth, s.array_elements_reserved,
                 s.array_bytes_reserved, s.aggregate_constructions,
                 s.aggregate_members_reserved, s.aggregate_bytes_reserved,
                 s.residual_specializations, s.residual_cache_hits},
                analyzer.cache_telemetry(),
                out.str()};
    }
    cache::QueryKey key(const cache_test::Input& input) {
        const auto identity = input.identity();
        const auto* definition =
            identity->find(cache::DefinitionKind::Function, "table");
        if (!definition)
            throw std::runtime_error("missing table");
        std::vector<cache::StageArgument> args;
        for (const auto& type : definition->parameters)
            args.push_back(
                {type, cache::ArgumentStage::Comptime,
                 type == sema::TypeKind::Bool
                     ? Constant(true)
                     : Constant(*IntegerValue::create(type.integer_type, 7))});
        return *make_query_key(
            *definition,
            cache_test::context(input.checker.types().pointer_bits),
            std::move(args));
    }
    auto load(const cache_test::Input& input, cache::Limits limits = {}) {
        return load_generator_replay(cache::FileStore(root), *input.module,
                                     input.checker.types(), key(input), limits);
    }
    fs::path manifest(const cache_test::Input& input) {
        const auto hash =
            cache::query_fingerprint(generator_replay_key(key(input)))->hex();
        return root / "queries" / hash.substr(0, 2) / hash /
               (input.identity()->module_fingerprint.hex() + ".manifest");
    }
    void reseal(const cache_test::Input& input,
                const GeneratorReplayDescriptor& descriptor,
                const GeneratorReplayObservation& observation,
                const cache::TypedValue& value) {
        const auto d = encode_generator_replay_descriptor(descriptor);
        const auto o = encode_generator_replay_observation(observation);
        const auto v = cache::encode_value(value);
        ASSERT_TRUE(d);
        ASSERT_TRUE(o);
        ASSERT_TRUE(v);
        fs::remove(manifest(input)); // Only this test's isolated generation.
        const std::array artifacts{
            cache::ArtifactInput{"value", cache::ObjectKind::Value, *v},
            cache::ArtifactInput{
                "generator", cache::ObjectKind::GeneratorReplayDescriptor, *d},
            cache::ArtifactInput{"observation",
                                 cache::ObjectKind::GeneratorReplayObservation,
                                 *o}};
        ASSERT_TRUE(cache::FileStore(root).publish(
            generator_replay_key(key(input)),
            input.identity()->module_fingerprint,
            generator_detail::semantic(*v, *d, *o), artifacts));
    }
    fs::path root;
    const std::string program =
        "fn table(x: u32) -> [u32; 4] { "
        "[for i in 2..6 { i * x + 1 }] } let a = table(7); let b = table(7);";
};
TEST_F(GeneratorRecord, RealRangeValuesAndHotReusePreserveStageAndMir) {
    const auto off = run(program, CacheMode::Off);
    EXPECT_FALSE(fs::exists(root));
    const auto cold = run(program);
    const auto hot =
        run(program, CacheMode::ReadOnly, CacheEvidence::GeneratorReplay);
    EXPECT_EQ(off.stats, cold.stats);
    EXPECT_EQ(off.stats, hot.stats);
    EXPECT_EQ(off.text, cold.text);
    EXPECT_EQ(off.text, hot.text);
    EXPECT_EQ(cold.telemetry.writes, 1U);
    EXPECT_EQ(cold.telemetry.replay_writes, 1U);
    EXPECT_EQ(hot.telemetry.replay_hits, 1U);
    EXPECT_EQ(hot.telemetry.body_evaluations, 0U);
    std::optional<GeneratorReplayRecord> owned;
    {
        cache_test::Input input(program);
        auto record = load(input);
        ASSERT_TRUE(record);
        EXPECT_EQ(record->observation.first, 2U);
        EXPECT_EQ(record->observation.last, 6U);
        EXPECT_EQ(
            std::get<std::vector<IntegerValue>>(record->value.value),
            (std::vector<IntegerValue>{IntegerValue(15), IntegerValue(22),
                                       IntegerValue(29), IntegerValue(36)}));
        owned = std::move(*record);
    }
    ASSERT_TRUE(owned);
    for (std::uint32_t index = 0; index < 4; ++index) {
        auto replay = execute_generator_replay(*owned, index);
        ASSERT_TRUE(replay);
        EXPECT_EQ(replay->iteration_value, index + 2);
        EXPECT_EQ(replay->element_index, index);
        EXPECT_EQ(std::get<IntegerValue>(replay->value.value).bits(),
                  (index + 2) * 7 + 1);
        EXPECT_EQ(replay->stats.bound_nodes_evaluated, 2U);
        EXPECT_EQ(replay->stats.element_nodes_evaluated, 5U);
        EXPECT_EQ(replay->stats.fuel_consumed, 10U);
        EXPECT_EQ(replay->details.size(), 5U);
    }
}
TEST_F(GeneratorRecord,
       MissingLegacyCapabilityFallsBackAndExplicitRequirementUpgrades) {
    run(program);
    cache_test::Input input(program);
    fs::remove(manifest(input));
    auto unavailable = load(input);
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().code,
              ScalarReplayErrorCode::MissingDescriptor);
    const auto old = run(program, CacheMode::ReadOnly);
    EXPECT_EQ(old.telemetry.hits, 1U);
    EXPECT_FALSE(fs::exists(manifest(input)));
    const auto missing =
        run(program, CacheMode::ReadOnly, CacheEvidence::GeneratorReplay);
    EXPECT_EQ(missing.telemetry.hits, 0U);
    EXPECT_EQ(missing.telemetry.replay_rejected, 1U);
    EXPECT_EQ(old.stats, missing.stats);
    EXPECT_EQ(old.text, missing.text);
    const auto upgrade =
        run(program, CacheMode::ReadWrite, CacheEvidence::GeneratorReplay);
    EXPECT_EQ(upgrade.telemetry.writes, 0U);
    EXPECT_EQ(upgrade.telemetry.replay_writes, 1U);
    EXPECT_TRUE(load(input));
}
TEST_F(GeneratorRecord,
       EveryMissingOrCorruptRequiredObjectRejectsReadOnlyReuse) {
    run(program);
    cache_test::Input input(program);
    const auto candidate = cache::FileStore(root).lookup(
        generator_replay_key(key(input)), input.identity()->module_fingerprint);
    ASSERT_TRUE(candidate);
    ASSERT_EQ(candidate->artifacts.size(), 3U);
    for (const auto& artifact : candidate->artifacts) {
        const auto hash = artifact.object.digest.hex();
        const auto path = root / "objects" / hash.substr(0, 2) / hash;
        std::ifstream stream(path, std::ios::binary);
        const std::string original{std::istreambuf_iterator<char>(stream), {}};
        stream.close();
        for (bool corrupt : {false, true}) {
            if (corrupt) {
                auto bad = original;
                bad.back() ^= 1;
                std::ofstream(path, std::ios::binary) << bad;
            } else
                fs::remove(path);
            EXPECT_FALSE(load(input));
            const auto hot = run(program, CacheMode::ReadOnly,
                                 CacheEvidence::GeneratorReplay);
            EXPECT_EQ(hot.telemetry.hits, 0U);
            std::ofstream(path, std::ios::binary) << original;
        }
    }
    EXPECT_TRUE(load(input));
}
TEST_F(GeneratorRecord,
       ResealedSourceAndObservationBindingsCannotBypassValidation) {
    run(program);
    cache_test::Input input(program);
    auto record = load(input);
    ASSERT_TRUE(record);
    for (unsigned change = 0; change < 4; ++change) {
        auto descriptor = record->admission.descriptor();
        auto observation = record->observation;
        if (change == 0) {
            descriptor.element.nodes.back().operation =
                ScalarReplayOp::Subtract;
            observation.descriptor =
                *generator_replay_descriptor_fingerprint(descriptor);
        }
        if (change == 1)
            observation.descriptor.bytes[0] ^= 1;
        if (change == 2)
            ++observation.last;
        if (change == 3)
            observation.expected_result.bytes[0] ^= 1;
        reseal(input, descriptor, observation, record->value);
        EXPECT_FALSE(load(input)) << change;
    }
}
TEST_F(GeneratorRecord,
       ResealedWrongValuesOrRangesFailActualExecutionWithoutPartialDetails) {
    run(program);
    cache_test::Input input(program);
    auto record = load(input);
    ASSERT_TRUE(record);
    auto value = record->value;
    std::get<std::vector<IntegerValue>>(value.value)[2] = IntegerValue(999);
    auto observation = record->observation;
    observation.expected_result = *generator_replay_value_fingerprint(value);
    reseal(input, record->admission.descriptor(), observation, value);
    auto forged = load(input);
    ASSERT_TRUE(forged); // Valid source/binding, not yet verified computation.
    auto result = execute_generator_replay(*forged, 2);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, Code::ResultMismatch);
    EXPECT_EQ(result.error().stats.element_nodes_evaluated, 5U);
    // Verification is explicitly local to the selected element.
    EXPECT_TRUE(execute_generator_replay(*forged, 0));
    observation = record->observation;
    ++observation.first;
    ++observation.last;
    reseal(input, record->admission.descriptor(), observation, record->value);
    forged = load(input);
    ASSERT_TRUE(forged);
    result = execute_generator_replay(*forged, 0);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, Code::BoundsMismatch);
    EXPECT_EQ(result.error().stats.element_nodes_evaluated, 0U);
}
TEST_F(GeneratorRecord, FuelEveryBoundaryAndDetailSelectionRemainIndependent) {
    run(program);
    cache_test::Input input(program);
    auto record = load(input);
    ASSERT_TRUE(record);
    for (std::size_t fuel = 0; fuel < 10; ++fuel) {
        auto replay = execute_generator_replay(*record, 1, {.fuel = fuel});
        ASSERT_FALSE(replay) << fuel;
        EXPECT_EQ(replay.error().code, Code::FuelExhausted);
        EXPECT_EQ(replay.error().stats.fuel_consumed, fuel);
    }
    auto selected = execute_generator_replay(*record, 1, {.fuel = 10},
                                             {.first = 2, .count = 2});
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->details.size(), 2U);
    EXPECT_EQ(selected->details[0].node, 2U);
    EXPECT_EQ(std::get<IntegerValue>(selected->details[0].value).bits(), 21U);
    EXPECT_EQ(selected->details[1].node, 3U);
    auto none = execute_generator_replay(
        *record, 1,
        {.output_records = 0, .output_bytes = sizeof(VerifiedGeneratorReplay)},
        {.count = 0});
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->details.empty());
    EXPECT_EQ(none->stats.fuel_consumed, selected->stats.fuel_consumed);
    EXPECT_EQ(none->value, selected->value);
}
TEST_F(GeneratorRecord,
       MemoryOutputCodecSelectionAndCancellationRejectWithoutResults) {
    run(program);
    cache_test::Input input(program);
    auto record = load(input);
    ASSERT_TRUE(record);
    const std::size_t working = 5 * sizeof(ScalarConstant);
    auto result =
        execute_generator_replay(*record, 0, {.value_bytes = working - 1});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, Code::ValueMemoryLimit);
    EXPECT_EQ(result.error().stats.fuel_consumed, 0U);
    const auto exact =
        sizeof(VerifiedGeneratorReplay) + 5 * sizeof(ScalarReplayStep);
    EXPECT_TRUE(execute_generator_replay(
        *record, 0, {.value_bytes = working, .output_bytes = exact}));
    for (const auto limit :
         {ScalarReplayExecutionLimits{.output_records = 4},
          ScalarReplayExecutionLimits{.output_bytes = exact - 1}}) {
        result = execute_generator_replay(*record, 0, limit);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, Code::OutputLimit);
    }
    result = execute_generator_replay(*record, 0, {.codec = {.bytes = 1}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, Code::InvalidRecord);
    for (const auto range : {ScalarReplayDetailRange{.first = 6},
                             ScalarReplayDetailRange{.first = 4, .count = 2}}) {
        result = execute_generator_replay(*record, 0, {}, range);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, Code::InvalidSelection);
    }
    result = execute_generator_replay(*record, 4);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, Code::InvalidSelection);
    std::stop_source stop;
    stop.request_stop();
    result = execute_generator_replay(*record, 0, {}, {}, stop.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, Code::Cancelled);
    EXPECT_EQ(result.error().stats.fuel_consumed, 0U);
    EXPECT_TRUE(execute_generator_replay(*record, 0));
}
TEST_F(GeneratorRecord,
       BoolSignedTargetSizedConstantsAndShadowedIteratorsMatchRealStage) {
    for (const auto& source :
         {"fn table(x: u8) -> [u8; 4] { [for i in 2..6 { ~x + 2u8 }] } let a = "
          "table(7u8);",
          "fn table(x: i64) -> [i64; 4] { [for i in 2..6 { -x }] } let a = "
          "table(7i64);",
          "fn table(x: usize) -> [usize; 4] { [for i in 2..6 { x * 2usize }] } "
          "let a = table(7usize);",
          "fn table(x: bool) -> [bool; 4] { [for i in 2..6 { !x }] } let a = "
          "table(true);",
          "fn table(x: u32) -> [bool; 4] { [for i in 2..6 { i < x }] } let a = "
          "table(7);",
          "fn table(i: u32) -> [u32; 4] { [for i in i..i + 4 { i }] } let a = "
          "table(7);",
          "fn table() -> [u32; 4] { [for i in 2..6 { 42 }] } let a = table();",
          "fn table() -> [u32; 2] { [for i in 4294967293..4294967295 { i }] } "
          "let a = table();"}) {
        for (unsigned bits : {32U, 64U}) {
            fs::remove_all(root);
            run(source, CacheMode::ReadWrite, CacheEvidence::Value, {}, bits);
            cache_test::Input input(source, bits);
            auto record = load(input);
            ASSERT_TRUE(record) << source << ' ' << bits;
            for (std::uint32_t i = 0; i < record->value.type.length; ++i) {
                auto replay = execute_generator_replay(*record, i);
                ASSERT_TRUE(replay) << source << ' ' << i;
                EXPECT_EQ(replay->value.type,
                          record->value.type.element_type());
            }
        }
    }
}
TEST_F(GeneratorRecord,
       ArithmeticFailuresInBoundsAndElementAreAttributedAndBounded) {
    // Deliberately assembled records for source-admitted but unsuccessful
    // expressions. Real Stage publication must never emit these records.
    for (unsigned which = 0; which < 3; ++which) {
        const auto begin = which == 0 ? "(1 / 0)" : "0";
        const auto end = which == 1 ? "(1 << 32)" : "2";
        const auto element = which == 2 ? "(i / 0)" : "i";
        cache_test::Input input(
            std::string("fn table() -> [u32; 2] { [for i in ") + begin + ".." +
            end + " { " + element + " }] }");
        auto admission = admit_generator_replay(
            *input.module, input.checker.types(), key(input));
        ASSERT_TRUE(admission);
        cache::TypedValue value{
            sema::Type::array(sema::Type::integer(32), 2),
            std::vector<IntegerValue>{IntegerValue(0), IntegerValue(1)}};
        GeneratorReplayRecord record{
            *admission,
            {.descriptor = *generator_replay_descriptor_fingerprint(
                 admission->descriptor()),
             .first = 0,
             .last = 2,
             .expected_result = *generator_replay_value_fingerprint(value)},
            value,
            {}};
        auto replay = execute_generator_replay(record, 0);
        ASSERT_FALSE(replay);
        EXPECT_EQ(replay.error().code, Code::ArithmeticFailure);
        EXPECT_EQ(replay.error().phase, which == 0 ? GeneratorReplayPhase::Begin
                                        : which == 1
                                            ? GeneratorReplayPhase::End
                                            : GeneratorReplayPhase::Element);
        EXPECT_TRUE(replay.error().integer);
    }
}
TEST_F(GeneratorRecord,
       StageFailuresNeverPublishGeneratorCapabilityAndWarmBudgetsStayLogical) {
    const auto baseline = run(program);
    cache_test::Input input(program);
    for (std::size_t fuel = 0; fuel <= baseline.stats[0]; ++fuel) {
        const auto off =
            run(program, CacheMode::Off, CacheEvidence::Value, {.fuel = fuel});
        const auto hot = run(program, CacheMode::ReadOnly,
                             CacheEvidence::GeneratorReplay, {.fuel = fuel});
        EXPECT_EQ(off.stats, hot.stats) << fuel;
        EXPECT_EQ(off.text, hot.text) << fuel;
    }
    for (const auto limits :
         {AnalysisLimits{.fuel = 1}, AnalysisLimits{.array_elements = 3},
          AnalysisLimits{.array_bytes = 15}}) {
        fs::remove_all(root);
        auto failed =
            run(program, CacheMode::ReadWrite, CacheEvidence::Value, limits);
        EXPECT_EQ(failed.telemetry.replay_writes, 0U);
        EXPECT_FALSE(load(input));
    }
    for (const auto& text :
         {"fn table() -> [u32; 2] { [for i in 0..2 { i / 0 }] } let a = "
          "table();",
          "fn table() -> [u32; 2] { [for i in 1..1 { i }] } let a = table();",
          "fn table(x: u32) -> [u32; 2] { [for i in 0..x { i }] } let a = "
          "table(7);"}) {
        fs::remove_all(root);
        const auto failed = run(text);
        EXPECT_EQ(failed.telemetry.replay_writes, 0U);
        cache_test::Input source(text);
        EXPECT_FALSE(load(source));
    }
}
TEST_F(
    GeneratorRecord,
    UnsupportedArraysDoNotAcquireCapabilityAndSpillFailureBlocksPublication) {
    for (const auto& text :
         {"fn table(x: u32) -> [u32; 2] { [x, x] } let a = table(7);",
          "fn table(x: u32) -> [u32; 2] { [for i in 0..2 { i as u32 }] } let a "
          "= table(7);",
          "extern fn input() -> u32; fn table() -> [u32; 2] { [for i in 0..2 { "
          "input() }] } let a = table();"}) {
        fs::remove_all(root);
        auto off = run(text, CacheMode::Off);
        auto cold = run(text);
        EXPECT_EQ(cold.telemetry.replay_writes, 0U);
        EXPECT_EQ(off.stats, cold.stats);
        EXPECT_EQ(off.text, cold.text);
        cache_test::Input input(text);
        EXPECT_FALSE(load(input));
    }
    fs::remove_all(root);
    provenance::EvidenceBuildConfig spill;
    spill.mode = provenance::EvidenceBuildMode::Spill;
    spill.directory = root / "evidence";
    const auto text = program + " comptime let bad = 1 / 0;";
    const auto failed =
        run(text, CacheMode::ReadWrite, CacheEvidence::Value, {}, 32, spill);
    EXPECT_EQ(failed.telemetry.replay_writes, 0U);
    EXPECT_GT(failed.telemetry.blocked_writes, 0U);
    cache_test::Input input(text);
    EXPECT_FALSE(load(input));
}
TEST_F(GeneratorRecord,
       SourceWhitespaceInputsTargetCompilerAndDependencyBindingsAreCurrent) {
    run(program);
    cache_test::Input shifted("// relocated\n" + program);
    EXPECT_TRUE(load(shifted));
    for (const auto& source :
         {program + " fn other() -> u32 { 0 }",
          std::string("fn table(x: u32) -> [u32; 4] { [for i in 2..6 { i * x - "
                      "1 }] } let a = table(7); let b = table(7);")}) {
        cache_test::Input changed(source);
        EXPECT_FALSE(load(changed));
    }
    cache_test::Input input(program);
    auto changed = key(input);
    changed.arguments[0].value = IntegerValue(8);
    EXPECT_FALSE(load_generator_replay(cache::FileStore(root), *input.module,
                                       input.checker.types(), changed));
    changed = key(input);
    changed.context.compiler_build = "other";
    EXPECT_FALSE(load_generator_replay(cache::FileStore(root), *input.module,
                                       input.checker.types(), changed));
    cache_test::Input target(program, 64);
    EXPECT_FALSE(load(target));
}
TEST_F(GeneratorRecord,
       LargeGeneratorExecutesOneElementAndReusesFixedWorkingBuffer) {
    const std::string source = "fn table(x: u32) -> [u32; 32768] { [for i in "
                               "100..32868 { i + x }] } let a = table(7);";
    run(source);
    cache_test::Input input(source);
    auto record = load(input);
    ASSERT_TRUE(record);
    const auto first = execute_generator_replay(*record, 0);
    const auto last = execute_generator_replay(*record, 32767);
    ASSERT_TRUE(first);
    ASSERT_TRUE(last);
    EXPECT_EQ(first->stats, last->stats);
    EXPECT_EQ(first->stats.element_nodes_evaluated, 3U);
    EXPECT_EQ(first->stats.fuel_consumed, 8U);
    EXPECT_EQ(first->stats.value_bytes, 3 * sizeof(ScalarConstant));
    EXPECT_EQ(std::get<IntegerValue>(last->value.value).bits(), 32874U);
}
TEST_F(GeneratorRecord,
       ObservationCodecAndNewCapabilityKindRejectMalformedOrRuntimeData) {
    run(program);
    cache_test::Input input(program);
    auto record = load(input);
    ASSERT_TRUE(record);
    const auto bytes = encode_generator_replay_observation(record->observation);
    ASSERT_TRUE(bytes);
    EXPECT_EQ((*bytes)[8], 17U);
    EXPECT_EQ(decode_generator_replay_observation(*bytes), record->observation);
    for (std::size_t size = 0; size < bytes->size(); ++size)
        EXPECT_FALSE(
            decode_generator_replay_observation(std::span(*bytes).first(size)));
    for (auto offset : {0U, 4U, 8U, 9U}) {
        auto bad = *bytes;
        bad[offset] = 255;
        EXPECT_FALSE(decode_generator_replay_observation(bad));
    }
    auto bad = *bytes;
    bad.push_back(0);
    EXPECT_FALSE(decode_generator_replay_observation(bad));
    auto observation = record->observation;
    observation.last = observation.first;
    EXPECT_FALSE(encode_generator_replay_observation(observation));
    EXPECT_FALSE(decode_generator_replay_observation(
        *bytes, {.bytes = bytes->size() - 1}));
    auto pure = key(input);
    auto capability = generator_replay_key(pure);
    auto encoded = cache::encode_query_key(capability);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(cache::decode_query_key(*encoded), capability);
    EXPECT_EQ((*encoded)[9], 7U);
    capability.arguments[0].stage = cache::ArgumentStage::Runtime;
    capability.arguments[0].value.reset();
    EXPECT_FALSE(cache::encode_query_key(capability));
}
} // namespace
