#include "cache_test_input.h"
#include "pagos/stage/scalar_replay_execution.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/vm/evaluator.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <sstream>

namespace {
using namespace pagos;
using namespace pagos::stage;
using Op = ScalarReplayOp;
using Code = ScalarReplayExecutionErrorCode;
ScalarReplayNode literal(sema::Type type, Constant value) {
    return {.operation = Op::Constant,
            .type = std::move(type),
            .constant = std::move(value)};
}
ScalarReplayRecord record(std::vector<ScalarReplayNode> nodes,
                          cache::TypedValue expected) {
    ReplayDescriptor descriptor;
    descriptor.specialization = cache_test::key();
    descriptor.verified_inputs =
        cache::fingerprint("test.execution-inputs", {});
    for (std::size_t i = 0; i < nodes.size(); ++i)
        nodes[i].position = static_cast<std::uint32_t>(i);
    descriptor.result_site = {descriptor.specialization.definition,
                              descriptor.specialization.definition_fingerprint,
                              static_cast<std::uint32_t>(nodes.size() - 1)};
    descriptor.result_type = expected.type;
    descriptor.expected_result = *replay_value_fingerprint(expected);
    descriptor.nodes = std::move(nodes);
    return {std::move(descriptor), std::move(expected), {}};
}
ScalarReplayRecord binary(Op op, sema::Type type, Constant a, Constant b,
                          cache::TypedValue expected) {
    return record(
        {literal(type, std::move(a)),
         literal(type, std::move(b)),
         {.operation = op, .type = expected.type, .operands = {0, 1}}},
        expected);
}
ScalarReplayRecord square() {
    const auto type = sema::Type::integer(32);
    return record(
        {{.operation = Op::Input, .type = type, .input = 0},
         {.operation = Op::Multiply, .type = type, .operands = {0, 0}}},
        {type, IntegerValue(49)});
}
TEST(ScalarReplayExecution,
     FuelChargesEveryNodeAndFinalVerificationWithoutPartialResults) {
    const auto input = square();
    for (std::size_t fuel = 0; fuel <= 4; ++fuel) {
        const auto output = execute_scalar_replay(input, {.fuel = fuel});
        if (fuel < 3) {
            ASSERT_FALSE(output);
            EXPECT_EQ(output.error().code, Code::FuelExhausted);
            EXPECT_EQ(output.error().stats.fuel_consumed, fuel);
            EXPECT_EQ(output.error().stats.nodes_evaluated,
                      std::min(fuel, std::size_t{2}));
            EXPECT_EQ(output.error().node,
                      fuel < 2 ? std::optional<std::uint32_t>(
                                     static_cast<std::uint32_t>(fuel))
                               : std::nullopt);
        } else {
            ASSERT_TRUE(output);
            EXPECT_EQ(output->stats.nodes_evaluated, 2U);
            EXPECT_EQ(output->stats.fuel_consumed, 3U);
            EXPECT_EQ(output->value, input.value);
            EXPECT_EQ(output->descriptor,
                      *replay_descriptor_fingerprint(input.descriptor));
        }
    }
}
TEST(ScalarReplayExecution,
     DetailWindowNeverSkipsDependenciesAndCanVerifyWithoutDetails) {
    const auto input = square();
    auto output = execute_scalar_replay(input, {}, {.first = 1, .count = 1});
    ASSERT_TRUE(output);
    ASSERT_EQ(output->details.size(), 1U);
    EXPECT_EQ(output->details[0].node, 1U);
    EXPECT_EQ(std::get<IntegerValue>(output->details[0].value).bits(), 49U);
    EXPECT_EQ(output->stats.nodes_evaluated, 2U);
    EXPECT_EQ(output->stats.fuel_consumed, 3U);
    output = execute_scalar_replay(
        input,
        {.output_records = 0, .output_bytes = sizeof(VerifiedScalarReplay)},
        {.first = 2, .count = 0});
    ASSERT_TRUE(output);
    EXPECT_TRUE(output->details.empty());
    EXPECT_EQ(output->stats.output_bytes, sizeof(VerifiedScalarReplay));
    EXPECT_EQ(output->stats.nodes_evaluated, 2U);
    for (const auto range :
         {ScalarReplayDetailRange{3, 0}, ScalarReplayDetailRange{1, 2},
          ScalarReplayDetailRange{0, SIZE_MAX},
          ScalarReplayDetailRange{SIZE_MAX, 0}}) {
        const auto invalid = execute_scalar_replay(input, {}, range);
        ASSERT_FALSE(invalid);
        EXPECT_EQ(invalid.error().code, Code::InvalidSelection);
        EXPECT_EQ(invalid.error().stats.fuel_consumed, 0U);
    }
}
TEST(ScalarReplayExecution,
     ValueAndOutputPayloadLimitsAreIndependentAndCheckedBeforeWork) {
    const auto input = square();
    const auto values = input.descriptor.nodes.size() * sizeof(ScalarConstant);
    const auto bytes =
        sizeof(VerifiedScalarReplay) + 2 * sizeof(ScalarReplayStep);
    const auto exact = execute_scalar_replay(
        input,
        {.value_bytes = values, .output_records = 2, .output_bytes = bytes});
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact->stats.value_bytes, values);
    EXPECT_LE(exact->stats.output_bytes, bytes);
    auto limited = execute_scalar_replay(input, {.value_bytes = values - 1});
    ASSERT_FALSE(limited);
    EXPECT_EQ(limited.error().code, Code::ValueMemoryLimit);
    EXPECT_EQ(limited.error().stats.nodes_evaluated, 0U);
    for (const auto limits :
         {ScalarReplayExecutionLimits{.output_records = 1},
          ScalarReplayExecutionLimits{.output_bytes = bytes - 1},
          ScalarReplayExecutionLimits{.output_bytes =
                                          sizeof(VerifiedScalarReplay) - 1}}) {
        limited = execute_scalar_replay(input, limits);
        ASSERT_FALSE(limited);
        EXPECT_EQ(limited.error().code, Code::OutputLimit);
        EXPECT_EQ(limited.error().stats.fuel_consumed, 0U);
        EXPECT_EQ(limited.error().stats.value_bytes, 0U);
    }
    const auto no_trace =
        execute_scalar_replay(input, {.output_records = 0}, {.count = 0});
    ASSERT_TRUE(no_trace);
    EXPECT_EQ(no_trace->value, exact->value);
}
TEST(ScalarReplayExecution,
     ForgedRecordedValueWithMatchingFingerprintStillFailsActualMath) {
    auto input = square();
    input.value.value = IntegerValue(50);
    input.descriptor.expected_result = *replay_value_fingerprint(input.value);
    ASSERT_TRUE(encode_replay_descriptor(input.descriptor));
    const auto original = *encode_replay_descriptor(input.descriptor);
    const auto output = execute_scalar_replay(input);
    ASSERT_FALSE(output);
    EXPECT_EQ(output.error().code, Code::ResultMismatch);
    EXPECT_EQ(output.error().stats.nodes_evaluated, 2U);
    EXPECT_EQ(output.error().stats.fuel_consumed, 3U);
    EXPECT_EQ(*encode_replay_descriptor(input.descriptor), original);
    EXPECT_EQ(std::get<IntegerValue>(input.value.value).bits(), 50U);
}
TEST(ScalarReplayExecution,
     InvalidShapesAndRecordedValuesNeverReachOperandAccess) {
    for (unsigned change = 0; change < 8; ++change) {
        auto input = square();
        switch (change) {
        case 0:
            input.descriptor.nodes[1].operands[0] = 1;
            break;
        case 1:
            input.descriptor.nodes[0].input = UINT32_MAX;
            break;
        case 2:
            input.descriptor.nodes[1].operation = static_cast<Op>(255);
            break;
        case 3:
            input.descriptor.nodes[1].type = sema::TypeKind::Bool;
            break;
        case 4:
            input.descriptor.nodes.clear();
            break;
        case 5:
            ++input.descriptor.semantics;
            break;
        case 6:
            input.value.value = IntegerValue(50);
            break;
        case 7:
            input.value = {sema::TypeKind::Bool, true};
            break;
        }
        const auto output = execute_scalar_replay(input);
        ASSERT_FALSE(output) << change;
        EXPECT_EQ(output.error().code, change < 6 ? Code::InvalidDescriptor
                                                  : Code::InvalidRecordedValue);
        EXPECT_EQ(output.error().stats.fuel_consumed, 0U);
        EXPECT_EQ(output.error().stats.value_bytes, 0U);
    }
    const auto small_codec =
        execute_scalar_replay(square(), {.codec = {.bytes = 1}});
    ASSERT_FALSE(small_codec);
    ASSERT_TRUE(small_codec.error().descriptor);
    EXPECT_EQ(small_codec.error().descriptor->code,
              ReplayErrorCode::LimitExceeded);
}
TEST(ScalarReplayExecution,
     CancellationPrecedesAdmissionAndRestartHasFreshIndependentBudgets) {
    std::stop_source stop;
    EXPECT_TRUE(stop.request_stop());
    const auto cancelled = execute_scalar_replay({}, {}, {}, stop.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, Code::Cancelled);
    EXPECT_EQ(cancelled.error().stats, ScalarReplayExecutionStats{});
    const auto input = square();
    const auto limited = execute_scalar_replay(input, {.fuel = 1});
    ASSERT_FALSE(limited);
    const auto repeated = execute_scalar_replay(input, {.fuel = 1});
    ASSERT_FALSE(repeated);
    EXPECT_EQ(limited.error().stats, repeated.error().stats);
    const auto complete = execute_scalar_replay(input, {.fuel = 3});
    ASSERT_TRUE(complete);
    EXPECT_EQ(complete->stats.fuel_consumed, 3U);
    EXPECT_EQ(complete->details.size(), 2U);
}
TEST(ScalarReplayExecution,
     AllBinaryOperationsMatchExistingVmAtEveryWidthAndSignedness) {
    using S = syntax::BinaryOperator;
    for (const auto& type :
         {sema::Type::integer(8), sema::Type::integer(8, true),
          sema::Type::integer(16), sema::Type::integer(16, true),
          sema::Type::integer(32), sema::Type::integer(32, true),
          sema::Type::integer(64), sema::Type::integer(64, true),
          sema::Type::usize(32), sema::Type::usize(64)}) {
        const auto a = *IntegerValue::create(type.integer_type, UINT64_MAX - 2);
        const auto b = *IntegerValue::create(type.integer_type, 3);
        for (const auto& [op, vm_op] : {std::pair{Op::Add, S::Add},
                                        {Op::Subtract, S::Subtract},
                                        {Op::Multiply, S::Multiply},
                                        {Op::Divide, S::Divide},
                                        {Op::Remainder, S::Remainder},
                                        {Op::BitAnd, S::BitAnd},
                                        {Op::BitOr, S::BitOr},
                                        {Op::BitXor, S::BitXor},
                                        {Op::ShiftLeft, S::ShiftLeft},
                                        {Op::ShiftRight, S::ShiftRight},
                                        {Op::Equal, S::Equal},
                                        {Op::NotEqual, S::NotEqual},
                                        {Op::Less, S::Less},
                                        {Op::LessEqual, S::LessEqual},
                                        {Op::Greater, S::Greater},
                                        {Op::GreaterEqual, S::GreaterEqual}}) {
            const auto expected = vm::Evaluator::binary(vm_op, a, b);
            ASSERT_TRUE(expected);
            const auto output_type =
                op >= Op::Equal ? sema::Type(sema::TypeKind::Bool) : type;
            auto input = binary(op, type, a, b, {output_type, *expected});
            if (type.integer_type.is_usize)
                input.descriptor.specialization.context.target =
                    cache_test::context(type.integer_type.width).target;
            const auto output = execute_scalar_replay(input);
            ASSERT_TRUE(output)
                << static_cast<unsigned>(op) << ' ' << sema::type_name(type);
            EXPECT_EQ(output->value.value, *expected);
            EXPECT_EQ(output->details.back().value,
                      std::get_if<bool>(&*expected)
                          ? ScalarConstant(std::get<bool>(*expected))
                          : ScalarConstant(std::get<IntegerValue>(*expected)));
        }
    }
}
TEST(ScalarReplayExecution, UnaryWrapAndBooleanOperationsMatchVm) {
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        for (bool sign : {false, true}) {
            const auto type = sema::Type::integer(width, sign);
            for (const auto bits : {std::uint64_t{0}, UINT64_MAX,
                                    std::uint64_t{1} << (width - 1)}) {
                const auto value =
                    *IntegerValue::create(type.integer_type, bits);
                for (const auto& [op, vm_op] :
                     {std::pair{Op::Negate, syntax::UnaryOperator::Negate},
                      {Op::BitNot, syntax::UnaryOperator::BitNot}}) {
                    const auto expected = vm::Evaluator::unary(vm_op, value);
                    ASSERT_TRUE(expected);
                    const auto input = record(
                        {literal(type, value),
                         {.operation = op, .type = type, .operands = {0}}},
                        {type, *expected});
                    const auto output = execute_scalar_replay(input);
                    ASSERT_TRUE(output);
                    EXPECT_EQ(output->value.value, *expected);
                }
            }
        }
    }
    for (bool a : {false, true}) {
        const sema::Type type{sema::TypeKind::Bool};
        const auto output = execute_scalar_replay(
            record({literal(type, a),
                    {.operation = Op::Not, .type = type, .operands = {0}}},
                   {type, !a}));
        ASSERT_TRUE(output);
        for (bool b : {false, true}) {
            for (const auto op : {Op::Equal, Op::NotEqual}) {
                const auto equal = execute_scalar_replay(binary(
                    op, type, a, b, {type, (a == b) == (op == Op::Equal)}));
                ASSERT_TRUE(equal);
            }
        }
    }
}
TEST(ScalarReplayExecution,
     DivisionRemainderAndShiftFailuresAreTypedAndChargeAttemptedNode) {
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        for (unsigned failure = 0; failure < 8; ++failure) {
            const auto type = sema::Type::integer(width, true);
            const auto a = *IntegerValue::create(
                type.integer_type, std::uint64_t{1} << (width - 1));
            const auto b = *IntegerValue::create(
                type.integer_type, failure < 2                   ? 0
                                   : failure < 4 || failure >= 6 ? UINT64_MAX
                                                                 : width);
            const auto op = failure == 0 || failure == 2   ? Op::Divide
                            : failure == 1 || failure == 3 ? Op::Remainder
                            : failure % 2 == 0             ? Op::ShiftLeft
                                                           : Op::ShiftRight;
            const auto expected = failure < 2   ? IntegerError::DivideByZero
                                  : failure < 4 ? IntegerError::SignedOverflow
                                                : IntegerError::InvalidShift;
            const auto input =
                binary(op, type, a, b,
                       {type, *IntegerValue::create(type.integer_type, 0)});
            const auto limited = execute_scalar_replay(input, {.fuel = 2});
            ASSERT_FALSE(limited);
            EXPECT_EQ(limited.error().code, Code::FuelExhausted);
            const auto output = execute_scalar_replay(input);
            ASSERT_FALSE(output);
            EXPECT_EQ(output.error().code, Code::ArithmeticFailure);
            EXPECT_EQ(output.error().integer, expected);
            EXPECT_EQ(output.error().node, 2U);
            EXPECT_EQ(output.error().stats.nodes_evaluated, 2U);
            EXPECT_EQ(output.error().stats.fuel_consumed, 3U);
        }
    }
}
TEST(ScalarReplayExecution,
     FlatLongRecipeIsIterativeAndNeedsOnlySelectedOutput) {
    auto input = square();
    input.descriptor.nodes.resize(1);
    for (std::uint32_t i = 1; i <= 20000; ++i)
        input.descriptor.nodes.push_back({.operation = Op::Negate,
                                          .type = input.value.type,
                                          .position = i,
                                          .operands = {i - 1}});
    input.descriptor.result_site.position = 20000;
    input.value.value = IntegerValue(7);
    input.descriptor.expected_result = *replay_value_fingerprint(input.value);
    const auto output = execute_scalar_replay(
        input,
        {.fuel = 20002,
         .value_bytes = input.descriptor.nodes.size() * sizeof(ScalarConstant),
         .output_records = 0,
         .output_bytes = sizeof(VerifiedScalarReplay),
         .codec = {.depth = 1}},
        {.count = 0});
    ASSERT_TRUE(output);
    EXPECT_EQ(output->stats.nodes_evaluated, 20001U);
    EXPECT_EQ(output->stats.fuel_consumed, 20002U);
    EXPECT_TRUE(output->details.empty());
    EXPECT_EQ(std::get<IntegerValue>(output->value.value).bits(), 7U);
}
TEST(ScalarReplayExecution,
     LoadedRealStageRecordOutlivesCompilerAndExecutionDoesNotWriteCache) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned> sequence{};
    const auto path =
        fs::temp_directory_path() /
        ("pagos-execute-replay-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(sequence++));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{path};
    std::optional<ScalarReplayRecord> owned;
    {
        cache_test::Input source(
            "fn square(x: u32) -> u32 { x * x } let unrelated = "
            "external_input(); let a = square(7);");
        StageAnalyzer analyzer(source.diagnostics, source.checker.types(), {},
                               cache_test::context(),
                               {CacheMode::ReadWrite, path});
        const auto module = analyzer.analyze(*source.module);
        ASSERT_FALSE(source.diagnostics.has_error());
        const auto identity = source.identity();
        ASSERT_TRUE(identity);
        const auto key = make_query_key(
            *identity->find(cache::DefinitionKind::Function, "square"),
            cache_test::context(), cache_test::key().arguments);
        ASSERT_TRUE(key);
        auto loaded = load_scalar_replay(cache::FileStore(path), *source.module,
                                         source.checker.types(), *key);
        ASSERT_TRUE(loaded);
        const auto stats = analyzer.stats();
        std::ostringstream before, after;
        hir::print(*module, before);
        const auto result = execute_scalar_replay(*loaded);
        ASSERT_TRUE(result);
        hir::print(*module, after);
        EXPECT_EQ(before.str(), after.str());
        EXPECT_EQ(analyzer.stats().fuel_consumed, stats.fuel_consumed);
        EXPECT_EQ(analyzer.stats().specializations, stats.specializations);
        EXPECT_EQ(analyzer.stats().cache_hits, stats.cache_hits);
        owned = std::move(*loaded);
    }
    ASSERT_TRUE(owned);
    const auto snapshot = [&] {
        std::map<std::string, std::pair<cache::Digest, fs::file_time_type>>
            files;
        for (const auto& file : fs::recursive_directory_iterator(path)) {
            if (!file.is_regular_file())
                continue;
            std::ifstream stream(file.path(), std::ios::binary);
            const cache::Bytes bytes((std::istreambuf_iterator<char>(stream)),
                                     {});
            files.emplace(file.path().string(),
                          std::pair{cache::fingerprint("test.files", bytes),
                                    fs::last_write_time(file.path())});
        }
        return files;
    };
    const auto before = snapshot();
    for (unsigned i = 0; i < 3; ++i) {
        const auto output = execute_scalar_replay(*owned);
        ASSERT_TRUE(output);
        EXPECT_EQ(output->value, owned->value);
        ASSERT_EQ(output->details.size(), 3U);
        EXPECT_EQ(std::get<IntegerValue>(output->details.back().value).bits(),
                  49U);
    }
    EXPECT_EQ(snapshot(), before);
}
} // namespace
