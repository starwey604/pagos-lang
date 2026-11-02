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
    BoundaryInput input(
        "export fn bad(x: u32) -> u32 { comptime let a = x; 1 } "
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
         {"x86_64-unknown-linux-gnu", "riscv32-unknown-elf",
          "x86_64-pc-windows-msvc", "x86_64-pc-windows-msvc19.51",
          "x86_64-pc-windows"}) {
        const auto target = TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target) << target.error();
        EXPECT_TRUE(target->supports_minimal_c_abi());
    }
    for (const auto* triple :
         {"riscv64-unknown-elf", "arm-none-eabi", "i386-unknown-linux-gnu",
          "i686-pc-windows-msvc", "x86_64-w64-windows-gnu",
          "x86_64-pc-windows-cygnus", "x86_64-pc-windows-msvc-elf"}) {
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

TEST(CAbi, WindowsX64EmitsCoffWithScalarAndPointerBoundaries) {
    const auto target =
        codegen::TargetLayout::create({.triple = "x86_64-pc-windows-msvc"});
    ASSERT_TRUE(target) << target.error();
    EXPECT_EQ(target->pointer_bits(), 64U);
    BoundaryInput input("extern fn foreign(p: *mut u32, x: i32) -> *mut u32; "
                        "export fn api(p: *mut u32, x: i32) -> *mut u32 { "
                        "foreign(p, x) }");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    ASSERT_FALSE(input.diagnostics.has_error());
    auto module = mir::lower(*hir);
    ASSERT_TRUE(module) << module.error();
    const auto ir = codegen::LLVMCodegen::emit_for_target(*module, *target);
    ASSERT_TRUE(ir) << ir.error();
    EXPECT_NE(ir->find("declare ptr @foreign(ptr, i32)"), std::string::npos);
    EXPECT_NE(ir->find("define ptr @api(ptr"), std::string::npos);
    EXPECT_EQ(ir->find("@pagos_main"), std::string::npos);
    for (auto level :
         {codegen::OptimizationLevel::O0, codegen::OptimizationLevel::O2}) {
        const auto object = codegen::LLVMCodegen::emit_object_for_target(
            *module, *target, {.optimization = level});
        ASSERT_TRUE(object) << object.error();
        EXPECT_TRUE(object->starts_with("\x64\x86"));
    }
    const auto mingw =
        codegen::TargetLayout::create({.triple = "x86_64-w64-windows-gnu"});
    ASSERT_TRUE(mingw) << mingw.error();
    EXPECT_FALSE(codegen::LLVMCodegen::emit_for_target(*module, *mingw));
    module->pointer_bits = 32;
    EXPECT_FALSE(
        codegen::LLVMCodegen::emit_object_for_target(*module, *target));
}

TEST(CAbi, SharedLibraryPolicyPreservesTheSameBoundaryAndPrivateStorage) {
    BoundaryInput input("static mut state: u32 = 17; "
                        "extern fn foreign(x: u32) -> u32; "
                        "fn helper(x: u32) -> u32 { x + 1 } "
                        "export fn api(x: u32) -> u32 { foreign(helper(x)) } "
                        "export fn address() -> *mut u32 { &state }");
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto module = mir::lower(*hir);
    ASSERT_TRUE(module) << module.error();
    for (const auto* triple :
         {"x86_64-unknown-linux-gnu", "x86_64-pc-windows-msvc"}) {
        SCOPED_TRACE(triple);
        const auto target = codegen::TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target) << target.error();
        EXPECT_TRUE(target->supports_shared_c_library());
        const bool windows =
            std::string_view(triple).find("windows") != std::string_view::npos;
        const auto ir = codegen::LLVMCodegen::emit_for_target(
            *module, *target, {.shared_library = true});
        ASSERT_TRUE(ir) << ir.error();
        EXPECT_NE(ir->find(windows ? "define dllexport i32 @api("
                                   : "define i32 @api("),
                  std::string::npos);
        EXPECT_NE(ir->find("declare i32 @foreign(i32)"), std::string::npos);
        EXPECT_NE(ir->find("define internal i32 @pagos.helper."),
                  std::string::npos);
        EXPECT_NE(ir->find("@pagos.global.state = internal global i32 17"),
                  std::string::npos);
        EXPECT_EQ(ir->find("dllimport"), std::string::npos);
        EXPECT_NE(ir->find("\"PIC Level\", i32 2"), std::string::npos);
        for (auto level :
             {codegen::OptimizationLevel::O0, codegen::OptimizationLevel::O2}) {
            const auto object = codegen::LLVMCodegen::emit_object_for_target(
                *module, *target,
                {.optimization = level, .shared_library = true});
            ASSERT_TRUE(object) << object.error();
            if (windows) {
                EXPECT_NE(object->find("/EXPORT:api"), std::string::npos);
                EXPECT_NE(object->find("/EXPORT:address"), std::string::npos);
                EXPECT_EQ(object->find("/EXPORT:foreign"), std::string::npos);
            }
        }
        const auto ordinary =
            codegen::LLVMCodegen::emit_for_target(*module, *target);
        ASSERT_TRUE(ordinary) << ordinary.error();
        EXPECT_EQ(ordinary->find("dllexport"), std::string::npos);
        EXPECT_FALSE(codegen::LLVMCodegen::emit_for_target(
            *module, *target,
            {.relocation = codegen::RelocationModel::Static,
             .shared_library = true}));
    }
    for (const auto* triple :
         {"riscv32-unknown-elf", "x86_64-w64-windows-gnu"}) {
        const auto target = codegen::TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target) << target.error();
        EXPECT_FALSE(target->supports_shared_c_library());
        EXPECT_FALSE(codegen::LLVMCodegen::emit_for_target(
            *module, *target, {.shared_library = true}));
    }
}

TEST(CAbi, SharedLibraryRejectsImplicitEntryAndMissingExports) {
    for (const auto* source :
         {"extern fn foreign() -> u32;",
          "export fn api() -> u32 { 42 } let result = api();"}) {
        BoundaryInput input(source);
        stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
        const auto hir = analyzer.analyze(*input.syntax);
        ASSERT_FALSE(input.diagnostics.has_error());
        const auto module = mir::lower(*hir);
        ASSERT_TRUE(module) << module.error();
        const auto target =
            codegen::TargetLayout::create({.triple = "x86_64-pc-windows-msvc"});
        ASSERT_TRUE(target) << target.error();
        EXPECT_FALSE(codegen::LLVMCodegen::emit_for_target(
            *module, *target, {.shared_library = true}));
    }
}

TEST(CAbi, CortexM33RequiresThumbMainlineBaseEabiAndDefaultFeatures) {
    using codegen::TargetConfig;
    using codegen::TargetLayout;
    const auto supported = TargetLayout::create(
        {.triple = "thumbv8m.main-none-eabi", .cpu = "cortex-m33"});
    ASSERT_TRUE(supported);
    EXPECT_TRUE(supported->supports_minimal_c_abi());
    EXPECT_EQ(supported->pointer_bits(), 32U);
    EXPECT_TRUE(supported->little_endian());
    for (const auto& config : {
             TargetConfig{.triple = "thumbv8m.main-none-eabihf",
                          .cpu = "cortex-m33"},
             TargetConfig{.triple = "armv8m.main-none-eabi",
                          .cpu = "cortex-m33"},
             TargetConfig{.triple = "thumbv7m-none-eabi", .cpu = "cortex-m3"},
             TargetConfig{.triple = "thumbv8m.main-none-eabi",
                          .cpu = "cortex-m4"},
             TargetConfig{.triple = "thumbv8m.main-none-eabi",
                          .cpu = "cortex-m33",
                          .features = "+dsp"},
         }) {
        const auto target = TargetLayout::create(config);
        ASSERT_TRUE(target) << target.error();
        EXPECT_FALSE(target->supports_minimal_c_abi()) << config.triple;
    }
    BoundaryInput input("export fn api(x: u32) -> u32 { x + 1 }", 32);
    stage::StageAnalyzer analyzer(input.diagnostics, input.checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    ASSERT_FALSE(input.diagnostics.has_error());
    const auto module = mir::lower(*hir);
    ASSERT_TRUE(module);
    for (const auto level :
         {codegen::OptimizationLevel::Legacy, codegen::OptimizationLevel::O0,
          codegen::OptimizationLevel::O1, codegen::OptimizationLevel::O2,
          codegen::OptimizationLevel::O3, codegen::OptimizationLevel::Os,
          codegen::OptimizationLevel::Oz}) {
        const auto object = codegen::LLVMCodegen::emit_object_for_target(
            *module, *supported,
            {.optimization = level,
             .relocation = codegen::RelocationModel::Static});
        ASSERT_TRUE(object) << object.error();
        EXPECT_TRUE(object->starts_with("\x7f"
                                        "ELF"));
        ASSERT_GE(object->size(), 40U);
        EXPECT_EQ(static_cast<unsigned char>((*object)[4]), 1U);   // ELF32.
        EXPECT_EQ(static_cast<unsigned char>((*object)[18]), 40U); // EM_ARM.
    }
}
} // namespace
