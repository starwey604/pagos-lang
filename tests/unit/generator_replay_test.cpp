#include "../../lib/cache/wire.h"
#include "cache_test_input.h"
#include "pagos/cache/store.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/generator_replay.h"
#include "pagos/stage/stage_analyzer.h"

#include <array>
#include <atomic>
#include <sstream>
#include <type_traits>

namespace {
using namespace pagos;
using namespace pagos::stage;
using Code = ReplayErrorCode;
using Op = ScalarReplayOp;
static_assert(!std::is_default_constructible_v<GeneratorReplayAdmission>);
static_assert(!std::is_constructible_v<GeneratorReplayAdmission,
                                       GeneratorReplayDescriptor>);
const std::string program =
    "fn table(start: u32, end: u32, scale: u32, unused: bool) -> [u32; 4] { "
    "[for i in start..end { i * scale + 1 }] }";
cache::QueryKey key(const cache_test::Input& input,
                    std::vector<cache::StageArgument> arguments = {}) {
    const auto identity = input.identity();
    const auto* definition =
        identity->find(cache::DefinitionKind::Function, "table");
    if (!definition)
        throw std::runtime_error("missing table");
    if (arguments.empty())
        for (const auto& type : definition->parameters) {
            Constant value = true;
            if (type.kind == sema::TypeKind::Integer)
                value = *IntegerValue::create(type.integer_type, 7);
            if (type.is_array())
                value = std::vector<IntegerValue>(type.length, IntegerValue(7));
            arguments.push_back({type, cache::ArgumentStage::Comptime, value});
        }
    auto result = make_query_key(
        *definition, cache_test::context(input.checker.types().pointer_bits),
        std::move(arguments));
    if (!result)
        throw std::runtime_error("invalid table key");
    return *result;
}
auto admit(const cache_test::Input& input) {
    return admit_generator_replay(*input.module, input.checker.types(),
                                  key(input));
}
auto validate(const GeneratorReplayDescriptor& descriptor,
              const cache_test::Input& input) {
    return validate_generator_replay(&descriptor, *input.module,
                                     input.checker.types(), key(input));
}
TEST(GeneratorReplay, SourceAdmissionOwnsBoundsIteratorAndAllCaptures) {
    std::optional<GeneratorReplayAdmission> owned;
    {
        cache_test::Input input(program);
        auto admitted = admit(input);
        ASSERT_TRUE(admitted);
        const auto& d = admitted->descriptor();
        EXPECT_EQ(d.specialization.arguments.size(), 4U);
        EXPECT_EQ(d.iterator_slot, 4U);
        EXPECT_EQ(d.begin.nodes.size(), 1U);
        EXPECT_EQ(d.begin.nodes[0].input, 0U);
        EXPECT_EQ(d.end.nodes[0].input, 1U);
        EXPECT_EQ(d.element.nodes[0].input, 4U);
        EXPECT_EQ(d.element.nodes[1].input, 2U);
        EXPECT_EQ(d.element.nodes.back().operation, Op::Add);
        EXPECT_EQ(d.result_type, sema::Type::array(sema::Type::integer(32), 4));
        EXPECT_EQ(d.verified_inputs, input.identity()->module_fingerprint);
        EXPECT_NE(d.generator_site, d.iterator_site);
        EXPECT_EQ(input.source.text().substr(
                      input.identity()->resolve(d.iterator_site)->begin, 1),
                  "i");
        EXPECT_TRUE(validate(d, input));
        owned = std::move(*admitted);
    }
    ASSERT_TRUE(owned);
    auto bytes = encode_generator_replay_descriptor(owned->descriptor());
    ASSERT_TRUE(bytes);
    EXPECT_EQ((*bytes)[8], 16U);
    auto decoded = decode_generator_replay_descriptor(*bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, owned->descriptor());
    EXPECT_EQ(encode_generator_replay_descriptor(*decoded), bytes);
    EXPECT_EQ(generator_replay_descriptor_fingerprint(*decoded),
              generator_replay_descriptor_fingerprint(owned->descriptor()));
    EXPECT_FALSE(decode_replay_descriptor(*bytes));
}
TEST(GeneratorReplay, IteratorShadowsParameterOnlyInsideElement) {
    cache_test::Input input(
        "fn table(i: u32) -> [u32; 2] { [for i in i..i + 2 { i }] }");
    auto admitted = admit(input);
    ASSERT_TRUE(admitted);
    const auto& d = admitted->descriptor();
    EXPECT_EQ(d.begin.nodes[0].input, 0U);
    EXPECT_EQ(d.end.nodes[0].input, 0U);
    EXPECT_EQ(d.element.nodes[0].input, 1U);
    auto forged = d;
    forged.element.nodes[0].input = 0;
    ASSERT_TRUE(encode_generator_replay_descriptor(forged));
    EXPECT_EQ(validate(forged, input).error().code, Code::BindingMismatch);
}
TEST(GeneratorReplay, BoolSignedWidthsUsizeAndUnusedIteratorAreClosed) {
    for (const auto& source :
         {"fn table(x: u8) -> [u8; 2] { [for i in 0..2 { ~x + 2u8 }] }",
          "fn table(x: i64) -> [i64; 2] { [for i in 0..2 { -x }] }",
          "fn table(x: usize) -> [usize; 2] { [for i in 0..2 { x * 2usize }] }",
          "fn table(x: bool) -> [bool; 2] { [for i in 0..2 { !x }] }",
          "fn table(x: u32) -> [bool; 2] { [for i in 0..2 { i < x }] }",
          "fn table() -> [u32; 2] { [for i in 0..2 { 42 }] }"})
        for (unsigned bits : {32U, 64U}) {
            cache_test::Input input(source, bits);
            auto admitted = admit(input);
            ASSERT_TRUE(admitted) << source << ' ' << bits;
            auto bytes =
                encode_generator_replay_descriptor(admitted->descriptor());
            ASSERT_TRUE(bytes);
            EXPECT_EQ(decode_generator_replay_descriptor(*bytes),
                      admitted->descriptor());
        }
}
TEST(GeneratorReplay,
     EffectsStateCallsBranchesCastsAndUnverifiedNamesFailClosed) {
    for (const auto& source :
         {"fn table(x: u32) -> [u32; 2] { let y = x; [for i in 0..2 { y }] }",
          "fn table(x: u32) -> [u32; 2] { [for i in 0..2 { let y = x; y }] }",
          "fn table(x: u32) -> [u32; 2] { [for i in 0..2 { "
          "volatile *(4096usize as *mut u32) = i; x }] }",
          "fn table(x: u32) -> [u32; 2] { [for i in 0..2 { i as u32 }] }",
          "fn table(x: bool) -> [bool; 2] { [for i in 0..2 { x && true }] }",
          "fn table(x: u32) -> [u32; 2] { "
          "[for i in 0..2 { if i == 0 { x } else { i } }] }",
          "fn helper(x: u32) -> u32 { x } fn table(x: u32) -> [u32; 2] { "
          "[for i in 0..2 { helper(i) }] }",
          "extern fn input() -> u32; fn table() -> [u32; 2] { "
          "[for i in 0..2 { input() }] }",
          "extern fn input() -> u32; fn table() -> [u32; 2] { "
          "[for i in 0..input() { i }] }",
          "static mut state: u32 = 1; fn table() -> [u32; 2] { "
          "[for i in 0..2 { state + i }] }",
          "fn table() -> [u32; 2] { [for i in 0..2 { [0, 1][i] }] }",
          "fn table() -> [u32; 2] { "
          "[for i in 0..2 { [for j in 0..2 { j }][i] }] }",
          "fn table() -> [u32; 2] { [0, 1] }",
          "fn table(a: [u32; 2]) -> [u32; 2] { [for i in 0..2 { i }] }"}) {
        cache_test::Input input(source);
        EXPECT_FALSE(admit(input)) << source;
    }
}
TEST(GeneratorReplay, RuntimeCapturesAndWrongTargetSignatureOrKeyAreRejected) {
    cache_test::Input input(program);
    for (unsigned change = 0; change < 7; ++change) {
        auto current = key(input);
        switch (change) {
        case 0:
            current.arguments[0].stage = cache::ArgumentStage::Runtime;
            current.arguments[0].value.reset();
            break;
        case 1:
            current.arguments.pop_back();
            break;
        case 2:
            current.arguments[0].type = sema::Type::integer(8);
            current.arguments[0].value = *IntegerValue::create({8, false}, 7);
            break;
        case 3:
            current.context.target = cache_test::context(64).target;
            break;
        case 4:
            current.definition_fingerprint.bytes[0] ^= 1;
            break;
        case 5:
            current.kind = cache::QueryKind::SpecializeResidual;
            break;
        case 6:
            current.definition.name = "missing";
            break;
        }
        EXPECT_FALSE(admit_generator_replay(*input.module,
                                            input.checker.types(), current))
            << change;
    }
}
TEST(GeneratorReplay, CompleteComparisonRejectsStructurallyValidForgeries) {
    cache_test::Input input(program);
    auto admitted = admit(input);
    ASSERT_TRUE(admitted);
    for (unsigned change = 0; change < 11; ++change) {
        auto d = admitted->descriptor();
        switch (change) {
        case 0:
            d.element.nodes.back().operation = Op::Subtract;
            break;
        case 1:
            d.element.nodes[0].input = 0;
            break;
        case 2:
            d.element.nodes[0].position = d.element.nodes[1].position;
            break;
        case 3:
            d.element.nodes[3].constant = IntegerValue(2);
            break;
        case 4:
            std::swap(d.begin, d.end);
            break;
        case 5:
            d.verified_inputs.bytes[0] ^= 1;
            break;
        case 6:
            d.specialization.arguments.back().value = false;
            break;
        case 7:
            ++d.result_type.length;
            break;
        case 8:
            std::swap(d.generator_site, d.iterator_site);
            break;
        case 9:
            d.element.nodes[0].position = UINT32_MAX;
            break;
        case 10:
            d.specialization.context.compiler_build = "other";
            break;
        }
        auto bytes = encode_generator_replay_descriptor(d);
        ASSERT_TRUE(bytes) << change;
        auto decoded = decode_generator_replay_descriptor(*bytes);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(validate(*decoded, input).error().code, Code::BindingMismatch)
            << change;
    }
}
TEST(GeneratorReplay, WhitespaceRebindsButChangedSourceAndDependenciesDoNot) {
    cache_test::Input input(program);
    auto admitted = admit(input);
    ASSERT_TRUE(admitted);
    cache_test::Input shifted("// relocated\n\n" + program);
    auto rebound = validate(admitted->descriptor(), shifted);
    ASSERT_TRUE(rebound);
    EXPECT_NE(
        input.identity()->resolve(rebound->descriptor().iterator_site)->begin,
        shifted.identity()
            ->resolve(rebound->descriptor().iterator_site)
            ->begin);
    for (const auto& text :
         {program + " fn unrelated() -> u32 { 0 }",
          std::string(
              "fn table(start: u32, end: u32, scale: u32, unused: bool) "
              "-> [u32; 4] { [for i in start..end { i * scale - 1 }] }")}) {
        cache_test::Input changed(text);
        EXPECT_FALSE(validate(admitted->descriptor(), changed));
    }
    auto current = key(input);
    current.arguments.back().value = false;
    EXPECT_FALSE(validate_generator_replay(&admitted->descriptor(),
                                           *input.module, input.checker.types(),
                                           current));
    EXPECT_EQ(validate_generator_replay(nullptr, *input.module,
                                        input.checker.types(), key(input))
                  .error()
                  .code,
              Code::MissingDescriptor);
    auto value = cache::encode_value(
        {sema::Type::array(sema::Type::integer(32), 2),
         std::vector<IntegerValue>{IntegerValue(1), IntegerValue(2)}});
    ASSERT_TRUE(value);
    EXPECT_FALSE(decode_generator_replay_descriptor(*value));
}
TEST(GeneratorReplay, CodecRejectsIteratorScopeCyclesGarbageAndTypeConfusion) {
    cache_test::Input input(program);
    auto admitted = admit(input);
    ASSERT_TRUE(admitted);
    for (unsigned change = 0; change < 13; ++change) {
        auto d = admitted->descriptor();
        switch (change) {
        case 0:
            d.iterator_slot = 0;
            break;
        case 1:
            d.begin.nodes[0].input = d.iterator_slot;
            break;
        case 2:
            d.element.nodes[0].input = d.iterator_slot + 1;
            break;
        case 3:
            d.element.nodes.back().operands[0] = 4;
            break;
        case 4:
            d.element.nodes.push_back(d.element.nodes[0]);
            break;
        case 5:
            d.element.nodes[0].constant = IntegerValue(1);
            break;
        case 6:
            d.element.nodes[0].type = sema::Type::integer(64);
            break;
        case 7:
            d.element.nodes.back().operation = static_cast<Op>(255);
            break;
        case 8:
            d.iterator_site.definition.name = "other";
            break;
        case 9:
            d.result_type.record_name = "noncanonical";
            break;
        case 10:
            d.begin.nodes[0].type = d.begin.result_type = sema::Type::usize(32);
            break;
        case 11:
            d.result_type.length = 0;
            break;
        case 12:
            d.element.nodes.clear();
            break;
        }
        EXPECT_FALSE(encode_generator_replay_descriptor(d)) << change;
    }
}
TEST(GeneratorReplay, VersionsTruncationTrailingBytesAndLengthsAreBounded) {
    cache_test::Input input(program);
    auto admitted = admit(input);
    ASSERT_TRUE(admitted);
    const auto original =
        *encode_generator_replay_descriptor(admitted->descriptor());
    for (std::size_t size = 0; size < original.size(); ++size)
        EXPECT_FALSE(
            decode_generator_replay_descriptor(std::span(original).first(size)))
            << size;
    for (const auto offset : {0U, 4U, 8U, 9U, 13U, 17U, 21U}) {
        auto bytes = original;
        bytes[offset] = 255;
        EXPECT_FALSE(decode_generator_replay_descriptor(bytes)) << offset;
    }
    auto bytes = original;
    std::fill_n(bytes.begin() + 21, 8, 255);
    EXPECT_EQ(decode_generator_replay_descriptor(bytes).error().code,
              Code::LimitExceeded);
    bytes = original;
    bytes.push_back(0);
    EXPECT_FALSE(decode_generator_replay_descriptor(bytes));
    for (unsigned change = 0; change < 5; ++change) {
        auto d = admitted->descriptor();
        if (change == 0)
            ++d.version;
        if (change == 1)
            ++d.semantics;
        if (change == 2)
            ++d.scalar_semantics;
        if (change == 3)
            ++d.specialization.context.query_version;
        if (change == 4)
            ++d.specialization.context.budget_version;
        EXPECT_EQ(encode_generator_replay_descriptor(d).error().code,
                  Code::IncompatibleVersion);
    }
}
TEST(GeneratorReplay, AggregateNodeQuotaAndSourceDepthFailWithoutPartialProof) {
    cache_test::Input input(program);
    auto admitted = admit(input);
    ASSERT_TRUE(admitted);
    const auto& d = admitted->descriptor();
    const auto bytes = *encode_generator_replay_descriptor(d);
    // 1 start + 1 end + 5 element nodes; 6 is enough for each separately.
    EXPECT_TRUE(encode_generator_replay_descriptor(d, {.elements = 7}));
    EXPECT_TRUE(decode_generator_replay_descriptor(bytes, {.elements = 7}));
    for (const auto limits : {cache::Limits{.elements = 6},
                              cache::Limits{.bytes = bytes.size() - 1},
                              cache::Limits{.string_bytes = 2}}) {
        EXPECT_FALSE(encode_generator_replay_descriptor(d, limits));
        EXPECT_FALSE(decode_generator_replay_descriptor(bytes, limits));
        EXPECT_FALSE(validate_generator_replay(
            &d, *input.module, input.checker.types(), key(input), limits));
    }
    EXPECT_FALSE(admit_generator_replay(*input.module, input.checker.types(),
                                        key(input), {.depth = 1}));
    EXPECT_FALSE(admit_generator_replay(*input.module, input.checker.types(),
                                        key(input), {.elements = 1}));
    auto missing = input.checker.types();
    missing.expressions.erase(static_cast<const syntax::ArrayGeneratorExpr&>(
                                  *input.module->functions[0]->body->tail)
                                  .body->tail.get());
    EXPECT_FALSE(admit_generator_replay(*input.module, missing, key(input)));
}
TEST(GeneratorReplay, IndependenceDoesNotAssertSuccessfulBoundsOrEvaluation) {
    for (const auto& source :
         {"fn table() -> [u32; 2] { [for i in 1..1 { i }] }",
          "fn table() -> [u32; 2] { [for i in 0..(1 / 0) { i }] }",
          "fn table() -> [u32; 2] { "
          "[for i in 0..(4294967294 + 1) { 1 / 0 }] }",
          "fn table() -> [u32; 2] { [for i in 0..2 { i << 32 }] }"}) {
        SCOPED_TRACE(source);
        cache_test::Input input(source);
        EXPECT_TRUE(admit(input)) << source;
    }
}
TEST(GeneratorReplay, ExplicitCasRoundTripDoesNotGrantSourceAdmission) {
    cache_test::Input input(program);
    auto admitted = admit(input);
    ASSERT_TRUE(admitted);
    static std::atomic<unsigned> sequence{};
    const auto root =
        std::filesystem::temp_directory_path() /
        ("pagos-generator-replay-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(sequence++));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root};
    auto forged = admitted->descriptor();
    forged.element.nodes.back().operation = Op::Subtract;
    const auto bytes = encode_generator_replay_descriptor(forged);
    ASSERT_TRUE(bytes);
    const auto object = cache::FileStore(root).write_object(
        cache::ObjectKind::GeneratorReplayDescriptor, *bytes);
    ASSERT_TRUE(object);
    const auto loaded = cache::FileStore(root).read_object(*object);
    ASSERT_TRUE(loaded);
    const auto decoded = decode_generator_replay_descriptor(*loaded);
    ASSERT_TRUE(decoded);
    EXPECT_FALSE(validate(*decoded, input));
    EXPECT_FALSE(std::filesystem::exists(root / "queries"));
}
TEST(GeneratorReplay, AdmissionPreservesStageFuelConstructionAndResidualMir) {
    struct Outcome {
        std::array<std::size_t, 11> stats;
        std::string text;
        bool operator==(const Outcome&) const = default;
    };
    const auto run = [](bool admission, AnalysisLimits limits) {
        cache_test::Input input(
            "fn table(x: u32) -> [u32; 4] { [for i in 2..6 { i * x }] } "
            "extern fn output(x: u32) -> u32; let a = table(7); "
            "let b = table(7); output(a[2]);");
        if (admission) {
            EXPECT_TRUE(admit(input));
        }
        StageAnalyzer analyzer(input.diagnostics, input.checker.types(),
                               limits);
        const auto module = analyzer.analyze(*input.module);
        std::ostringstream text;
        hir::print(*module, text);
        input.diagnostics.render(text);
        if (!input.diagnostics.has_error()) {
            const auto lowered = mir::lower(*module);
            EXPECT_TRUE(lowered);
            if (lowered)
                mir::print(*lowered, text);
        }
        const auto& s = analyzer.stats();
        return Outcome{{s.fuel_consumed, s.specializations, s.cache_hits,
                        s.maximum_recursion_depth, s.array_elements_reserved,
                        s.array_bytes_reserved, s.aggregate_constructions,
                        s.aggregate_members_reserved,
                        s.aggregate_bytes_reserved, s.residual_specializations,
                        s.residual_cache_hits},
                       text.str()};
    };
    for (const auto limits : {AnalysisLimits{}, AnalysisLimits{.fuel = 1},
                              AnalysisLimits{.array_elements = 3}})
        EXPECT_EQ(run(false, limits), run(true, limits));
}
} // namespace
