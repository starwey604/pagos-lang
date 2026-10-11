#include "pagos/codegen/target.h"
#include "pagos/source/source_manager.h"
#include "pagos/stage/cache_identity.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <iostream>

// Test-only entry point: exercises the real checked AST in fresh processes.
// No pagosc command or cache IO is added by C1a.
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    const auto source = pagos::source::SourceFile::load(argv[1]);
    if (!source)
        return 2;
    const std::string bits = argv[2];
    if (bits != "32" && bits != "64")
        return 2;
    const auto target = pagos::codegen::TargetLayout::create(
        {.triple =
             bits == "32" ? "riscv32-unknown-elf" : "riscv64-unknown-elf"});
    if (!target)
        return 2;
    pagos::source::DiagnosticEngine diagnostics(*source);
    pagos::syntax::Lexer lexer(*source, diagnostics);
    const auto tokens = lexer.tokenize();
    pagos::syntax::Parser parser(tokens, diagnostics);
    auto module = parser.parse_module();
    if (diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }
    pagos::sema::TypeChecker checker(diagnostics, target->pointer_bits());
    if (!checker.check(*module)) {
        diagnostics.render(std::cerr);
        return 1;
    }
    const auto& config = target->config();
    const pagos::cache::QueryContext context{
        .module = {"probe-workspace", "input.pgs"},
        .target = {config.triple, config.cpu, config.features,
                   target->data_layout(), target->pointer_bits(),
                   target->little_endian()},
        .compiler_build = "probe-build-v1",
        .semantic_options = "pagos.staging.v1"};
    const auto identity =
        pagos::stage::build_cache_identity(*module, checker.types(), context);
    if (!identity)
        return 1;
    const auto* function =
        identity->find(pagos::cache::DefinitionKind::Function, "square");
    if (!function || function->parameters.size() != 1)
        return 1;
    const auto type = function->parameters[0];
    const auto value = pagos::IntegerValue::create(type.integer_type, 7);
    if (!value)
        return 1;
    const auto key = pagos::stage::make_query_key(
        *function, context,
        {{type, pagos::cache::ArgumentStage::Comptime,
          pagos::Constant(*value)}});
    if (!key)
        return 1;
    const auto hash = pagos::cache::query_fingerprint(*key);
    if (!hash)
        return 1;
    std::cout << identity->module_fingerprint.hex() << '\n'
              << hash->hex() << '\n';
}
