#include "pagos/codegen/llvm_codegen.h"
#include "pagos/codegen/target.h"

#include <gtest/gtest.h>

namespace {

using pagos::codegen::TargetLayout;
using pagos::mir::Type;

TEST(Target, PointerWidthsAreSessionLocal) {
    auto rv32 = TargetLayout::create({.triple = "riscv32-unknown-elf"});
    auto rv64 = TargetLayout::create({.triple = "riscv64-unknown-elf"});
    auto host = TargetLayout::create();
    ASSERT_TRUE(rv32) << rv32.error();
    ASSERT_TRUE(rv64) << rv64.error();
    ASSERT_TRUE(host) << host.error();
    EXPECT_EQ(rv32->pointer_bits(), 32U);
    EXPECT_EQ(rv64->pointer_bits(), 64U);
    EXPECT_EQ(rv32->pointer_bits(), 32U);
    EXPECT_TRUE(rv32->little_endian());
    EXPECT_FALSE(host->config().triple.empty());
    EXPECT_NE(rv32->config(), rv64->config());
}

TEST(Target, ScalarArrayAndRecordLayoutUsesTargetAlignment) {
    auto x86 = TargetLayout::create({.triple = "i386-unknown-linux-gnu"});
    auto rv32 = TargetLayout::create({.triple = "riscv32-unknown-elf"});
    auto arm = TargetLayout::create(
        {.triple = "thumbv7m-none-eabi", .cpu = "cortex-m3"});
    ASSERT_TRUE(x86) << x86.error();
    ASSERT_TRUE(rv32) << rv32.error();
    ASSERT_TRUE(arm) << arm.error();
    EXPECT_EQ(arm->pointer_bits(), 32U);
    Type record{Type::Record};
    record.record_name = "AlignmentProbe";
    record.fields = {Type::integer(8), Type::integer(64)};
    const auto x86_layout = x86->layout_of(record);
    const auto rv32_layout = rv32->layout_of(record);
    ASSERT_TRUE(x86_layout);
    ASSERT_TRUE(rv32_layout);
    EXPECT_EQ(x86_layout->field_offsets, (std::vector<std::uint64_t>{0, 4}));
    EXPECT_EQ(x86_layout->size_bytes, 12U);
    EXPECT_EQ(rv32_layout->field_offsets, (std::vector<std::uint64_t>{0, 8}));
    EXPECT_EQ(rv32_layout->size_bytes, 16U);
    EXPECT_EQ(rv32_layout->alignment_bytes, 8U);
    EXPECT_EQ(rv32->layout_of(Type::array(Type::integer(16), 7))->size_bytes,
              14U);
    EXPECT_EQ(rv32->layout_of(Type::Bool)->size_bytes, 1U);
    EXPECT_FALSE(rv32->layout_of(Type::array(Type::U32, 0)));
}

TEST(Target, RejectsUnknownTargetsCpuAndFeatures) {
    EXPECT_FALSE(TargetLayout::create({.triple = "not-a-target"}));
    EXPECT_FALSE(TargetLayout::create(
        {.triple = "riscv32-unknown-elf", .features = "+64bit"}));
    EXPECT_FALSE(TargetLayout::create(
        {.triple = "riscv64-unknown-elf", .features = "-64bit"}));
    EXPECT_FALSE(TargetLayout::create(
        {.triple = "riscv32-unknown-elf", .features = "+zfinx,+f"}));
    EXPECT_FALSE(TargetLayout::create(
        {.triple = "riscv32-unknown-elf", .cpu = "generic-rv64"}));
    EXPECT_FALSE(TargetLayout::create(
        {.triple = "riscv32-unknown-elf", .cpu = "cortex-m3"}));
    for (const auto* features :
         {"+nonexistent", "m", "+m,+m", "+m,-m", "+m,", ",+m"}) {
        EXPECT_FALSE(TargetLayout::create(
            {.triple = "riscv32-unknown-elf", .features = features}));
    }
}

TEST(Target, EmissionCarriesMatchingTripleLayoutAndFeatures) {
    pagos::mir::Module module;
    module.functions.push_back(
        {.name = "pagos_main",
         .result_type = Type::U32,
         .entry = 0,
         .blocks = {{.id = 0,
                     .name = "entry",
                     .instructions = {{.result = 0,
                                       .type = Type::U32,
                                       .operation =
                                           pagos::mir::ConstantOperation{
                                               std::uint32_t{42}},
                                       .span = {}}},
                     .terminator = pagos::mir::Return{0}}}});
    for (const auto* triple : {"riscv32-unknown-elf", "riscv64-unknown-elf",
                               "riscv32-unknown-elf"}) {
        const auto target =
            TargetLayout::create({.triple = triple, .features = "+m,+a,+c"});
        ASSERT_TRUE(target);
        const auto ir =
            pagos::codegen::LLVMCodegen::emit(module, target->config());
        ASSERT_TRUE(ir) << ir.error();
        EXPECT_NE(
            ir->find("target datalayout = \"" + target->data_layout() + "\""),
            std::string::npos);
        EXPECT_NE(
            ir->find("target triple = \"" + target->config().triple + "\""),
            std::string::npos);
        EXPECT_NE(ir->find("\"target-features\"=\"+m,+a,+c\""),
                  std::string::npos);
    }
}

} // namespace
