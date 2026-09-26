#include "pagos/integer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using pagos::IntegerError;
using pagos::IntegerOperation;
using pagos::IntegerValue;

TEST(Integer, SignedLiteralBoundsAndNegationAreWidthDefined) {
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        const auto magnitude = std::uint64_t{1} << (width - 1);
        const auto minimum = IntegerValue::parse_decimal(
            {width, true}, "-" + std::to_string(magnitude));
        ASSERT_TRUE(minimum);
        EXPECT_EQ(minimum->bits(), magnitude);
        EXPECT_EQ(minimum->negate(), minimum);
        EXPECT_TRUE(IntegerValue::parse_decimal({width, true},
                                                std::to_string(magnitude - 1)));
        EXPECT_EQ(IntegerValue::parse_decimal({width, true},
                                              std::to_string(magnitude))
                      .error(),
                  IntegerError::InvalidLiteral);
        EXPECT_EQ(IntegerValue::parse_decimal(
                      {width, true}, "-" + std::to_string(magnitude + 1))
                      .error(),
                  IntegerError::InvalidLiteral);
        const auto minus_one =
            *IntegerValue::parse_decimal({width, true}, "-1");
        EXPECT_EQ(minimum->apply(IntegerOperation::Divide, minus_one).error(),
                  IntegerError::SignedOverflow);
        EXPECT_EQ(
            minimum->apply(IntegerOperation::Remainder, minus_one).error(),
            IntegerError::SignedOverflow);
        EXPECT_EQ(
            minus_one.apply(IntegerOperation::ShiftRight, minus_one).error(),
            IntegerError::InvalidShift);
        EXPECT_EQ(minus_one.convert({64, false})->bits(), ~std::uint64_t{0});
        EXPECT_EQ(minus_one.convert({8, true})->decimal(), "-1");
    }
    EXPECT_EQ(
        IntegerValue::parse_decimal({64, true}, "-9_223_372_036_854_775_808")
            ->decimal(),
        "-9223372036854775808");
    EXPECT_EQ(IntegerValue::parse_decimal({8, true}, "-0")->bits(), 0U);
    for (const auto* spelling : {"-", "--1", "-128x"}) {
        EXPECT_EQ(IntegerValue::parse_decimal({8, true}, spelling).error(),
                  IntegerError::InvalidLiteral);
    }
}

TEST(Integer, SignedArithmeticMatchesSmallIndependentReferenceValues) {
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        const auto mask = ~std::uint64_t{0} >> (64 - width);
        for (std::int64_t left : {-64, -7, -1, 0, 1, 3, 63}) {
            const auto a = *IntegerValue::parse_decimal({width, true},
                                                        std::to_string(left));
            for (std::int64_t right : {-7, -1, 1, 3, 63}) {
                const auto b = *IntegerValue::parse_decimal(
                    {width, true}, std::to_string(right));
                for (const auto& [operation, expected] :
                     {std::pair{IntegerOperation::Add, left + right},
                      {IntegerOperation::Subtract, left - right},
                      {IntegerOperation::Multiply, left * right},
                      {IntegerOperation::Divide, left / right},
                      {IntegerOperation::Remainder, left % right}}) {
                    const auto result = a.apply(operation, b);
                    ASSERT_TRUE(result);
                    EXPECT_EQ(result->bits(),
                              static_cast<std::uint64_t>(expected) & mask);
                }
                EXPECT_EQ(a.compare(b), (left > right) - (left < right));
            }
            EXPECT_EQ(a.apply(IntegerOperation::ShiftRight,
                              *IntegerValue::create({width, true}, 1))
                          ->bits(),
                      static_cast<std::uint64_t>(left >= 0 ? left / 2
                                                           : (left - 1) / 2) &
                          mask);
        }
    }
}

TEST(Integer, WidthsWrapWithoutHostOverflow) {
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        const auto maximum =
            IntegerValue::create({width, false}, ~std::uint64_t{0});
        const auto one = IntegerValue::create({width, false}, 1);
        ASSERT_TRUE(maximum);
        ASSERT_TRUE(one);
        const auto sum = maximum->apply(IntegerOperation::Add, *one);
        ASSERT_TRUE(sum);
        EXPECT_EQ(sum->bits(), 0U);
        EXPECT_EQ(maximum->bit_not().bits(), 0U);
        EXPECT_EQ(sum->apply(IntegerOperation::Subtract, *one), maximum);
    }
}

TEST(Integer, RejectsInvalidWidthsAndMixedTypes) {
    EXPECT_EQ(IntegerValue::create({0, false}, 0).error(),
              IntegerError::InvalidWidth);
    EXPECT_EQ(IntegerValue::create({65, false}, 0).error(),
              IntegerError::InvalidWidth);
    auto byte = *IntegerValue::create({8, false}, 1);
    auto word = *IntegerValue::create({32, false}, 1);
    EXPECT_EQ(byte.apply(IntegerOperation::Add, word).error(),
              IntegerError::TypeMismatch);
    EXPECT_EQ(byte.compare(word).error(), IntegerError::TypeMismatch);
    EXPECT_NE(byte, *IntegerValue::create({8, true}, 1));
}

TEST(Integer, SignedInterpretationIsExplicit) {
    auto negative = *IntegerValue::create({8, true}, 255);
    auto positive = *IntegerValue::create({8, true}, 1);
    EXPECT_EQ(negative.decimal(), "-1");
    EXPECT_EQ(negative.compare(positive), -1);
    EXPECT_EQ(negative.apply(IntegerOperation::ShiftRight, positive), negative);
    auto minimum = *IntegerValue::create({8, true}, 128);
    EXPECT_EQ(minimum.apply(IntegerOperation::Divide, negative).error(),
              IntegerError::SignedOverflow);
}

TEST(Integer, CheckedOperationsFailBeforeLLVMArithmetic) {
    auto one = IntegerValue::from_u32(1);
    auto zero = IntegerValue::from_u32(0);
    for (auto operation :
         {IntegerOperation::Divide, IntegerOperation::Remainder}) {
        EXPECT_EQ(one.apply(operation, zero).error(),
                  IntegerError::DivideByZero);
    }
    for (auto operation :
         {IntegerOperation::ShiftLeft, IntegerOperation::ShiftRight}) {
        EXPECT_EQ(one.apply(operation, IntegerValue::from_u32(32)).error(),
                  IntegerError::InvalidShift);
        EXPECT_EQ(one.apply(operation, IntegerValue::from_u32(~0U)).error(),
                  IntegerError::InvalidShift);
    }
}

TEST(Integer, U32MatchesDefinedUnsignedArithmetic) {
    std::uint32_t state = 12345;
    for (unsigned index = 0; index < 1000; ++index) {
        state = state * 1664525U + 1013904223U;
        const auto left = state;
        state = state * 1664525U + 1013904223U;
        const auto right = state | 1U;
        auto a = IntegerValue::from_u32(left);
        auto b = IntegerValue::from_u32(right);
        EXPECT_EQ(a.apply(IntegerOperation::Add, b)->bits(),
                  static_cast<std::uint32_t>(left + right));
        EXPECT_EQ(a.apply(IntegerOperation::Multiply, b)->bits(),
                  static_cast<std::uint32_t>(left * right));
        EXPECT_EQ(a.apply(IntegerOperation::Divide, b)->bits(), left / right);
        EXPECT_EQ(a.apply(IntegerOperation::Remainder, b)->bits(),
                  left % right);
    }
}

TEST(Integer, DecimalParsingNeverTruncates) {
    EXPECT_EQ(IntegerValue::parse_decimal({8, false}, "2_55")->bits(), 255U);
    EXPECT_EQ(IntegerValue::parse_decimal({8, false}, "256").error(),
              IntegerError::InvalidLiteral);
    EXPECT_EQ(IntegerValue::parse_decimal({64, false}, "18446744073709551615")
                  ->bits(),
              ~std::uint64_t{0});
    EXPECT_EQ(IntegerValue::parse_decimal({64, false}, "18446744073709551616")
                  .error(),
              IntegerError::InvalidLiteral);
    for (const auto* text : {"", "_", "-1", "12bad"}) {
        EXPECT_EQ(IntegerValue::parse_decimal({32, false}, text).error(),
                  IntegerError::InvalidLiteral);
    }
    EXPECT_EQ(IntegerValue::parse_decimal({0, false}, "0").error(),
              IntegerError::InvalidWidth);
}

TEST(Integer, ConversionsPreserveLowBitsAndExtendAccordingToSource) {
    for (unsigned width : {8U, 16U, 32U, 64U}) {
        auto source = *IntegerValue::create({width, false}, ~std::uint64_t{0});
        for (unsigned destination : {8U, 16U, 32U, 64U}) {
            auto converted = source.convert({destination, false});
            ASSERT_TRUE(converted);
            EXPECT_EQ(converted->type(),
                      (pagos::IntegerType{destination, false}));
            EXPECT_EQ(converted->bits(),
                      IntegerValue::create({destination, false}, source.bits())
                          ->bits());
        }
    }
    auto negative = *IntegerValue::create({8, true}, 255);
    EXPECT_EQ(negative.convert({64, false})->bits(), ~std::uint64_t{0});
    EXPECT_EQ(negative.convert({0, false}).error(), IntegerError::InvalidWidth);
}

} // namespace
