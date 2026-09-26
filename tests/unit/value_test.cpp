#include "pagos/value.h"

#include <gtest/gtest.h>

#include <sstream>

namespace {

TEST(Constant, IntegerIdentityIncludesWidthAndSignednessInAggregates) {
    using namespace pagos;
    const auto byte = *IntegerValue::create({8, false}, 1);
    const auto word = *IntegerValue::create({64, false}, 1);
    const auto signed_byte = *IntegerValue::create({8, true}, 1);
    for (const auto other : {word, signed_byte}) {
        for (const auto& pair :
             {std::pair<Constant, Constant>{byte, other},
              {std::vector<IntegerValue>{byte},
               std::vector<IntegerValue>{other}},
              {RecordConstant{"R", {byte}}, RecordConstant{"R", {other}}}}) {
            EXPECT_FALSE(constant_equal(pair.first, pair.second));
            EXPECT_NE(constant_hash(pair.first), constant_hash(pair.second));
        }
    }
}

TEST(Constant, IdentityIncludesNominalTypeAndScalarTypes) {
    using namespace pagos;
    Constant left = RecordConstant{"A", {std::uint32_t{1}, true}};
    Constant same = RecordConstant{"A", {std::uint32_t{1}, true}};
    EXPECT_TRUE(constant_equal(left, same));
    EXPECT_EQ(constant_hash(left), constant_hash(same));
    EXPECT_FALSE(
        constant_equal(left, RecordConstant{"B", {std::uint32_t{1}, true}}));
    EXPECT_FALSE(
        constant_equal(left, RecordConstant{"A", {true, std::uint32_t{1}}}));
    EXPECT_FALSE(constant_equal(std::vector<bool>{true},
                                std::vector<pagos::IntegerValue>{1}));
}

TEST(Constant, SharedPrinterPreservesExistingFormat) {
    using namespace pagos;
    std::ostringstream output;
    print_constant(RecordConstant{"Config", {std::uint32_t{42}, true}}, output);
    output << ' ';
    print_constant(std::vector<bool>{false, true}, output);
    output << ' ';
    print_constant(std::vector<pagos::IntegerValue>{1, 2}, output);
    EXPECT_EQ(output.str(), "Config(42, true) [false, true] [1, 2]");
    EXPECT_EQ(from_scalar(ScalarConstant{true}), Constant{true});
}

} // namespace
