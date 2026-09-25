#include "pagos/integer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using pagos::IntegerError;
using pagos::IntegerOperation;
using pagos::IntegerValue;

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

} // namespace
