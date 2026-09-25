#include "pagos/hir/hir.h"

#include <gtest/gtest.h>

namespace {

TEST(HirEffects, StaticValueCanRetainRuntimeWork) {
    using namespace pagos::hir;
    const auto value =
        make_constant(std::uint32_t{7}, pagos::sema::TypeKind::Integer, {});
    EXPECT_FALSE(has_residual_work(value));
    EXPECT_TRUE(is_cacheable_result(value));
    auto input = std::make_shared<Expr>();
    input->kind = Expr::Kind::ExternalInput;
    input->stage = Stage::Runtime;
    auto sequence = std::make_shared<Expr>(*value);
    sequence->kind = Expr::Kind::Sequence;
    sequence->operands = {input, value};
    EXPECT_EQ(sequence->stage, Stage::Static);
    EXPECT_TRUE(has_residual_work(sequence));
    EXPECT_FALSE(is_cacheable_result(sequence));
    auto alias = std::make_shared<Expr>(*value);
    alias->kind = Expr::Kind::Reference;
    alias->operands = {sequence};
    EXPECT_TRUE(has_residual_work(alias));
    EXPECT_FALSE(is_cacheable_result(alias));
}

TEST(HirEffects, ReturnAndIncompleteValuesAreNotCacheable) {
    using namespace pagos::hir;
    EXPECT_FALSE(has_residual_work(nullptr));
    EXPECT_FALSE(is_cacheable_result(nullptr));
    auto value = std::make_shared<Expr>();
    EXPECT_FALSE(is_cacheable_result(value));
    value->constant = std::uint32_t{7};
    value->may_return = true;
    EXPECT_TRUE(has_residual_work(value));
    EXPECT_FALSE(is_cacheable_result(value));
    value->may_return = false;
    value->falls_through = false;
    EXPECT_FALSE(is_cacheable_result(value));
}

} // namespace
