#include "pagos/sema/type_checker.h"
#include "pagos/source/diagnostic.h"
#include "pagos/source/source_manager.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace {

struct StageInput {
    explicit StageInput(std::string text)
        : source(pagos::source::SourceFile::from_text("stage.pgs",
                                                      std::move(text))),
          diagnostics(source), checker(diagnostics) {
        pagos::syntax::Lexer lexer(source, diagnostics);
        const auto tokens = lexer.tokenize();
        pagos::syntax::Parser parser(tokens, diagnostics);
        module = parser.parse_module();
        EXPECT_FALSE(diagnostics.has_error());
        EXPECT_TRUE(checker.check(*module));
    }

    pagos::source::SourceFile source;
    pagos::source::DiagnosticEngine diagnostics;
    pagos::sema::TypeChecker checker;
    std::unique_ptr<pagos::syntax::Module> module;
};

TEST(StageBudget, ResetsReservationsBetweenAnalyses) {
    StageInput input("let table = [for i in 2..5 { i * i }];");
    pagos::stage::AnalysisLimits limits;
    limits.array_elements = 3;
    limits.array_bytes = 12;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    for (int iteration = 0; iteration < 2; ++iteration) {
        const auto result = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(analyzer.stats().array_elements_reserved, 3U);
        EXPECT_EQ(analyzer.stats().array_bytes_reserved, 12U);
        ASSERT_TRUE(result->result->constant);
        EXPECT_EQ(std::get<std::vector<pagos::IntegerValue>>(
                      *result->result->constant),
                  (std::vector<pagos::IntegerValue>{4, 9, 16}));
    }
}

TEST(StageBudget, RejectsHugeArrayBeforeAllocationOrEvaluation) {
    StageInput input("let table = [for i in 0..4294967295 { 1 / 0 }];");
    pagos::stage::AnalysisLimits limits;
    limits.array_elements = std::numeric_limits<std::size_t>::max();
    limits.array_bytes = 7;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto result = analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E4008");
    EXPECT_EQ(analyzer.stats().array_elements_reserved, 0U);
    EXPECT_EQ(analyzer.stats().array_bytes_reserved, 0U);
    EXPECT_EQ(result->result, nullptr);
}

TEST(StageBudget, ReservesAtomicallyAndReportsExhaustionOnce) {
    StageInput input("let a = [1, 2]; let b = [true]; let c = [3];");
    pagos::stage::AnalysisLimits limits;
    limits.array_bytes = 8;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto result = analyzer.analyze(*input.module);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E4008");
    EXPECT_EQ(analyzer.stats().array_elements_reserved, 2U);
    EXPECT_EQ(analyzer.stats().array_bytes_reserved, 8U);
}

TEST(StageBudget, ChargesBooleanSlotsAsLogicalBytes) {
    StageInput input("let table = [for i in 0..9 { i == 0 }];");
    pagos::stage::AnalysisLimits limits;
    limits.array_elements = 9;
    limits.array_bytes = 9;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto result = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().array_elements_reserved, 9U);
    EXPECT_EQ(analyzer.stats().array_bytes_reserved, 9U);
    ASSERT_TRUE(result->result->constant);
    EXPECT_EQ(std::get<std::vector<bool>>(*result->result->constant).size(),
              9U);
}

TEST(StageRecord, CanonicalFieldsParticipateInSpecializationKeys) {
    StageInput input(
        "record Config { value: u32, ready: bool } "
        "fn read(c: Config) -> u32 { if c.ready { c.value } else { 0 } } "
        "let a = read(Config(ready: true, value: 7)); "
        "let b = read(Config(value: 7, ready: true)); "
        "let c = read(Config(value: 8, ready: true)); "
        "let d = read(Config(value: 7, ready: false)); "
        "let result = a + b + c + d;");
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types());
    for (int iteration = 0; iteration < 2; ++iteration) {
        const auto result = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(analyzer.stats().specializations, 3U);
        EXPECT_EQ(analyzer.stats().cache_hits, 1U);
        ASSERT_TRUE(result->result->constant);
        EXPECT_EQ(
            std::get<pagos::IntegerValue>(*result->result->constant).bits(),
            22U);
    }
}

TEST(StageRecord, ConstantsCarryNominalIdentityAndScalarFieldTypes) {
    const pagos::RecordConstant first{.name = "A",
                                      .fields = {std::uint32_t{1}, true}};
    EXPECT_NE(first,
              (pagos::RecordConstant{.name = "B", .fields = first.fields}));
    EXPECT_NE(first, (pagos::RecordConstant{
                         .name = "A", .fields = {true, std::uint32_t{1}}}));
}

TEST(AggregateBudget, MixedReservationsResetBetweenAnalyses) {
    StageInput input("record R { flag: bool, value: u32 } "
                     "let r = R(value: 1, flag: true); let a = [true, false];");
    pagos::stage::AnalysisLimits limits;
    limits.aggregate_members = 4;
    limits.aggregate_bytes = 7;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    for (int iteration = 0; iteration < 2; ++iteration) {
        const auto result = analyzer.analyze(*input.module);
        ASSERT_NE(result, nullptr);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(analyzer.stats().aggregate_constructions, 2U);
        EXPECT_EQ(analyzer.stats().aggregate_members_reserved, 4U);
        EXPECT_EQ(analyzer.stats().aggregate_bytes_reserved, 7U);
        EXPECT_EQ(analyzer.stats().array_elements_reserved, 2U);
        EXPECT_EQ(analyzer.stats().array_bytes_reserved, 2U);
    }
}

TEST(AggregateBudget, RejectsRecordBeforeInitializerAndReportsOnce) {
    StageInput input("record R { flag: bool, value: u32 } "
                     "let r = R(value: 1 / 0, flag: true); let a = [1];");
    pagos::stage::AnalysisLimits limits;
    limits.aggregate_bytes = 4;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto result = analyzer.analyze(*input.module);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E4010");
    EXPECT_EQ(analyzer.stats().aggregate_constructions, 0U);
    EXPECT_EQ(analyzer.stats().aggregate_members_reserved, 0U);
    EXPECT_EQ(analyzer.stats().aggregate_bytes_reserved, 0U);
    EXPECT_EQ(analyzer.stats().array_elements_reserved, 0U);
}

TEST(AggregateBudget, ArrayAndSharedReservationsAreAtomic) {
    for (const bool fail_array_limit : {false, true}) {
        StageInput input("record R { value: u32 } let r = R(value: 1); "
                         "let a = [1, 2]; let b = R(value: 3);");
        pagos::stage::AnalysisLimits limits;
        limits.aggregate_members = 2;
        if (fail_array_limit) {
            limits.array_elements = 1;
        }
        pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                             input.checker.types(), limits);
        const auto result = analyzer.analyze(*input.module);
        ASSERT_NE(result, nullptr);
        ASSERT_EQ(input.diagnostics.size(), 1U);
        EXPECT_EQ(input.diagnostics.diagnostics()[0].code,
                  fail_array_limit ? "E4008" : "E4010");
        EXPECT_EQ(analyzer.stats().aggregate_constructions, 1U);
        EXPECT_EQ(analyzer.stats().aggregate_members_reserved, 1U);
        EXPECT_EQ(analyzer.stats().aggregate_bytes_reserved, 4U);
        EXPECT_EQ(analyzer.stats().array_elements_reserved, 0U);
        EXPECT_EQ(analyzer.stats().array_bytes_reserved, 0U);
    }
}

TEST(AggregateBudget, HugeGeneratorFailsBeforeExpansion) {
    StageInput input("let table = [for i in 0..4294967295 { 1 / 0 }];");
    pagos::stage::AnalysisLimits limits;
    limits.array_elements = std::numeric_limits<std::size_t>::max();
    limits.array_bytes = std::numeric_limits<std::size_t>::max();
    limits.aggregate_members = std::numeric_limits<std::size_t>::max();
    limits.aggregate_bytes = 7;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto result = analyzer.analyze(*input.module);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code,
              sizeof(std::size_t) >= 8 ? "E4010" : "E4008");
    EXPECT_EQ(analyzer.stats().aggregate_members_reserved, 0U);
    EXPECT_EQ(analyzer.stats().array_elements_reserved, 0U);
}

TEST(AggregateBudget, MaximumLimitsPreserveMixedAccounting) {
    StageInput input("record R { flag: bool, value: u32 } "
                     "let r = R(value: 1, flag: true); let a = [1, 2];");
    pagos::stage::AnalysisLimits limits;
    limits.aggregate_members = std::numeric_limits<std::size_t>::max();
    limits.aggregate_bytes = std::numeric_limits<std::size_t>::max();
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto result = analyzer.analyze(*input.module);
    ASSERT_NE(result, nullptr);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().aggregate_members_reserved, 4U);
    EXPECT_EQ(analyzer.stats().aggregate_bytes_reserved, 13U);
}

TEST(StageGenerator, ResolvesLengthWithoutMutatingSemanticTypes) {
    using namespace pagos;
    StageInput input("let n = 3; runtime let table = [for i in 0..n { i }];");
    const auto& binding =
        static_cast<const syntax::BindingStmt&>(*input.module->statements[1]);
    ASSERT_EQ(input.checker.types().expressions.at(binding.initializer.get()),
              sema::Type::array(sema::TypeKind::Integer, 0));
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    for (int iteration = 0; iteration < 2; ++iteration) {
        const auto result = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        ASSERT_EQ(result->bindings.size(), 2U);
        const auto expected = sema::Type::array(sema::TypeKind::Integer, 3);
        EXPECT_EQ(result->bindings[1].type, expected);
        EXPECT_EQ(result->bindings[1].value->type, expected);
        EXPECT_EQ(result->bindings[1].value->operands[0]->type, expected);
        EXPECT_EQ(input.checker.types()
                      .expressions.at(binding.initializer.get())
                      .length,
                  0U);
        EXPECT_EQ(analyzer.stats().array_elements_reserved, 3U);
    }
}

TEST(StageGenerator, ResolvesEachSpecializationIndependently) {
    StageInput input("fn last(n: u32) -> u32 { "
                     "let a = [for i in 0..n { i }]; a[n - 1] } "
                     "let a = last(2); let b = last(4); let c = last(2);");
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types());
    const auto result = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    ASSERT_EQ(result->bindings.size(), 3U);
    EXPECT_EQ(
        std::get<pagos::IntegerValue>(*result->bindings[0].value->constant)
            .bits(),
        1U);
    EXPECT_EQ(
        std::get<pagos::IntegerValue>(*result->bindings[1].value->constant)
            .bits(),
        3U);
    EXPECT_EQ(
        std::get<pagos::IntegerValue>(*result->bindings[2].value->constant)
            .bits(),
        1U);
    EXPECT_EQ(analyzer.stats().array_elements_reserved, 6U);
    EXPECT_EQ(analyzer.stats().cache_hits, 1U);
}

TEST(StageGenerator, BoundConstructionPrecedesGeneratorReservation) {
    StageInput input("let a = [for i in 0..[1, 3][1] { 1 / 0 }];");
    pagos::stage::AnalysisLimits limits;
    limits.aggregate_members = 4;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    (void)analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E4010");
    EXPECT_EQ(analyzer.stats().array_elements_reserved, 2U);
    EXPECT_EQ(analyzer.stats().aggregate_constructions, 1U);
    EXPECT_EQ(analyzer.stats().aggregate_members_reserved, 2U);
    EXPECT_EQ(analyzer.stats().aggregate_bytes_reserved, 8U);
}

TEST(StageGenerator, BoundsFailBeforeReservation) {
    for (const auto& [bound, code] :
         {std::pair{"external_input()", "E2002"}, std::pair{"1 / 0", "E4001"},
          std::pair{"1 - 1", "E1005"}}) {
        StageInput input("let a = [for i in 0.." + std::string(bound) +
                         " { i }];");
        pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                             input.checker.types());
        (void)analyzer.analyze(*input.module);
        ASSERT_EQ(input.diagnostics.size(), 1U);
        EXPECT_EQ(input.diagnostics.diagnostics()[0].code, code);
        EXPECT_EQ(analyzer.stats().aggregate_constructions, 0U);
        EXPECT_EQ(analyzer.stats().array_elements_reserved, 0U);
    }
}

TEST(StageGenerator, ReturningBoundDoesNotReserveOrEvaluateLaterWork) {
    StageInput input(
        "fn choose() -> u32 { "
        "[for i in if true { return 7; } else { 0 }..1 / 0 { 1 / 0 }]; "
        "1 / 0 } let result = choose();");
    pagos::stage::AnalysisLimits limits;
    limits.aggregate_members = 0;
    limits.aggregate_bytes = 0;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto result = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    ASSERT_TRUE(result->result->constant);
    EXPECT_EQ(std::get<pagos::IntegerValue>(*result->result->constant).bits(),
              7U);
    EXPECT_EQ(analyzer.stats().aggregate_constructions, 0U);
}

} // namespace
