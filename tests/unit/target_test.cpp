#include "pagos/codegen/llvm_codegen.h"
#include "pagos/codegen/target.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <gtest/gtest.h>

namespace {

using pagos::codegen::TargetLayout;
using pagos::mir::Type;

TEST(Target, UsizeAnalysisIsSessionLocalAndDoesNotMutateSyntax) {
    using namespace pagos;
    const auto source = source::SourceFile::from_text(
        "usize.pgs", "record R { size: usize } "
                     "fn read(a: [usize; 2]) -> usize { return a[1]; } "
                     "static let maximum: usize = ~0usize; "
                     "static let r = R(size: maximum); "
                     "static let result = read([1usize, r.size]) as u64;");
    source::DiagnosticEngine diagnostics(source);
    syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    syntax::Parser parser(tokens, diagnostics);
    auto syntax = parser.parse_module();
    ASSERT_FALSE(diagnostics.has_error());
    for (const auto* triple : {"riscv32-unknown-elf", "riscv64-unknown-elf",
                               "riscv32-unknown-elf"}) {
        const auto target = TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target);
        const auto bits = target->pointer_bits();
        sema::TypeChecker checker(diagnostics, bits);
        ASSERT_TRUE(checker.check(*syntax));
        stage::StageAnalyzer analyzer(diagnostics, checker.types());
        const auto hir = analyzer.analyze(*syntax);
        ASSERT_FALSE(diagnostics.has_error());
        ASSERT_TRUE(hir->result->constant);
        EXPECT_EQ(std::get<IntegerValue>(*hir->result->constant).decimal(),
                  bits == 32 ? "4294967295" : "18446744073709551615");
        EXPECT_EQ(analyzer.stats().aggregate_bytes_reserved, 3U * bits / 8U);
        EXPECT_EQ(syntax->functions[0]->result, syntax::Type::usize());
        EXPECT_EQ(syntax->records[0].fields[0].type, syntax::Type::usize());
        const auto mir = mir::lower(*hir);
        ASSERT_TRUE(mir) << mir.error();
        EXPECT_EQ(mir->pointer_bits, bits);
        EXPECT_TRUE(codegen::LLVMCodegen::emit_for_target(*mir, *target));
        // The usize dependency has folded into a fixed-width u64 constant.
        const auto wrong = TargetLayout::create(
            {.triple =
                 bits == 32 ? "riscv64-unknown-elf" : "riscv32-unknown-elf"});
        ASSERT_TRUE(wrong);
        const auto rejected =
            codegen::LLVMCodegen::emit_for_target(*mir, *wrong);
        ASSERT_FALSE(rejected);
        EXPECT_NE(rejected.error().find("semantic target pointer width"),
                  std::string::npos);
    }
}

TEST(Target, UsizeLayoutsRejectWrongWidthIncludingAggregates) {
    const auto target = TargetLayout::create({.triple = "riscv32-unknown-elf"});
    ASSERT_TRUE(target);
    for (unsigned bits : {32U, 64U}) {
        const auto scalar = Type::integer(bits, false, true);
        Type record{Type::Record};
        record.record_name = "R";
        record.fields = {Type::Bool, scalar};
        for (const auto& type : {scalar, Type::array(scalar, 2), record}) {
            EXPECT_EQ(target->layout_of(type).has_value(), bits == 32);
        }
    }
}

TEST(Target, ResidualCallSignaturesUseTheSemanticTargetWidth) {
    using namespace pagos;
    const auto source = source::SourceFile::from_text(
        "call.pgs", "fn next(x: usize) -> usize { x + 1usize } "
                    "let a = next(external_input() as usize); "
                    "let result = next(external_input() as usize);");
    source::DiagnosticEngine diagnostics(source);
    syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    syntax::Parser parser(tokens, diagnostics);
    auto syntax = parser.parse_module();
    ASSERT_FALSE(diagnostics.has_error());
    for (const auto* triple : {"riscv32-unknown-elf", "riscv64-unknown-elf",
                               "riscv32-unknown-elf"}) {
        const auto target = TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target);
        sema::TypeChecker checker(diagnostics, target->pointer_bits());
        ASSERT_TRUE(checker.check(*syntax));
        stage::StageAnalyzer analyzer(diagnostics, checker.types());
        auto hir = analyzer.analyze(*syntax);
        ASSERT_FALSE(diagnostics.has_error());
        EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
        EXPECT_EQ(analyzer.stats().residual_cache_hits, 1U);
        const auto module = mir::lower(*hir);
        ASSERT_TRUE(module) << module.error();
        ASSERT_EQ(module->functions.size(), 2U);
        const auto& callee = module->functions[1];
        EXPECT_TRUE(callee.internal);
        ASSERT_EQ(callee.parameters.size(), 1U);
        EXPECT_EQ(callee.parameters[0],
                  Type::integer(target->pointer_bits(), false, true));
        EXPECT_EQ(callee.result_type, callee.parameters[0]);
        const auto ir = codegen::LLVMCodegen::emit_for_target(*module, *target);
        ASSERT_TRUE(ir) << ir.error();
        EXPECT_NE(ir->find("define internal i" +
                           std::to_string(target->pointer_bits())),
                  std::string::npos);
    }
}

TEST(Target, MirRejectsUnboundOrMismatchedUsize) {
    using namespace pagos;
    hir::Module hir;
    hir.pointer_bits = 32;
    hir.result = hir::make_constant(*IntegerValue::create({32, false, true}, 1),
                                    sema::Type::usize(32), {});
    auto module = mir::lower(hir);
    ASSERT_TRUE(module);
    for (unsigned bits : {0U, 16U, 64U}) {
        module->pointer_bits = bits;
        EXPECT_FALSE(mir::verify(*module));
    }
    module->pointer_bits = 32;
    EXPECT_TRUE(mir::verify(*module));
}

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
