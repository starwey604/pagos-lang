#include "pagos/codegen/llvm_codegen.h"
#include "pagos/codegen/target.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

namespace {
using namespace pagos;

struct BoundaryInput {
    explicit BoundaryInput(std::string text, unsigned bits = 64)
        : source(
              source::SourceFile::from_text("boundary.pgs", std::move(text))),
          diagnostics(source), checker(diagnostics, bits) {
        syntax::Lexer lexer(source, diagnostics);
        const auto tokens = lexer.tokenize();
        syntax::Parser parser(tokens, diagnostics);
        syntax = parser.parse_module();
        EXPECT_FALSE(diagnostics.has_error());
        EXPECT_TRUE(checker.check(*syntax));
    }
    source::SourceFile source;
    source::DiagnosticEngine diagnostics;
    sema::TypeChecker checker;
    std::unique_ptr<syntax::Module> syntax;
};

TEST(CAbi, PreservesLinkageAndExternalDeclarationsHaveNoBody) {
    BoundaryInput input("extern fn read(x: i32) -> u32; "
                        "export fn api(x: i32) -> u32 { read(x) } "
                        "fn helper() -> u32 { 1 }");
    ASSERT_EQ(input.syntax->functions.size(), 3U);
    EXPECT_EQ(input.syntax->functions[0]->linkage,
              syntax::Function::Linkage::ExternC);
    EXPECT_EQ(input.syntax->functions[0]->body, nullptr);
    EXPECT_EQ(input.syntax->functions[1]->linkage,
              syntax::Function::Linkage::ExportC);
    EXPECT_NE(input.syntax->functions[1]->body, nullptr);
    EXPECT_EQ(input.syntax->functions[2]->linkage,
              syntax::Function::Linkage::Internal);
}

TEST(CAbi, ExportRootsSurviveAnalyzerResetAndDoNotLeakRecursiveEdges) {
    BoundaryInput input("extern fn read() -> u32; "
                        "export fn api(n: u32) -> u32 { "
                        "if n == 0 { read() } else { api(n - 1) } }");
    std::unique_ptr<hir::Module> first;
    std::weak_ptr<const hir::ResidualFunction> definition;
    {
        stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
        first = analyzer.analyze(*input.syntax);
        ASSERT_FALSE(input.diagnostics.has_error());
        ASSERT_EQ(first->residual_functions.size(), 2U);
        EXPECT_FALSE(first->emit_entry);
        EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
        definition = first->residual_functions[1];
        const auto second = analyzer.analyze(*input.syntax);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
        EXPECT_NE(first->residual_functions[1], second->residual_functions[1]);
    }
    ASSERT_FALSE(definition.expired());
    const auto module = mir::lower(*first);
    ASSERT_TRUE(module) << module.error();
    ASSERT_EQ(module->functions.size(), 2U);
    EXPECT_TRUE(module->functions[0].declaration);
    EXPECT_FALSE(module->functions[1].declaration);
    EXPECT_TRUE(mir::verify(*module));
    first.reset();
    EXPECT_TRUE(definition.expired());
}

TEST(CAbi, CallsAreRuntimeEvenWhenExportedBodyIsConstant) {
    BoundaryInput input(
        "export fn fixed() -> u32 { 42 } let result = fixed();");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_TRUE(hir->emit_entry);
    EXPECT_EQ(hir->result->stage, hir::Stage::Runtime);
    EXPECT_FALSE(hir->result->constant);
    std::ostringstream printed;
    hir::print(*hir, printed);
    EXPECT_NE(printed.str().find("{export C}"), std::string::npos);
    EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
}

TEST(CAbi, FailedExportDoesNotHideIndependentErrorsOrCountAsCompleted) {
    BoundaryInput input("export fn bad(x: u32) -> u32 { static let a = x; 1 } "
                        "export fn other() -> u32 { 1 / 0 } "
                        "export fn good() -> u32 { 7 }");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    EXPECT_EQ(input.diagnostics.size(), 2U);
    EXPECT_EQ(analyzer.stats().residual_specializations, 1U);
    EXPECT_EQ(hir->residual_functions[0]->body, nullptr);
    EXPECT_EQ(hir->residual_functions[1]->body, nullptr);
    EXPECT_NE(hir->residual_functions[2]->body, nullptr);
}

TEST(CAbi, ImportsDoNotConsumeDefinitionBudget) {
    BoundaryInput input("extern fn read() -> u32;");
    stage::AnalysisLimits limits;
    limits.residual_specializations = 0;
    limits.recursion_depth = 0;
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types(),
                                  limits);
    const auto hir = analyzer.analyze(*input.syntax);
    ASSERT_FALSE(input.diagnostics.has_error());
    EXPECT_EQ(analyzer.stats().residual_specializations, 0U);
    EXPECT_TRUE(mir::lower(*hir));
}

TEST(CAbi, VerifierRejectsMalformedExternalSignaturesAndBodies) {
    mir::Module module;
    module.functions.push_back({.name = "read",
                                .result_type = mir::Type::U32,
                                .c_abi = true,
                                .declaration = true});
    ASSERT_TRUE(mir::verify(module));
    auto& function = module.functions[0];
    function.internal = true;
    EXPECT_FALSE(mir::verify(module));
    function.internal = false;
    function.c_abi = false;
    EXPECT_FALSE(mir::verify(module));
    function.c_abi = true;
    function.blocks.emplace_back();
    EXPECT_FALSE(mir::verify(module));
    function.blocks.clear();
    for (auto type : {mir::Type{mir::Type::Bool}, mir::Type::integer(64),
                      mir::Type::integer(32, false, true),
                      mir::Type::array(mir::Type::U32, 2)}) {
        function.parameters = {type};
        EXPECT_FALSE(mir::verify(module));
        function.parameters.clear();
        function.result_type = type;
        EXPECT_FALSE(mir::verify(module));
        function.result_type = mir::Type::U32;
    }
    EXPECT_TRUE(mir::verify(module));
}

TEST(CAbi, TargetWhitelistRejectsUnvalidatedCallingConventions) {
    using codegen::TargetLayout;
    for (const auto* triple :
         {"x86_64-unknown-linux-gnu", "riscv32-unknown-elf"}) {
        const auto target = TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target) << target.error();
        EXPECT_TRUE(target->supports_minimal_c_abi());
    }
    for (const auto* triple :
         {"riscv64-unknown-elf", "arm-none-eabi", "x86_64-pc-windows-msvc",
          "i386-unknown-linux-gnu"}) {
        const auto target = TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target) << target.error();
        EXPECT_FALSE(target->supports_minimal_c_abi());
    }
    for (const auto* features : {"+f", "+e", "+m,+a,+c,+d"}) {
        const auto target = TargetLayout::create(
            {.triple = "riscv32-unknown-elf", .features = features});
        ASSERT_TRUE(target) << target.error();
        EXPECT_FALSE(target->supports_minimal_c_abi());
    }
}

TEST(CAbi, EmitsRealElfObjectsAndRejectsSemanticTargetMismatch) {
    for (const auto* triple :
         {"x86_64-unknown-linux-gnu", "riscv32-unknown-elf"}) {
        const auto target = codegen::TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target) << target.error();
        BoundaryInput input("export fn api(x: i32) -> i32 { x + 1i32 }",
                            target->pointer_bits());
        stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
        const auto hir = analyzer.analyze(*input.syntax);
        ASSERT_FALSE(input.diagnostics.has_error());
        auto module = mir::lower(*hir);
        ASSERT_TRUE(module) << module.error();
        const auto object =
            codegen::LLVMCodegen::emit_object_for_target(*module, *target);
        ASSERT_TRUE(object) << object.error();
        EXPECT_TRUE(object->starts_with("\x7f"
                                        "ELF"));
        module->pointer_bits = target->pointer_bits() == 32 ? 64 : 32;
        EXPECT_FALSE(
            codegen::LLVMCodegen::emit_object_for_target(*module, *target));
    }
}
} // namespace
