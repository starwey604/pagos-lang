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

#include <charconv>
#include <expected>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>

namespace {

enum class Command { Check, EmitHir, EmitMir, EmitLlvm, ExplainStage };

struct Options {
    Command command;
    std::string source_path;
    pagos::stage::AnalysisLimits limits;
};

void print_usage() {
    std::println(stderr, "usage: pagosc [--max-fuel=N] "
                         "[--max-recursion-depth=N] "
                         "[--max-specializations=N] "
                         "[--max-array-elements=N] [--max-array-bytes=N] "
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

std::expected<std::size_t, std::string> parse_limit(std::string_view option,
                                                    std::string_view prefix) {
    const auto spelling = option.substr(prefix.size());
    std::size_t value{};
    const auto result = std::from_chars(
        spelling.data(), spelling.data() + spelling.size(), value);
    if (spelling.empty() || result.ec != std::errc{} ||
        result.ptr != spelling.data() + spelling.size()) {
        return std::unexpected("invalid numeric option `" +
                               std::string(option) + "`");
    }
    return value;
}

std::expected<Options, std::string> parse_options(int argument_count,
                                                  char** arguments) {
    std::optional<Command> command;
    std::optional<std::string> source_path;
    pagos::stage::AnalysisLimits limits;

    for (int index = 1; index < argument_count; ++index) {
        const std::string_view argument{arguments[index]};
        constexpr std::string_view fuel_prefix = "--max-fuel=";
        constexpr std::string_view depth_prefix = "--max-recursion-depth=";
        constexpr std::string_view specialization_prefix =
            "--max-specializations=";
        constexpr std::string_view array_elements_prefix =
            "--max-array-elements=";
        constexpr std::string_view array_bytes_prefix = "--max-array-bytes=";
        if (argument.starts_with(fuel_prefix)) {
            auto value = parse_limit(argument, fuel_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.fuel = *value;
        } else if (argument.starts_with(depth_prefix)) {
            auto value = parse_limit(argument, depth_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.recursion_depth = *value;
        } else if (argument.starts_with(specialization_prefix)) {
            auto value = parse_limit(argument, specialization_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.specializations = *value;
        } else if (argument.starts_with(array_elements_prefix)) {
            auto value = parse_limit(argument, array_elements_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.array_elements = *value;
        } else if (argument.starts_with(array_bytes_prefix)) {
            auto value = parse_limit(argument, array_bytes_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.array_bytes = *value;
        } else if (argument.starts_with("--")) {
            return std::unexpected("unknown option `" + std::string(argument) +
                                   "`");
        } else if (!command) {
            command = parse_command(argument);
            if (!command) {
                return std::unexpected("unknown command `" +
                                       std::string(argument) + "`");
            }
        } else if (!source_path) {
            source_path = std::string(argument);
        } else {
            return std::unexpected("unexpected argument `" +
                                   std::string(argument) + "`");
        }
    }
    if (!command || !source_path) {
        return std::unexpected("a command and source file are required");
    }
    return Options{.command = *command,
                   .source_path = std::move(*source_path),
                   .limits = limits};
}

} // namespace

int main(int argument_count, char** arguments) {
    const auto options = parse_options(argument_count, arguments);
    if (!options) {
        std::println(stderr, "error: {}", options.error());
        print_usage();
        return 2;
    }

    auto source = pagos::source::SourceFile::load(options->source_path);
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

    pagos::stage::StageAnalyzer stage_analyzer(
        diagnostics, type_checker.types(), options->limits);
    auto hir_module = stage_analyzer.analyze(*syntax_module);
    if (diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }

    switch (options->command) {
    case Command::Check:
        return 0;
    case Command::EmitHir:
        pagos::hir::print(*hir_module, std::cout);
        return 0;
    case Command::ExplainStage:
        pagos::hir::explain_stages(*hir_module, std::cout);
        std::println(std::cout,
                     "analysis: fuel={}, specializations={}, cache-hits={}, "
                     "max-depth={}, array-elements={}, array-bytes={}",
                     stage_analyzer.stats().fuel_consumed,
                     stage_analyzer.stats().specializations,
                     stage_analyzer.stats().cache_hits,
                     stage_analyzer.stats().maximum_recursion_depth,
                     stage_analyzer.stats().array_elements_reserved,
                     stage_analyzer.stats().array_bytes_reserved);
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
    if (options->command == Command::EmitMir) {
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
