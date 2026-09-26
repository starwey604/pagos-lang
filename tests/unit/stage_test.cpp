#include "pagos/mir/lowering.h"
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

TEST(ResidualSpecialization, ReusesDefinitionsAndResetsBetweenAnalyses) {
    StageInput input("fn scale(k: u32, x: u32) -> u32 { "
                     "let table = [for i in 0..16 { i }]; x * k + table[0] } "
                     "let a = scale(2, external_input()); "
                     "let b = scale(2, external_input());");
    pagos::stage::AnalysisLimits limits;
    limits.residual_specializations = 1;
    limits.specializations = 0;
    limits.array_elements = 16;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    for (int iteration = 0; iteration < 2; ++iteration) {
        const auto hir = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
        EXPECT_EQ(analyzer.stats().residual_cache_hits, 1U);
        EXPECT_EQ(analyzer.stats().specializations, 0U);
        EXPECT_EQ(analyzer.stats().array_elements_reserved, 16U);
        const auto mir = pagos::mir::lower(*hir);
        ASSERT_TRUE(mir) << mir.error();
        ASSERT_EQ(mir->functions.size(), 2U);
        EXPECT_EQ(mir->functions[1].parameters.size(), 1U);
    }
}

TEST(ResidualSpecialization, KeysSeparateValuesPositionsAndFunctions) {
    StageInput input("fn f(a: u32, b: u32) -> u32 { a + b } "
                     "fn g(a: u32, b: u32) -> u32 { a + b } "
                     "let x = external_input(); "
                     "let a = f(1, x); let b = f(2, x); "
                     "let c = f(x, 1); let d = f(x, x); "
                     "let e = g(1, x); let hit = f(1, x);");
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types());
    const auto hir = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().residual_specializations, 5U);
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 1U);
    const auto mir = pagos::mir::lower(*hir);
    ASSERT_TRUE(mir) << mir.error();
    EXPECT_EQ(mir->functions.size(), 6U);
}

TEST(ResidualSpecialization, RejectsOneOverBudgetAndReportsOnce) {
    StageInput input("fn f(k: u32, x: u32) -> u32 { k + x } "
                     "let a = f(1, external_input()); "
                     "let b = f(2, external_input()); "
                     "let c = f(3, external_input());");
    pagos::stage::AnalysisLimits limits;
    limits.residual_specializations = 1;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    (void)analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E4012");
    EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 0U);
}

TEST(ResidualSpecialization, ZeroBudgetPermitsStaticResultsWithEffects) {
    StageInput input("fn known(x: u32) -> u32 { x; external_input(); 7 } "
                     "static let a = known(external_input()); "
                     "static let b = known(external_input());");
    pagos::stage::AnalysisLimits limits;
    limits.residual_specializations = 0;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto hir = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().residual_specializations, 0U);
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 0U);
    ASSERT_TRUE(hir->result->constant);
    EXPECT_EQ(std::get<pagos::IntegerValue>(*hir->result->constant).bits(), 7U);
    const auto mir = pagos::mir::lower(*hir);
    ASSERT_TRUE(mir) << mir.error();
    EXPECT_EQ(mir->functions.size(), 1U);
}

TEST(ResidualSpecialization, AllStaticArgumentsCanShareAnEffectfulBody) {
    StageInput input("fn read(k: u32) -> u32 { external_input() + k } "
                     "let a = read(1); let b = read(1);");
    pagos::stage::AnalysisLimits limits;
    limits.specializations = 1;
    limits.residual_specializations = 1;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto hir = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().specializations, 1U);
    EXPECT_EQ(analyzer.stats().cache_hits, 0U);
    EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 1U);
    const auto mir = pagos::mir::lower(*hir);
    ASSERT_TRUE(mir) << mir.error();
    ASSERT_EQ(mir->functions.size(), 2U);
    EXPECT_TRUE(mir->functions[1].parameters.empty());
}

TEST(ResidualSpecialization, FailedBodiesAreNotPublished) {
    StageInput input("fn bad(x: u32) -> u32 { static let invalid = x; x } "
                     "let a = bad(external_input()); "
                     "let b = bad(external_input());");
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types());
    (void)analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 2U);
    EXPECT_EQ(analyzer.stats().residual_specializations, 0U);
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 0U);
    for (const auto& diagnostic : input.diagnostics.diagnostics()) {
        EXPECT_EQ(diagnostic.code, "E2001");
        EXPECT_EQ(
            diagnostic.dependency_path,
            (std::vector<std::string>{"external_input", "bad.x", "invalid"}));
    }
}

TEST(ResidualSpecialization, CacheHitTraceUsesCurrentCallerThroughNestedCalls) {
    StageInput input(
        "fn inner(x: u32) -> u32 { x + x } "
        "fn outer(x: u32) -> u32 { inner(x) } "
        "let first = external_input(); let a = outer(first); "
        "let second = external_input(); static let b = outer(second);");
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types());
    (void)analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    const auto& diagnostic = input.diagnostics.diagnostics()[0];
    EXPECT_EQ(diagnostic.code, "E2001");
    EXPECT_EQ(diagnostic.dependency_path,
              (std::vector<std::string>{"external_input", "second", "outer.x",
                                        "inner.x", "b"}));
    EXPECT_EQ(analyzer.stats().residual_specializations, 2U);
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 1U);
}

TEST(ResidualSpecialization, StaticRecursionMayBuildDistinctResidualVersions) {
    StageInput input(
        "fn expand(n: u32) -> u32 { if n == 0 { external_input() } "
        "else { expand(n - 1) + 1 } } "
        "let a = expand(2); let b = expand(2);");
    pagos::stage::AnalysisLimits limits;
    limits.residual_specializations = 3;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto hir = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().residual_specializations, 3U);
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 1U);
    const auto mir = pagos::mir::lower(*hir);
    ASSERT_TRUE(mir) << mir.error();
    EXPECT_EQ(mir->functions.size(), 4U);
}

TEST(RuntimeRecursion, ModuleOwnsCyclesWithoutKeepingThemAlive) {
    StageInput input(
        "fn a(n: u32) -> u32 { if n == 0 { 0 } else { b(n - 1) } } "
        "fn b(n: u32) -> u32 { if n == 0 { 1 } else { a(n - 1) } } "
        "let result = a(external_input());");
    std::unique_ptr<pagos::hir::Module> hir;
    std::weak_ptr<const pagos::hir::ResidualFunction> observed;
    {
        pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                             input.checker.types());
        hir = analyzer.analyze(*input.module);
        ASSERT_FALSE(input.diagnostics.has_error());
        ASSERT_EQ(hir->residual_functions.size(), 2U);
        observed = hir->residual_functions.front();
    }
    // Calls survive the analyzer, but cyclic call edges do not own definitions.
    ASSERT_FALSE(observed.expired());
    const auto mir = pagos::mir::lower(*hir);
    ASSERT_TRUE(mir) << mir.error();
    EXPECT_EQ(mir->functions.size(), 3U);
    hir.reset();
    EXPECT_TRUE(observed.expired());
}

TEST(RuntimeRecursion, ReusesActiveSignatureAndResetsSessions) {
    StageInput input(
        "fn sum(n: u32) -> u32 { if n == 0 { 0 } "
        "else { n + sum(n - 1) } } "
        "let a = sum(external_input()); let b = sum(external_input());");
    pagos::stage::AnalysisLimits limits;
    limits.recursion_depth = 1;
    limits.residual_specializations = 1;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto first = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto second = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().maximum_recursion_depth, 1U);
    EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 2U);
    ASSERT_EQ(first->residual_functions.size(), 1U);
    ASSERT_EQ(second->residual_functions.size(), 1U);
    EXPECT_NE(first->residual_functions[0], second->residual_functions[0]);
    EXPECT_TRUE(pagos::mir::lower(*first));
    EXPECT_TRUE(pagos::mir::lower(*second));
}

TEST(RuntimeRecursion, StaticSubcallsRemainStaticInsideRuntimeVersions) {
    StageInput input("fn f(n: u32) -> u32 { if n == 0 { 7 } else { "
                     "static let base = f(0); base + n } } "
                     "let result = f(external_input());");
    pagos::stage::AnalysisLimits limits;
    limits.residual_specializations = 1;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto hir = analyzer.analyze(*input.module);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().specializations, 1U);
    EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
    EXPECT_FALSE(hir->residual_functions[0]->recursive);
    EXPECT_TRUE(pagos::mir::lower(*hir));
}

TEST(RuntimeRecursion, VersionGrowthIsBoundedBeforeDepthExhaustion) {
    StageInput input("fn grow(k: u32, n: u32) -> u32 { if n == 0 { k } "
                     "else { grow(k + 1, n - 1) } } "
                     "let result = grow(0, external_input());");
    pagos::stage::AnalysisLimits limits;
    limits.residual_specializations = 3;
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types(), limits);
    const auto hir = analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 1U);
    EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E4012");
    EXPECT_EQ(analyzer.stats().residual_specializations, 0U);
    EXPECT_LE(analyzer.stats().maximum_recursion_depth, 4U);
    EXPECT_TRUE(hir->residual_functions.empty());
}

TEST(RuntimeRecursion, FailedComponentsAreNotReused) {
    StageInput input(
        "fn a(n: u32) -> u32 { b(n); static let invalid = n; n } "
        "fn b(n: u32) -> u32 { a(n) } "
        "let first = a(external_input()); let second = b(external_input());");
    pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                         input.checker.types());
    (void)analyzer.analyze(*input.module);
    ASSERT_EQ(input.diagnostics.size(), 2U);
    for (const auto& diagnostic : input.diagnostics.diagnostics())
        EXPECT_EQ(diagnostic.code, "E2001");
    // Both hits are active backedges, not reuse of the failed first component.
    EXPECT_EQ(analyzer.stats().residual_cache_hits, 2U);
}

TEST(RuntimeRecursion,
     EffectfulInfiniteCycleIsResidualButPureCycleIsStaticError) {
    for (const bool effectful : {false, true}) {
        StageInput input(std::string("fn spin() -> u32 { ") +
                         (effectful ? "external_input(); " : "") +
                         "spin() } let result = spin();");
        pagos::stage::StageAnalyzer analyzer(input.diagnostics,
                                             input.checker.types());
        const auto hir = analyzer.analyze(*input.module);
        if (effectful) {
            ASSERT_FALSE(input.diagnostics.has_error());
            EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
            EXPECT_TRUE(pagos::mir::lower(*hir));
        } else {
            ASSERT_EQ(input.diagnostics.size(), 1U);
            EXPECT_EQ(input.diagnostics.diagnostics()[0].code, "E4003");
        }
    }
}

} // namespace
