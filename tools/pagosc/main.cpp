#include "pagos/codegen/llvm_codegen.h"
#include "pagos/hir/hir.h"
#include "pagos/mir/lowering.h"
#include "pagos/mir/mir.h"
#include "pagos/sema/type_checker.h"
#include "pagos/source/diagnostic.h"
#include "pagos/source/source_manager.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <iostream>
#include <optional>
#include <print>
#include <string_view>

namespace {

enum class Command { Check, EmitHir, EmitMir, EmitLlvm, ExplainStage };

void print_usage() {
    std::println(stderr, "usage: pagosc "
                         "<check|emit-hir|emit-mir|emit-llvm|explain-stage> "
                         "<source.pgs>");
}

std::optional<Command> parse_command(std::string_view name) {
    if (name == "check") {
        return Command::Check;
    }
    if (name == "emit-hir") {
        return Command::EmitHir;
    }
    if (name == "emit-mir") {
        return Command::EmitMir;
    }
    if (name == "emit-llvm") {
        return Command::EmitLlvm;
    }
    if (name == "explain-stage") {
        return Command::ExplainStage;
    }
    return std::nullopt;
}

} // namespace

int main(int argument_count, char** arguments) {
    if (argument_count != 3) {
        print_usage();
        return 2;
    }
    const auto command = parse_command(arguments[1]);
    if (!command) {
        std::println(stderr, "error: unknown command `{}`", arguments[1]);
        print_usage();
        return 2;
    }

    auto source = pagos::source::SourceFile::load(arguments[2]);
    if (!source) {
        std::println(stderr, "error: {}", source.error());
        return 1;
    }
    pagos::source::DiagnosticEngine diagnostics(*source);
    pagos::syntax::Lexer lexer(*source, diagnostics);
    auto tokens = lexer.tokenize();
    if (diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }

    pagos::syntax::Parser parser(tokens, diagnostics);
    auto syntax_module = parser.parse_module();
    if (diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }

    pagos::sema::TypeChecker type_checker(diagnostics);
    if (!type_checker.check(*syntax_module)) {
        diagnostics.render(std::cerr);
        return 1;
    }

    pagos::stage::StageAnalyzer stage_analyzer(diagnostics,
                                               type_checker.types());
    auto hir_module = stage_analyzer.analyze(*syntax_module);
    if (diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }

    switch (*command) {
    case Command::Check:
        return 0;
    case Command::EmitHir:
        pagos::hir::print(*hir_module, std::cout);
        return 0;
    case Command::ExplainStage:
        pagos::hir::explain_stages(*hir_module, std::cout);
        return 0;
    case Command::EmitMir:
    case Command::EmitLlvm:
        break;
    }

    auto mir_module = pagos::mir::lower(*hir_module);
    if (!mir_module) {
        std::println(stderr, "error: {}", mir_module.error());
        return 1;
    }
    if (*command == Command::EmitMir) {
        pagos::mir::print(*mir_module, std::cout);
        return 0;
    }

    auto llvm_ir = pagos::codegen::LLVMCodegen::emit(*mir_module);
    if (!llvm_ir) {
        std::println(stderr, "error: {}", llvm_ir.error());
        return 1;
    }
    std::cout << *llvm_ir;
    return 0;
}
