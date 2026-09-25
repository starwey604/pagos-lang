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
        EXPECT_EQ(
            std::get<std::vector<std::uint32_t>>(*result->result->constant),
            (std::vector<std::uint32_t>{4, 9, 16}));
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

} // namespace
