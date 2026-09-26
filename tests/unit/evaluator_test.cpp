#include "pagos/hir/hir.h"
#include "pagos/syntax/ast.h"
#include "pagos/vm/evaluator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <tuple>

namespace {

TEST(Evaluator, UnsignedWidthsMatchIndependentModularArithmetic) {
    using pagos::IntegerValue;
    using enum pagos::syntax::BinaryOperator;
    std::uint64_t state = 12345;
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        const auto mask = ~std::uint64_t{0} >> (64 - width);
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            state = state * 6364136223846793005ULL + 1;
            const auto left = state & mask;
            state = state * 6364136223846793005ULL + 1;
            const auto right = (state & mask) | 1;
            const auto a = *IntegerValue::create({width, false}, left);
            const auto b = *IntegerValue::create({width, false}, right);
            for (const auto& [operation, expected] :
                 {std::pair{Add, (left + right) & mask},
                  {Subtract, (left - right) & mask},
                  {Multiply, (left * right) & mask},
                  {Divide, left / right},
                  {Remainder, left % right},
                  {BitAnd, left & right},
                  {BitOr, left | right},
                  {BitXor, left ^ right}}) {
                const auto result =
                    pagos::vm::Evaluator::binary(operation, a, b);
                ASSERT_TRUE(result);
                EXPECT_EQ(std::get<IntegerValue>(*result).bits(), expected);
                EXPECT_EQ(std::get<IntegerValue>(*result).type(), a.type());
            }
            const auto comparison = pagos::vm::Evaluator::binary(Less, a, b);
            ASSERT_TRUE(comparison);
            EXPECT_EQ(std::get<bool>(*comparison), left < right);
        }
    }
}

TEST(Evaluator, U32AdditionWraps) {
    const pagos::hir::Constant maximum{
        std::numeric_limits<std::uint32_t>::max()};
    const pagos::hir::Constant one{std::uint32_t{1}};

    const auto result = pagos::vm::Evaluator::binary(
        pagos::syntax::BinaryOperator::Add, maximum, one);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<pagos::IntegerValue>(*result).bits(), 0U);
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

TEST(Evaluator, BitwiseOperationsUseUnsigned32BitSemantics) {
    using enum pagos::syntax::BinaryOperator;
    for (const auto& [operation, left, right, expected] :
         {std::tuple{BitAnd, 240U, 170U, 160U},
          std::tuple{BitOr, 240U, 15U, 255U},
          std::tuple{BitXor, 255U, 170U, 85U},
          std::tuple{ShiftLeft, 1U, 31U, 2147483648U},
          std::tuple{ShiftLeft, 2U, 31U, 0U},
          std::tuple{ShiftRight, 2147483648U, 31U, 1U},
          std::tuple{ShiftRight, 4294967295U, 0U, 4294967295U}}) {
        const auto result = pagos::vm::Evaluator::binary(
            operation, std::uint32_t{left}, std::uint32_t{right});
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(std::get<pagos::IntegerValue>(*result).bits(), expected);
    }
    const auto result = pagos::vm::Evaluator::unary(
        pagos::syntax::UnaryOperator::BitNot, std::uint32_t{0});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<pagos::IntegerValue>(*result).bits(), 4294967295U);
}

TEST(Evaluator, InvalidShiftsFailBeforeHostArithmetic) {
    for (const auto operation : {pagos::syntax::BinaryOperator::ShiftLeft,
                                 pagos::syntax::BinaryOperator::ShiftRight}) {
        for (const auto count : {32U, 33U, 4294967295U}) {
            const auto result = pagos::vm::Evaluator::binary(
                operation, std::uint32_t{1}, std::uint32_t{count});
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code, "E4009");
        }
    }
}

} // namespace
