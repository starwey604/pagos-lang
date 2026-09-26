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
} // namespace
