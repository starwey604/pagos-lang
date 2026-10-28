#include "pagos/source/source_manager.h"
#include "pagos/stage/scalar_replay.h"
#include "pagos/stage/scalar_replay_execution.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <charconv>
#include <iostream>

// Independent process harness. The read branch never constructs StageAnalyzer.
int main(int argc, char** argv) {
    using namespace pagos;
    if (argc != 6 && argc != 7)
        return 2;
    const std::string_view action = argv[1], bits_text = argv[4];
    if ((action != "read" && action != "publish" && action != "require" &&
         action != "value" && action != "execute") ||
        (bits_text != "32" && bits_text != "64"))
        return 2;
    if (argc == 7 && action != "execute")
        return 2;
    stage::ScalarReplayExecutionLimits execution_limits;
    if (argc == 7) {
        const std::string_view fuel = argv[6];
        const auto parsed = std::from_chars(
            fuel.data(), fuel.data() + fuel.size(), execution_limits.fuel);
        if (fuel.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != fuel.data() + fuel.size())
            return 2;
    }
    const unsigned bits = bits_text == "32" ? 32U : 64U;
    const auto source = source::SourceFile::load(argv[3]);
    if (!source)
        return 2;
    source::DiagnosticEngine diagnostics(*source);
    syntax::Lexer lexer(*source, diagnostics);
    const auto tokens = lexer.tokenize();
    syntax::Parser parser(tokens, diagnostics);
    auto module = parser.parse_module();
    sema::TypeChecker checker(diagnostics, bits);
    if (!module || diagnostics.has_error() || !checker.check(*module)) {
        diagnostics.render(std::cerr);
        return 1;
    }
    const cache::QueryContext context{
        .module = {"scalar-replay-probe", "input.pgs"},
        .target = {bits == 32 ? "riscv32-unknown-elf" : "riscv64-unknown-elf",
                   bits == 32 ? "generic-rv32" : "generic-rv64", "",
                   bits == 32 ? "e-p:32:32" : "e-p:64:64", bits, true},
        .compiler_build = argv[5],
        .semantic_options = "pagos.staging.v1"};
    if (action == "read" || action == "execute") {
        const auto identity =
            stage::build_cache_identity(*module, checker.types(), context);
        if (!identity)
            return 1;
        const auto* definition =
            identity->find(cache::DefinitionKind::Function, "square");
        if (!definition || definition->parameters.size() != 1)
            return 1;
        const auto type = definition->parameters[0];
        const auto argument = IntegerValue::create(type.integer_type, 7);
        if (!argument)
            return 1;
        const auto key = stage::make_query_key(
            *definition, context,
            {{type, cache::ArgumentStage::Comptime, Constant(*argument)}});
        if (!key)
            return 1;
        const auto record = stage::load_scalar_replay(
            cache::FileStore(argv[2]), *module, checker.types(), *key);
        if (!record) {
            std::cerr << "replay unavailable: "
                      << static_cast<unsigned>(record.error().code) << '\n';
            return 3;
        }
        if (action == "execute") {
            const auto replay =
                stage::execute_scalar_replay(*record, execution_limits);
            if (!replay) {
                std::cerr << "execution unavailable: "
                          << static_cast<unsigned>(replay.error().code)
                          << " nodes=" << replay.error().stats.nodes_evaluated
                          << " fuel=" << replay.error().stats.fuel_consumed
                          << '\n';
                return 3;
            }
            std::cout << "{\"stage_runs\":0,\"nodes\":"
                      << replay->stats.nodes_evaluated
                      << ",\"fuel\":" << replay->stats.fuel_consumed
                      << ",\"descriptor\":\"" << replay->descriptor.hex()
                      << "\",\"value\":"
                      << std::get<IntegerValue>(replay->value.value).bits()
                      << ",\"details\":[";
            bool first = true;
            for (const auto& step : replay->details) {
                if (!first)
                    std::cout << ',';
                first = false;
                std::cout << "{\"node\":" << step.node << ",\"value\":"
                          << std::get<IntegerValue>(step.value).bits() << '}';
            }
            std::cout << "]}\n";
            return 0;
        }
        const auto span = identity->resolve(record->descriptor.result_site);
        if (!span)
            return 1;
        std::cout
            << "{\"stage_runs\":0,\"nodes\":" << record->descriptor.nodes.size()
            << ",\"descriptor\":\""
            << stage::replay_descriptor_fingerprint(record->descriptor)->hex()
            << "\",\"object\":\"" << record->object.digest.hex()
            << "\",\"value\":"
            << std::get<IntegerValue>(record->value.value).bits()
            << ",\"begin\":" << span->begin << ",\"end\":" << span->end
            << "}\n";
        return 0;
    }
    stage::StageAnalyzer analyzer(
        diagnostics, checker.types(), {}, context,
        {.mode = action == "publish" ? stage::CacheMode::ReadWrite
                                     : stage::CacheMode::ReadOnly,
         .directory = argv[2],
         .evidence = action == "require" ? stage::CacheEvidence::ScalarReplay
                                         : stage::CacheEvidence::Value});
    const auto hir = analyzer.analyze(*module);
    if (diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }
    const auto& cache = analyzer.cache_telemetry();
    std::cout << "{\"stage_runs\":1,\"fuel\":" << analyzer.stats().fuel_consumed
              << ",\"hits\":" << cache.hits
              << ",\"evaluations\":" << cache.body_evaluations
              << ",\"replay_writes\":" << cache.replay_writes
              << ",\"replay_hits\":" << cache.replay_hits << "}\n";
}
