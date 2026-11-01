#include "pagos/codegen/llvm_codegen.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <gtest/gtest.h>

#include <string>

namespace {
using namespace pagos;
using codegen::EmissionOptions;
using codegen::LLVMCodegen;
using codegen::OptimizationLevel;

std::expected<mir::Module, std::string>
lower(std::string text, const codegen::TargetLayout& target) {
    const auto source = source::SourceFile::from_text("emission.pgs", text);
    source::DiagnosticEngine diagnostics(source);
    syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    syntax::Parser parser(tokens, diagnostics);
    const auto syntax = parser.parse_module();
    sema::TypeChecker checker(diagnostics, target.pointer_bits());
    if (!checker.check(*syntax) || diagnostics.has_error())
        return std::unexpected("invalid test input");
    stage::StageAnalyzer analyzer(diagnostics, checker.types());
    const auto hir = analyzer.analyze(*syntax);
    if (diagnostics.has_error())
        return std::unexpected("invalid Stage input");
    return mir::lower(*hir);
}

TEST(Emission, LegacyAndO0KeepRawIRWhileO2InlinesResidualCalls) {
    const auto target = codegen::TargetLayout::create(
        {.triple = "riscv32-unknown-elf", .features = "+m,+a,+c"});
    ASSERT_TRUE(target);
    const auto module = lower("fn twice(x: u32) -> u32 { x + x } "
                              "export fn api(x: u32) -> u32 { twice(x) }",
                              *target);
    ASSERT_TRUE(module) << module.error();
    const auto original = LLVMCodegen::emit_for_target(*module, *target);
    const auto raw = LLVMCodegen::emit_for_target(
        *module, *target, {.optimization = OptimizationLevel::O0});
    ASSERT_TRUE(original);
    ASSERT_TRUE(raw);
    EXPECT_EQ(*original, *raw);
    EXPECT_NE(raw->find("call i32 @pagos.twice"), std::string::npos);
    for (const auto level :
         {OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::O3,
          OptimizationLevel::Os, OptimizationLevel::Oz}) {
        const auto ir = LLVMCodegen::emit_for_target(*module, *target,
                                                     {.optimization = level});
        ASSERT_TRUE(ir) << ir.error();
        EXPECT_EQ(ir->find("@pagos.twice"), std::string::npos);
        EXPECT_NE(ir->find("@api("), std::string::npos);
        if (level == OptimizationLevel::Os || level == OptimizationLevel::Oz) {
            EXPECT_NE(ir->find("optsize"), std::string::npos);
        }
        if (level == OptimizationLevel::Oz) {
            EXPECT_NE(ir->find("minsize"), std::string::npos);
        }
    }
}

TEST(Emission, AllLevelsEmitObjectsWithoutChangingSemanticTarget) {
    for (const auto* triple :
         {"x86_64-unknown-linux-gnu", "riscv32-unknown-elf"}) {
        const auto target = codegen::TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target);
        const auto config = target->config();
        const auto module =
            lower("export fn api(x: u32) -> u32 { x + 1 }", *target);
        ASSERT_TRUE(module);
        for (const auto level :
             {OptimizationLevel::Legacy, OptimizationLevel::O0,
              OptimizationLevel::O1, OptimizationLevel::O2,
              OptimizationLevel::O3, OptimizationLevel::Os,
              OptimizationLevel::Oz}) {
            const auto object = LLVMCodegen::emit_object_for_target(
                *module, *target,
                {.optimization = level,
                 .relocation = codegen::RelocationModel::Static,
                 .code_model = codegen::CodeModel::Medium});
            ASSERT_TRUE(object) << object.error();
            EXPECT_TRUE(object->starts_with("\x7f"
                                            "ELF"));
            EXPECT_EQ(target->config(), config);
        }
    }
}

TEST(Emission, RejectsUnsupportedCodeModelBeforeEnteringLLVMBackend) {
    const auto target =
        codegen::TargetLayout::create({.triple = "thumbv7m-none-eabi"});
    ASSERT_TRUE(target);
    const auto module = lower("let result = 42;", *target);
    ASSERT_TRUE(module);
    const auto ir = LLVMCodegen::emit_for_target(
        *module, *target,
        {.optimization = OptimizationLevel::O2,
         .code_model = codegen::CodeModel::Medium});
    ASSERT_FALSE(ir);
    EXPECT_EQ(ir.error(),
              "medium code model requires an x86-64 or RISC-V target");
}

TEST(Emission, OptimizationDoesNotBypassMirOrTargetVerification) {
    const auto target =
        codegen::TargetLayout::create({.triple = "riscv32-unknown-elf"});
    ASSERT_TRUE(target);
    auto module = lower("let result = 42;", *target);
    ASSERT_TRUE(module);
    module->pointer_bits = 64;
    const auto mismatch = LLVMCodegen::emit_for_target(
        *module, *target, {.optimization = OptimizationLevel::O2});
    ASSERT_FALSE(mismatch);
    EXPECT_NE(mismatch.error().find("pointer width"), std::string::npos);
    module->pointer_bits = 32;
    module->functions.front().blocks.clear();
    const auto invalid = LLVMCodegen::emit_for_target(
        *module, *target, {.optimization = OptimizationLevel::O2});
    ASSERT_FALSE(invalid);
    EXPECT_TRUE(invalid.error().starts_with("invalid MIR:"));
}
} // namespace
