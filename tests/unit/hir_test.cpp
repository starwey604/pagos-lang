#include "pagos/hir/hir.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

TEST(HirEffects, ComptimeValueCanRetainRuntimeWork) {
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
    EXPECT_EQ(sequence->stage, Stage::Comptime);
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

TEST(RuntimePath, CopiesBranchWithoutChangingDiagnosticOrder) {
    using pagos::hir::RuntimePath;
    RuntimePath prefix{"external_input", "source"};
    auto left = prefix;
    auto right = prefix;
    left.push_back("left");
    right.push_back("right");
    EXPECT_EQ(prefix.to_vector(),
              (std::vector<std::string>{"external_input", "source"}));
    EXPECT_EQ(left.to_vector(),
              (std::vector<std::string>{"external_input", "source", "left"}));
    EXPECT_EQ(right.back(), "right");
    RuntimePath::Accounting storage;
    storage.add(prefix);
    storage.add(left);
    storage.add(right);
    EXPECT_EQ(storage.paths, 3U);
    EXPECT_EQ(storage.logical_steps, 8U);
    EXPECT_EQ(storage.unique_steps, 4U);
}

TEST(RuntimePath, ConcatenationAndSelfAppendPreserveCallSiteSteps) {
    using pagos::hir::RuntimePath;
    RuntimePath empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_TRUE(empty.to_vector().empty());
    RuntimePath caller{"external_input", "caller"};
    const RuntimePath body{"fn.parameter", "result"};
    caller.append(body);
    caller.append(empty);
    EXPECT_EQ(caller.to_vector(),
              (std::vector<std::string>{"external_input", "caller",
                                        "fn.parameter", "result"}));
    caller.append(caller);
    EXPECT_EQ(caller.size(), 8U);
    const auto steps = caller.to_vector();
    EXPECT_TRUE(
        std::equal(steps.begin(), steps.begin() + 4, steps.begin() + 4));
    EXPECT_EQ(body.size(), 2U);
}

TEST(RuntimePath, MovesAndVectorRestoreKeepValidReusablePaths) {
    using pagos::hir::RuntimePath;
    RuntimePath restored(std::vector<std::string>{"source", "parameter"});
    auto moved = std::move(restored);
    EXPECT_TRUE(restored.empty());
    EXPECT_TRUE(restored.to_vector().empty());
    restored.push_back("new source");
    EXPECT_EQ(restored.back(), "new source");
    restored = std::move(moved);
    EXPECT_TRUE(moved.empty());
    EXPECT_EQ(restored.to_vector(),
              (std::vector<std::string>{"source", "parameter"}));
}

TEST(RuntimePath, RetainedPrefixesUseLinearStorageAndReleaseDeepSuffixes) {
    using pagos::hir::RuntimePath;
    constexpr std::size_t count = 10000;
    std::vector<RuntimePath> prefixes;
    RuntimePath path;
    RuntimePath::Accounting storage;
    for (std::size_t index = 0; index < count; ++index) {
        path.push_back("step");
        prefixes.push_back(path);
        storage.add(path);
    }
    EXPECT_EQ(storage.unique_steps, count);
    EXPECT_EQ(storage.logical_steps, count * (count + 1) / 2);
    EXPECT_LT(storage.bytes, count * 128);
    prefixes.clear();
    // Destruction used to be a recursive shared_ptr chain in a naive persistent
    // implementation. Keep an independent shared prefix through suffix release.
    auto prefix = path;
    for (std::size_t index = 0; index < 200000; ++index)
        path.push_back("deep");
    path = RuntimePath{};
    EXPECT_EQ(prefix.size(), count);
    EXPECT_EQ(prefix.to_vector().front(), "step");
}

} // namespace
