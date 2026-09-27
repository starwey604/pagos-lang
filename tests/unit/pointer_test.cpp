#include "pagos/codegen/llvm_codegen.h"
#include "pagos/codegen/target.h"
#include "pagos/mir/lowering.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"
#include "pagos/vm/evaluator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>

namespace {
using namespace pagos;

struct PointerInput {
    explicit PointerInput(std::string text)
        : source(source::SourceFile::from_text("pointer.pgs", std::move(text))),
          diagnostics(source) {
        syntax::Lexer lexer(source, diagnostics);
        const auto tokens = lexer.tokenize();
        syntax::Parser parser(tokens, diagnostics);
        syntax = parser.parse_module();
        EXPECT_FALSE(diagnostics.has_error());
    }
    source::SourceFile source;
    source::DiagnosticEngine diagnostics;
    std::unique_ptr<syntax::Module> syntax;
};

TEST(Pointer, TypeIdentityIncludesPointeePermissionAndTarget) {
    const auto source = syntax::Type::pointer(syntax::Type::usize(), true);
    EXPECT_EQ(sema::Type(source), sema::TypeKind::Error);
    for (unsigned bits : {32U, 64U}) {
        const sema::Type resolved(source, bits);
        EXPECT_TRUE(resolved.is_pointer());
        EXPECT_EQ(resolved.pointer_bits, bits);
        EXPECT_EQ(resolved.pointee_type(), sema::Type::usize(bits));
        EXPECT_NE(resolved,
                  sema::Type::pointer(sema::Type::usize(bits), false, bits));
        EXPECT_NE(resolved,
                  sema::Type::pointer(sema::Type::integer(bits), true, bits));
        EXPECT_NE(resolved, sema::Type(source, bits == 32 ? 64 : 32));
        const auto concrete = mir::Type::pointer(
            mir::Type::integer(bits, false, true), true, bits);
        EXPECT_TRUE(concrete.valid());
        EXPECT_TRUE(concrete.matches_pointer_width(bits));
        EXPECT_FALSE(concrete.matches_pointer_width(bits == 32 ? 64 : 32));
    }
    EXPECT_FALSE(mir::Type::pointer(mir::Type::Bool, true, 64).valid());
    EXPECT_FALSE(mir::Type::pointer(mir::Type::U32, true, 16).valid());
}

TEST(Pointer, ConstantsAreTargetAddressesNotIntegerOrHostPointerPayloads) {
    const auto address = IntegerValue::create({64, false, true}, 4096).value();
    const Constant pointer = PointerConstant{address};
    EXPECT_NE(pointer, Constant{address});
    EXPECT_EQ(constant_hash(pointer),
              constant_hash(Constant{PointerConstant{address}}));
    const Constant narrow =
        PointerConstant{IntegerValue::create({32, false, true}, 4096).value()};
    EXPECT_NE(pointer, narrow);
    EXPECT_NE(constant_hash(pointer), constant_hash(narrow));
    std::ostringstream output;
    print_constant(pointer, output);
    EXPECT_EQ(output.str(), "ptr(4096)");
}

TEST(Pointer, SourcePointersResolveIndependentlyAcrossAnalysisSessions) {
    PointerInput input("fn keep(p: *mut usize) -> *mut usize { p } "
                       "static let p = keep((~0usize) as *mut usize); "
                       "static let result = p as usize;");
    for (const auto* triple : {"riscv32-unknown-elf", "riscv64-unknown-elf",
                               "riscv32-unknown-elf"}) {
        const auto target = codegen::TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target);
        sema::TypeChecker checker(input.diagnostics, target->pointer_bits());
        ASSERT_TRUE(checker.check(*input.syntax));
        stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
        const auto hir = analyzer.analyze(*input.syntax);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(std::get<IntegerValue>(*hir->result->constant).decimal(),
                  target->pointer_bits() == 32 ? "4294967295"
                                               : "18446744073709551615");
        EXPECT_EQ(input.syntax->functions[0]->result.integer_width, 0U);
        const auto concrete = mir::Type::pointer(mir::Type::integer(8), true,
                                                 target->pointer_bits());
        const auto layout = target->layout_of(concrete);
        ASSERT_TRUE(layout);
        EXPECT_EQ(layout->size_bytes, target->pointer_bits() / 8U);
    }
}

TEST(Pointer, StaticAddressesSpecializeButReadsAreNeverMemoized) {
    PointerInput input(
        "fn read(p: *const u32) -> u32 { *p } "
        "let a = read(4096usize as *const u32); "
        "let b = read(8192usize as *const u32); "
        "let c = read(4096usize as *const u32); let result = a + b + c;");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    for (int iteration = 0; iteration < 2; ++iteration) {
        const auto hir = analyzer.analyze(*input.syntax);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(hir->result->stage, hir::Stage::Runtime);
        EXPECT_EQ(analyzer.stats().residual_specializations, 2U);
        EXPECT_EQ(analyzer.stats().residual_cache_hits, 1U);
        EXPECT_EQ(analyzer.stats().cache_hits, 0U);
        const auto module = mir::lower(*hir);
        ASSERT_TRUE(module) << module.error();
        EXPECT_EQ(module->functions.size(), 3U);
    }
}

TEST(Pointer, MirRejectsMalformedMemoryOperationsAndVoidUses) {
    PointerInput input("export fn api(p: *mut u32) -> u32 { *p = 7; *p }");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    const auto good = mir::lower(*hir);
    ASSERT_TRUE(good) << good.error();
    for (int case_id = 0; case_id < 6; ++case_id) {
        auto bad = *good;
        auto& function = bad.functions.front();
        auto& instructions = function.blocks.front().instructions;
        auto store =
            std::ranges::find_if(instructions, [](const auto& instruction) {
                return std::holds_alternative<mir::StoreOperation>(
                    instruction.operation);
            });
        ASSERT_NE(store, instructions.end());
        auto& operation = std::get<mir::StoreOperation>(store->operation);
        switch (case_id) {
        case 0:
            store->type = mir::Type::U32;
            break;
        case 1:
            operation.value = store->result;
            break;
        case 2:
            operation.pointer = 999;
            break;
        case 3:
            function.parameters[0].pointer_mutable = false;
            instructions[0].type.pointer_mutable = false;
            break;
        case 4:
            function.blocks[0].terminator = mir::Return{store->result};
            break;
        case 5:
            bad.pointer_bits = 32;
            break;
        }
        EXPECT_FALSE(mir::verify(bad)) << case_id;
    }
}

TEST(Pointer, MirRejectsWrongAddressPayloadAndPermissionEscalation) {
    PointerInput input("let p = 4096usize as *const u32; let result = *p;");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    auto module = mir::lower(*hir);
    ASSERT_TRUE(module) << module.error();
    auto& instructions = module->functions[0].blocks[0].instructions;
    auto& constant =
        std::get<mir::ConstantOperation>(instructions[0].operation);
    const auto saved = constant.value;
    constant.value =
        PointerConstant{IntegerValue::create({32, false, true}, 4096).value()};
    EXPECT_FALSE(mir::verify(*module));
    constant.value = IntegerValue::create({64, false, true}, 4096).value();
    EXPECT_FALSE(mir::verify(*module));
    constant.value = saved;
    auto destination = instructions[0].type;
    destination.pointer_mutable = true;
    instructions.push_back(
        {.result = 99,
         .type = destination,
         .operation = mir::PointerCastOperation{instructions[0].result}});
    EXPECT_FALSE(mir::verify(*module));
}

TEST(Pointer, StaticEvaluatorNeverDereferencesEvenANumericAddress) {
    const auto result = vm::Evaluator::unary(
        syntax::UnaryOperator::Dereference,
        PointerConstant{IntegerValue::create({64, false, true}, 4096).value()});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "E4013");
}

TEST(Volatile, StaticResultsNeverSuppressRepeatedStores) {
    PointerInput input(
        "fn set(p: *mut u32) -> u32 { volatile *p = 7; 1 } "
        "let p = 4096usize as *mut u32; "
        "static let a = set(p); static let b = set(p); let result = a + b;");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    for (int session = 0; session < 2; ++session) {
        const auto hir = analyzer.analyze(*input.syntax);
        ASSERT_FALSE(input.diagnostics.has_error());
        EXPECT_EQ(analyzer.stats().cache_hits, 0U);
        const auto module = mir::lower(*hir);
        ASSERT_TRUE(module) << module.error();
        unsigned stores = 0;
        for (const auto& function : module->functions)
            for (const auto& block : function.blocks)
                for (const auto& instruction : block.instructions)
                    if (const auto* store = std::get_if<mir::StoreOperation>(
                            &instruction.operation))
                        stores += store->is_volatile ? 1U : 0U;
        EXPECT_EQ(stores, 2U);
    }
}

TEST(Volatile, MirRejectsWideLoadAndStoreOnNarrowTarget) {
    PointerInput input("export fn sample(p: *mut u64) -> u32 { "
                       "volatile *p = 1u64; volatile *p; 0 }");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    const auto good = mir::lower(*hir);
    ASSERT_TRUE(good) << good.error();
    for (bool load_case : {false, true}) {
        auto bad = *good;
        bad.pointer_bits = 32;
        auto& function = bad.functions.front();
        function.parameters[0].pointer_bits = 32;
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.type.is_pointer())
                    instruction.type.pointer_bits = 32;
                if (auto* store = std::get_if<mir::StoreOperation>(
                        &instruction.operation))
                    store->is_volatile = !load_case;
            }
        }
        const auto result = mir::verify(bad);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), "invalid MIR volatile access width");
    }
}

TEST(Volatile, StaticEvaluatorRejectsVolatileDereference) {
    const auto result = vm::Evaluator::unary(
        syntax::UnaryOperator::VolatileDereference,
        PointerConstant{IntegerValue::create({64, false, true}, 4096).value()});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "E4013");
}

TEST(GlobalStorage, TargetInitializersAndSymbolicAddressesSurviveSessionReset) {
    PointerInput input(
        "global mut size: usize = ~0usize; let p = &size; let result = size;");
    for (unsigned bits : {32U, 64U, 32U}) {
        sema::TypeChecker checker(input.diagnostics, bits);
        ASSERT_TRUE(checker.check(*input.syntax));
        stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
        for (int iteration = 0; iteration < 2; ++iteration) {
            const auto hir = analyzer.analyze(*input.syntax);
            ASSERT_FALSE(input.diagnostics.has_error());
            ASSERT_EQ(hir->globals.size(), 1U);
            EXPECT_EQ(
                std::get<IntegerValue>(hir->globals[0].initializer).type(),
                (IntegerType{bits, false, true}));
            auto address = hir->bindings[0].value;
            while (address->kind == hir::Expr::Kind::Reference)
                address = address->operands.front();
            EXPECT_EQ(address->kind, hir::Expr::Kind::GlobalAddress);
            EXPECT_FALSE(hir->bindings[0].value->constant);
            EXPECT_EQ(hir->result->stage, hir::Stage::Runtime);
            const auto module = mir::lower(*hir);
            ASSERT_TRUE(module) << module.error();
            EXPECT_EQ(module->globals.size(), 1U);
            EXPECT_EQ(module->pointer_bits, bits);
        }
    }
}

TEST(GlobalStorage, VerifierRejectsMalformedStorageAndReferences) {
    PointerInput input("global const data: u32 = 17; export fn address() -> "
                       "*const u32 { &data }");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    const auto good = mir::lower(*hir);
    ASSERT_TRUE(good) << good.error();
    for (int case_id = 0; case_id < 7; ++case_id) {
        auto bad = *good;
        auto& instruction = bad.functions[0].blocks[0].instructions[0];
        switch (case_id) {
        case 0:
            bad.globals.push_back(bad.globals.front());
            break;
        case 1:
            bad.globals[0].initializer =
                IntegerValue::create({16, false, false}, 17).value();
            break;
        case 2:
            bad.globals[0].type = mir::Type::Bool;
            break;
        case 3:
            std::get<mir::GlobalAddressOperation>(instruction.operation).name =
                "missing";
            break;
        case 4:
            instruction.type.pointer_mutable = true;
            bad.functions[0].result_type.pointer_mutable = true;
            break;
        case 5:
            bad.globals[0].name = bad.functions[0].name;
            break;
        case 6:
            bad.pointer_bits = 0;
            break;
        }
        EXPECT_FALSE(mir::verify(bad)) << case_id;
    }
}

TEST(GlobalStorage, GlobalOnlyModuleHasNoSyntheticEntry) {
    PointerInput input("global mut word: u32 = 0;");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    EXPECT_FALSE(hir->emit_entry);
    const auto module = mir::lower(*hir);
    ASSERT_TRUE(module) << module.error();
    EXPECT_TRUE(module->functions.empty());
    EXPECT_EQ(module->globals.size(), 1U);
}

TEST(GlobalStorage, InitializersRejectRuntimeWorkEvenWithAStaticResult) {
    PointerInput input(
        "fn init() -> u32 { *&data = 7; 1 } global mut data: u32 = init();");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    EXPECT_TRUE(hir->globals.empty());
    ASSERT_TRUE(input.diagnostics.has_error());
    EXPECT_EQ(input.diagnostics.diagnostics().back().code, "E2010");
}

TEST(GlobalStorage, PlacementSurvivesTargetAndAnalyzerSessionChanges) {
    PointerInput input(
        "global mut size: usize align(64) section(\".data.size\") = ~0usize; "
        "global mut zero: u32 section(\".bss.zero\") = 7 - 7; "
        "global const small: u8 align(32) = 1u8;");
    for (const auto* triple : {"riscv32-unknown-elf", "riscv64-unknown-elf",
                               "riscv32-unknown-elf"}) {
        const auto target = codegen::TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target);
        sema::TypeChecker checker(input.diagnostics, target->pointer_bits());
        ASSERT_TRUE(checker.check(*input.syntax));
        stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
        for (int session = 0; session < 2; ++session) {
            const auto hir = analyzer.analyze(*input.syntax);
            ASSERT_FALSE(input.diagnostics.has_error());
            ASSERT_EQ(hir->globals.size(), 3U);
            EXPECT_EQ(hir->globals[0].storage.section, ".data.size");
            EXPECT_EQ(hir->globals[0].storage.alignment, 64U);
            const auto module = mir::lower(*hir);
            ASSERT_TRUE(module) << module.error();
            EXPECT_EQ(
                std::get<IntegerValue>(module->globals[1].initializer).bits(),
                0U);
            EXPECT_EQ(module->globals[1].storage.section, ".bss.zero");
            const auto ir =
                codegen::LLVMCodegen::emit_for_target(*module, *target);
            ASSERT_TRUE(ir) << ir.error();
            EXPECT_NE(ir->find("section \".data.size\", align 64"),
                      std::string::npos);
            EXPECT_NE(ir->find("internal constant i8 1, align 32"),
                      std::string::npos);
        }
    }
}

TEST(GlobalStorage, VerifierRejectsInvalidPlacementBeforeLLVM) {
    PointerInput input("global mut data: u32 section(\".data.state\") = 7;");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    const auto good = mir::lower(*hir);
    ASSERT_TRUE(good);
    for (const auto& storage : {
             GlobalStorage{.section = ".rodata.bad"},
             GlobalStorage{.section = ".bss.nonzero"},
             GlobalStorage{.section = ".data."},
             GlobalStorage{.section = ".text.bad"},
             GlobalStorage{.section = ".data.rel.ro.state"},
             GlobalStorage{.section = ".bss.rel.ro"},
             GlobalStorage{.section = ".data.a\nb"},
             GlobalStorage{.section = std::string(".data.a\0b", 9)},
             GlobalStorage{.alignment = 3},
             GlobalStorage{.alignment = 8192},
         }) {
        auto bad = *good;
        bad.globals[0].storage = storage;
        const auto valid = mir::verify(bad);
        ASSERT_FALSE(valid);
        EXPECT_EQ(valid.error(), "invalid MIR global storage attributes");
        const auto ir = codegen::LLVMCodegen::emit(bad);
        ASSERT_FALSE(ir);
        EXPECT_EQ(ir.error(),
                  "invalid MIR: invalid MIR global storage attributes");
    }
}

TEST(GlobalStorage, ParserRejectsInvalidOrRepeatedAttributes) {
    for (const auto* attribute : {
             "align(0)",
             "align(3)",
             "align(8192)",
             "align(4294967296)",
             "align(99999999999999999999999)",
             "align(4u32)",
             "align(-1)",
             "align(1 + 1)",
             "align(value)",
             "align(4) align(8)",
             "section(\".data.a\") section(\".data.b\")",
             "section(\"\")",
             "section(name)",
             "section(\".data.a\"",
             "unknown(4)",
         }) {
        SCOPED_TRACE(attribute);
        const auto source = source::SourceFile::from_text(
            "attributes.pgs",
            std::string("global mut data: u32 ") + attribute + " = 0;");
        source::DiagnosticEngine diagnostics(source);
        syntax::Lexer lexer(source, diagnostics);
        const auto tokens = lexer.tokenize();
        syntax::Parser parser(tokens, diagnostics);
        const auto module = parser.parse_module();
        EXPECT_TRUE(diagnostics.has_error());
    }
}

TEST(GlobalStorage, QuotedMetadataDoesNotIntroduceStringExpressions) {
    for (const auto* text :
         {"let text = \"hello\";",
          "global mut data: u32 section(\"unterminated",
          "global mut data: u32 section(\"line\nbreak\") = 0;"}) {
        const auto source = source::SourceFile::from_text("quoted.pgs", text);
        source::DiagnosticEngine diagnostics(source);
        syntax::Lexer lexer(source, diagnostics);
        const auto tokens = lexer.tokenize();
        syntax::Parser parser(tokens, diagnostics);
        const auto module = parser.parse_module();
        EXPECT_TRUE(diagnostics.has_error());
    }
}
TEST(GlobalArrays, TargetInitializersAndElementAddressesSurviveSessionReset) {
    PointerInput input("global mut data: [usize; 2] = [~0usize, 7usize]; "
                       "let p = &data[1]; let result = data[1];");
    for (const auto* triple : {"riscv32-unknown-elf", "riscv64-unknown-elf",
                               "riscv32-unknown-elf"}) {
        const auto target = codegen::TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target);
        sema::TypeChecker checker(input.diagnostics, target->pointer_bits());
        ASSERT_TRUE(checker.check(*input.syntax));
        stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
        for (int session = 0; session < 2; ++session) {
            const auto hir = analyzer.analyze(*input.syntax);
            ASSERT_FALSE(input.diagnostics.has_error());
            const auto& values = std::get<std::vector<IntegerValue>>(
                hir->globals[0].initializer);
            ASSERT_EQ(values.size(), 2U);
            EXPECT_EQ(values[0].type(),
                      (IntegerType{target->pointer_bits(), false, true}));
            EXPECT_EQ(values[0].decimal(), target->pointer_bits() == 32
                                               ? "4294967295"
                                               : "18446744073709551615");
            EXPECT_EQ(hir->bindings[0].stage, hir::Stage::Runtime);
            EXPECT_FALSE(hir->bindings[0].value->constant);
            EXPECT_EQ(hir->result->stage, hir::Stage::Runtime);
            const auto module = mir::lower(*hir);
            ASSERT_TRUE(module) << module.error();
            const auto ir =
                codegen::LLVMCodegen::emit_for_target(*module, *target);
            ASSERT_TRUE(ir) << ir.error();
            EXPECT_EQ(ir->find("alloca"), std::string::npos);
            EXPECT_EQ(ir->find("extractvalue"), std::string::npos);
        }
    }
}

TEST(GlobalArrays, VerifierRejectsMalformedInitializers) {
    PointerInput input("global mut data: [u32; 2] = [0, 0];");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    const auto good = mir::lower(*hir);
    ASSERT_TRUE(good);
    for (int case_id = 0; case_id < 8; ++case_id) {
        auto bad = *good;
        auto& global = bad.globals[0];
        switch (case_id) {
        case 0:
            global.initializer = IntegerValue::create({32}, 0).value();
            break;
        case 1:
            global.initializer = std::vector<bool>{false, false};
            break;
        case 2:
            std::get<std::vector<IntegerValue>>(global.initializer).pop_back();
            break;
        case 3:
            std::get<std::vector<IntegerValue>>(global.initializer)[1] =
                IntegerValue::create({16}, 0).value();
            break;
        case 4:
            global.type.length = 0;
            break;
        case 5:
            global.type.element_kind = mir::Type::Bool;
            break;
        case 6:
            global.storage.section = ".bss.test";
            std::get<std::vector<IntegerValue>>(global.initializer)[1] =
                IntegerValue::create({32}, 1).value();
            break;
        case 7:
            global.type.integer_type.is_usize = true;
            break;
        }
        EXPECT_FALSE(mir::verify(bad)) << case_id;
        EXPECT_FALSE(codegen::LLVMCodegen::emit(bad)) << case_id;
    }
}

TEST(GlobalArrays, VerifierChecksAddressTypeIndexAndDominance) {
    PointerInput input(
        "global const data: [u32; 2] = [1, 2]; global const scalar: u32 = 1; "
        "export fn address(i: u32) -> *const u32 { &data[i] }");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    const auto good = mir::lower(*hir);
    ASSERT_TRUE(good);
    for (int case_id = 0; case_id < 6; ++case_id) {
        auto bad = *good;
        auto& function = bad.functions[0];
        auto& instructions = function.blocks[0].instructions;
        auto address = std::ranges::find_if(instructions, [](const auto&
                                                                 instruction) {
            return std::holds_alternative<mir::GlobalElementAddressOperation>(
                instruction.operation);
        });
        ASSERT_NE(address, instructions.end());
        auto& operation =
            std::get<mir::GlobalElementAddressOperation>(address->operation);
        switch (case_id) {
        case 0:
            operation.name = "missing";
            break;
        case 1:
            operation.name = "pagos.global.scalar";
            break;
        case 2:
            operation.index = 999;
            break;
        case 3:
            address->type.pointer_mutable = true;
            function.result_type.pointer_mutable = true;
            break;
        case 4:
            instructions[0].type = mir::Type::Bool;
            instructions[0].operation = mir::ConstantOperation{true};
            break;
        case 5:
            std::iter_swap(instructions.begin(), address);
            break;
        }
        const auto result = mir::verify(bad);
        ASSERT_FALSE(result) << case_id;
        if (case_id == 5)
            EXPECT_NE(result.error().find("does not dominate"),
                      std::string::npos);
        else
            EXPECT_EQ(result.error(), "invalid MIR global element address");
    }
}

TEST(GlobalArrays,
     UnusedInitializersStillConsumeTargetSizedConstructionBudget) {
    for (unsigned bits : {32U, 64U}) {
        PointerInput input(
            "global const table: [usize; 4] = [for i in 0..4 { i as usize }];");
        sema::TypeChecker checker(input.diagnostics, bits);
        ASSERT_TRUE(checker.check(*input.syntax));
        stage::AnalysisLimits limits;
        limits.array_bytes = 16;
        stage::StageAnalyzer analyzer(input.diagnostics, checker.types(),
                                      limits);
        const auto hir = analyzer.analyze(*input.syntax);
        EXPECT_EQ(input.diagnostics.has_error(), bits == 64);
        if (bits == 32) {
            EXPECT_FALSE(hir->emit_entry);
            EXPECT_EQ(hir->globals.size(), 1U);
        }
    }
}

TEST(GlobalArrays, ObjectSizesRespectSignedTargetOffsets) {
    EXPECT_TRUE(GlobalStorage::fits_target_size(2147483647ULL, 32));
    EXPECT_FALSE(GlobalStorage::fits_target_size(2147483648ULL, 32));
    EXPECT_TRUE(GlobalStorage::fits_target_size(9223372036854775807ULL, 64));
    EXPECT_FALSE(GlobalStorage::fits_target_size(9223372036854775808ULL, 64));
    EXPECT_FALSE(GlobalStorage::fits_target_size(1, 0));
    EXPECT_FALSE(GlobalStorage::fits_target_size(1, 128));
    EXPECT_TRUE(GlobalStorage::fits_target(8, 2147483647U, 32));
    EXPECT_FALSE(GlobalStorage::fits_target(8, 2147483648U, 32));
    EXPECT_FALSE(GlobalStorage::fits_target(64, 268435456U, 32));
    EXPECT_TRUE(GlobalStorage::fits_target(64, 268435456U, 64));
    EXPECT_FALSE(GlobalStorage::fits_target(8, 1, 0));
}
TEST(GlobalRecords, TargetLayoutAndFieldAddressesSurviveSessionReset) {
    PointerInput input(
        "record State { tag: u8, count: u32, wide: u64, size: usize, tail: i16 "
        "} "
        "global mut state: State align(64) = "
        "State(tail: -7i16, size: ~0usize, wide: ~0u64, count: 12, tag: 3u8); "
        "let p = &state.size; let result = state.count;");
    for (const auto* triple :
         {"riscv32-unknown-elf", "x86_64-unknown-linux-gnu",
          "riscv32-unknown-elf"}) {
        const auto target = codegen::TargetLayout::create({.triple = triple});
        ASSERT_TRUE(target);
        const auto bits = target->pointer_bits();
        sema::TypeChecker checker(input.diagnostics, bits);
        ASSERT_TRUE(checker.check(*input.syntax));
        stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
        for (int session = 0; session < 2; ++session) {
            const auto hir = analyzer.analyze(*input.syntax);
            ASSERT_FALSE(input.diagnostics.has_error());
            ASSERT_EQ(hir->globals.size(), 1U);
            const auto& constant =
                std::get<RecordConstant>(hir->globals[0].initializer);
            EXPECT_EQ(std::get<IntegerValue>(constant.fields[3]).type(),
                      (IntegerType{bits, false, true}));
            EXPECT_EQ(hir->bindings[0].stage, hir::Stage::Runtime);
            EXPECT_FALSE(hir->bindings[0].value->constant);
            const auto module = mir::lower(*hir);
            ASSERT_TRUE(module) << module.error();
            const auto layout = target->layout_of(module->globals[0].type);
            ASSERT_TRUE(layout);
            EXPECT_EQ(layout->size_bytes, bits == 32 ? 24U : 32U);
            EXPECT_EQ(layout->alignment_bytes, 8U);
            EXPECT_EQ(layout->field_offsets,
                      (std::vector<std::uint64_t>{0, 4, 8, 16,
                                                  bits == 32 ? 20U : 24U}));
            const auto ir =
                codegen::LLVMCodegen::emit_for_target(*module, *target);
            ASSERT_TRUE(ir) << ir.error();
            EXPECT_NE(ir->find("align 64"), std::string::npos);
            EXPECT_EQ(ir->find("alloca"), std::string::npos);
            EXPECT_EQ(ir->find("extractvalue"), std::string::npos);
        }
    }
}

TEST(GlobalRecords, VerifierRejectsMalformedInitializersAndNominalSchemas) {
    PointerInput input("record R { tag: u8, size: usize } "
                       "global mut state: R = R(tag: 0u8, size: 0usize); "
                       "global mut twin: R = R(tag: 0u8, size: 0usize);");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    const auto good = mir::lower(*hir);
    ASSERT_TRUE(good);
    for (int case_id = 0; case_id < 11; ++case_id) {
        auto bad = *good;
        auto& global = bad.globals[0];
        auto& value = std::get<RecordConstant>(global.initializer);
        switch (case_id) {
        case 0:
            value.name = "Other";
            break;
        case 1:
            value.fields.pop_back();
            break;
        case 2:
            value.fields[0] = true;
            break;
        case 3:
            value.fields[0] = IntegerValue::create({16}, 0).value();
            break;
        case 4:
            value.fields[1] = IntegerValue::create({64}, 0).value();
            break;
        case 5:
            global.type.fields[0] = mir::Type::Bool;
            value.fields[0] = false;
            break;
        case 6:
            global.storage.section = ".bss.state";
            value.fields[0] = IntegerValue::create({8}, 1).value();
            break;
        case 7:
            global.type.fields[1] = mir::Type::integer(32, false, true);
            value.fields[1] =
                IntegerValue::create({32, false, true}, 0).value();
            break;
        case 8:
            // Both initializers are individually valid, but one nominal type
            // must not acquire a second schema through a global definition.
            global.type.fields[0] = mir::Type::integer(16);
            value.fields[0] = IntegerValue::create({16}, 0).value();
            break;
        case 9:
            global.initializer = false;
            break;
        case 10:
            global.type.fields.clear();
            value.fields.clear();
            break;
        }
        EXPECT_FALSE(mir::verify(bad)) << case_id;
        EXPECT_FALSE(codegen::LLVMCodegen::emit(bad)) << case_id;
    }
}

TEST(GlobalRecords, VerifierChecksFieldAddressSymbolIndexTypeAndPermission) {
    PointerInput input(
        "record R { field: u32 } global const data: R = R(field: 1); "
        "global const scalar: u32 = 0; "
        "export fn address() -> *const u32 { &data.field }");
    sema::TypeChecker checker(input.diagnostics, 64);
    ASSERT_TRUE(checker.check(*input.syntax));
    stage::StageAnalyzer analyzer(input.diagnostics, checker.types());
    const auto hir = analyzer.analyze(*input.syntax);
    const auto good = mir::lower(*hir);
    ASSERT_TRUE(good);
    for (int case_id = 0; case_id < 6; ++case_id) {
        auto bad = *good;
        auto& instructions = bad.functions[0].blocks[0].instructions;
        auto address =
            std::ranges::find_if(instructions, [](const auto& instruction) {
                return std::holds_alternative<mir::GlobalFieldAddressOperation>(
                    instruction.operation);
            });
        ASSERT_NE(address, instructions.end());
        auto& operation =
            std::get<mir::GlobalFieldAddressOperation>(address->operation);
        switch (case_id) {
        case 0:
            operation.name = "missing";
            break;
        case 1:
            operation.name = "pagos.global.scalar";
            break;
        case 2:
            operation.field_index = 1;
            break;
        case 3:
            address->type.pointer_mutable = true;
            break;
        case 4:
            address->type.integer_type.width = 16;
            break;
        case 5:
            address->type.integer_type.is_signed = true;
            break;
        }
        bad.functions[0].result_type = address->type;
        const auto result = mir::verify(bad);
        ASSERT_FALSE(result) << case_id;
        EXPECT_EQ(result.error(), "invalid MIR global field address");
        EXPECT_FALSE(codegen::LLVMCodegen::emit(bad));
    }
}

TEST(GlobalRecords, ConstructionBudgetCountsLogicalFieldsNotStoragePadding) {
    for (unsigned bits : {32U, 64U}) {
        PointerInput input(
            "record R { tag: u8, size: usize } "
            "global const unused: R = R(tag: 0u8, size: 0usize);");
        sema::TypeChecker checker(input.diagnostics, bits);
        ASSERT_TRUE(checker.check(*input.syntax));
        stage::AnalysisLimits limits;
        limits.aggregate_bytes = 5;
        stage::StageAnalyzer analyzer(input.diagnostics, checker.types(),
                                      limits);
        const auto hir = analyzer.analyze(*input.syntax);
        EXPECT_EQ(input.diagnostics.has_error(), bits == 64);
        if (bits == 32) {
            ASSERT_EQ(hir->globals.size(), 1U);
            EXPECT_FALSE(hir->emit_entry);
            const auto module = mir::lower(*hir);
            ASSERT_TRUE(module);
            const auto target = codegen::TargetLayout::create(
                {.triple = "riscv32-unknown-elf"});
            ASSERT_TRUE(target);
            EXPECT_EQ(target->layout_of(module->globals[0].type)->size_bytes,
                      8U);
        }
    }
}
} // namespace
