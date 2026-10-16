#include "pagos/stage/provenance_snapshot.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <charconv>
#include <chrono>
#include <iostream>
#include <sstream>

int main(int count, char** arguments) {
    using namespace pagos;
    using Clock = std::chrono::steady_clock;
    if (count != 2)
        return 1;
    const std::string_view text(arguments[1]);
    unsigned size{};
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), size);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !size || size > 4096)
        return 1;
    std::ostringstream program;
    program << "fn large(x: u32) -> u32 { let n0 = x + 1; ";
    for (unsigned i = 1; i < size; ++i)
        program << "let n" << i << " = n" << i - 1 << " + 1; ";
    program << "n" << size - 1 << " } let result = large(external_input());";
    const auto source =
        source::SourceFile::from_text("benchmark.pgs", program.str());
    source::DiagnosticEngine diagnostics(source);
    const auto start = Clock::now();
    syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    syntax::Parser parser(tokens, diagnostics);
    const auto syntax_module = parser.parse_module();
    sema::TypeChecker checker(diagnostics, 32);
    if (!syntax_module || !checker.check(*syntax_module))
        return 1;
    cache::QueryContext context{.module = {"benchmark", "module.pgs"},
                                .target = {"riscv32-unknown-elf",
                                           "generic-rv32", "", "e-p:32:32", 32,
                                           true},
                                .compiler_build = "snapshot-bench-v1",
                                .semantic_options = "pagos.staging.v1"};
    const auto identity =
        stage::build_cache_identity(*syntax_module, checker.types(), context);
    if (!identity)
        return 1;
    const auto checked = Clock::now();
    stage::StageAnalyzer analyzer(diagnostics, checker.types());
    const auto module = analyzer.analyze(*syntax_module);
    if (!module || diagnostics.has_error())
        return 1;
    const auto built = Clock::now();
    const auto encoded = stage::ProvenanceSnapshot::encode(
        *module->provenance, *identity, context, source);
    if (!encoded) {
        std::cerr << "encode " << static_cast<int>(encoded.error());
        return 1;
    }
    const auto exported = Clock::now();
    const auto opened = stage::ProvenanceSnapshot::open(encoded->graph);
    if (!opened)
        return 1;
    const auto validated = Clock::now();
    const auto restored = opened->restore(*identity, context);
    if (!restored)
        return 1;
    const auto decoded = Clock::now();
    const auto result =
        (*restored)->graph().query((*restored)->roots().back().node);
    const auto queried = Clock::now();
    const auto oracle = module->provenance->graph().query(
        module->provenance->roots().back().node);
    if (!result.complete || result.sources != oracle.sources ||
        result.invocations != oracle.invocations)
        return 1;
    const auto ns = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a)
            .count();
    };
    std::cout << "{\"size\":" << size
              << ",\"checked_identity_ns\":" << ns(start, checked)
              << ",\"stage_ns\":" << ns(checked, built)
              << ",\"encode_ns\":" << ns(built, exported)
              << ",\"checksum_open_ns\":" << ns(exported, validated)
              << ",\"restore_ns\":" << ns(validated, decoded)
              << ",\"query_ns\":" << ns(decoded, queried)
              << ",\"graph_bytes\":" << encoded->graph.size()
              << ",\"positions_bytes\":" << encoded->positions.size()
              << ",\"nodes\":" << (*restored)->graph().stats().nodes
              << ",\"edges\":" << (*restored)->graph().stats().edges
              << ",\"states\":" << result.visited_states << "}\n";
}
