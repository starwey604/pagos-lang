#pragma once

#include "pagos/source/source_manager.h"
#include "pagos/stage/cache_identity.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <gtest/gtest.h>

namespace cache_test {
inline pagos::cache::QueryContext context(unsigned bits = 32) {
    return {
        .module = {"workspace", "src/input.pgs"},
        .target = {bits == 32 ? "riscv32-unknown-elf" : "riscv64-unknown-elf",
                   bits == 32 ? "generic-rv32" : "generic-rv64", "",
                   bits == 32 ? "e-p:32:32" : "e-p:64:64", bits, true},
        .compiler_build = "test-build-v1",
        .semantic_options = "pagos.staging.v1"};
}
struct Input {
    explicit Input(std::string text, unsigned bits = 32)
        : source(pagos::source::SourceFile::from_text("input.pgs",
                                                      std::move(text))),
          diagnostics(source), checker(diagnostics, bits) {
        pagos::syntax::Lexer lexer(source, diagnostics);
        const auto tokens = lexer.tokenize();
        pagos::syntax::Parser parser(tokens, diagnostics);
        module = parser.parse_module();
        EXPECT_FALSE(diagnostics.has_error());
        if (module) {
            EXPECT_TRUE(checker.check(*module));
        }
        EXPECT_FALSE(diagnostics.has_error());
    }
    auto identity() const {
        return pagos::stage::build_cache_identity(
            *module, checker.types(), context(checker.types().pointer_bits));
    }
    pagos::source::SourceFile source;
    pagos::source::DiagnosticEngine diagnostics;
    pagos::sema::TypeChecker checker;
    std::unique_ptr<pagos::syntax::Module> module;
};
inline pagos::cache::QueryKey key() {
    using namespace pagos;
    const auto environment = context();
    return {cache::QueryKind::EvaluatePureSpecialization,
            environment,
            {environment.module, cache::DefinitionKind::Function, "square"},
            cache::fingerprint("definition", {}),
            {{sema::Type::integer(32), cache::ArgumentStage::Comptime,
              Constant(IntegerValue(7))}}};
}
} // namespace cache_test
