#include "pagos/value.h"

#include <gtest/gtest.h>

#include <sstream>

namespace {

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
    EXPECT_FALSE(
        constant_equal(std::vector<bool>{true}, std::vector<std::uint32_t>{1}));
}

TEST(Constant, SharedPrinterPreservesExistingFormat) {
    using namespace pagos;
    std::ostringstream output;
    print_constant(RecordConstant{"Config", {std::uint32_t{42}, true}}, output);
    output << ' ';
    print_constant(std::vector<bool>{false, true}, output);
    output << ' ';
    print_constant(std::vector<std::uint32_t>{1, 2}, output);
    EXPECT_EQ(output.str(), "Config(42, true) [false, true] [1, 2]");
    EXPECT_EQ(from_scalar(ScalarConstant{true}), Constant{true});
}

} // namespace
