#include "pagos/hir/hir.h"
#include "pagos/syntax/ast.h"
#include "pagos/vm/evaluator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

TEST(Evaluator, U32AdditionWraps) {
    const pagos::hir::Constant maximum{
        std::numeric_limits<std::uint32_t>::max()};
    const pagos::hir::Constant one{std::uint32_t{1}};

    const auto result = pagos::vm::Evaluator::binary(
        pagos::syntax::BinaryOperator::Add, maximum, one);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<std::uint32_t>(*result), 0U);
}

TEST(Evaluator, DivisionByZeroIsAnEvaluationError) {
    const pagos::hir::Constant ten{std::uint32_t{10}};
    const pagos::hir::Constant zero{std::uint32_t{0}};

    const auto result = pagos::vm::Evaluator::binary(
        pagos::syntax::BinaryOperator::Divide, ten, zero);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, "E4001");
    EXPECT_EQ(result.error().message, "division by zero");
}

TEST(Evaluator, BooleanNotIsStatic) {
    const pagos::hir::Constant value{true};
    const auto result =
        pagos::vm::Evaluator::unary(pagos::syntax::UnaryOperator::Not, value);

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(std::get<bool>(*result));
}

} // namespace
