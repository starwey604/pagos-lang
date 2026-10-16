#include "pagos/stage/provenance_analysis.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

// Process-only probe: hooks pause at real FileStore IO inside Stage recording.
// The parent kills us with SIGKILL, bypassing all stack and RAII cleanup.
int main(int count, char** args) {
    using namespace pagos;
    using namespace stage;
    using namespace stage::provenance;
    namespace fs = std::filesystem;
    if (count != 3)
        return 2;
    const std::string action(args[1]);
    const fs::path root(args[2]);
    std::string text = "fn square(x: u32) -> u32 { x * x } comptime let k = "
                       "square(7); fn big(x: u32) -> u32 { let n0 = x + 1; ";
    for (unsigned i = 1; i < 64; ++i)
        text += "let n" + std::to_string(i) + " = n" + std::to_string(i - 1) +
                " + 1; ";
    text += "n63 } let aa = big(external_input()); aa + k;";
    auto source = source::SourceFile::from_text("probe.pgs", text);
    source::DiagnosticEngine diagnostics(source);
    syntax::Lexer lexer(source, diagnostics);
    const auto tokens = lexer.tokenize();
    syntax::Parser parser(tokens, diagnostics);
    const auto ast = parser.parse_module();
    sema::TypeChecker checker(diagnostics, 32);
    if (!ast || !checker.check(*ast))
        return 2;
    EvidenceBuildConfig config;
    config.mode = action == "memory" ? EvidenceBuildMode::Memory
                                     : EvidenceBuildMode::Spill;
    config.directory = root / "spill";
    config.block_bytes = 256;
    config.cache_bytes = 1024;
    config.resident_bytes = 8192;
    config.failure_policy = SpillFailurePolicy::Unavailable;
    if (action == "quota")
        config.disk_bytes = 600;
    if (action == "before" || action == "after")
        config.store.fault = [&](cache::IoOperation operation,
                                 const fs::path& destination) {
            if ((action == "before" &&
                 operation == cache::IoOperation::WriteTemporary) ||
                (action == "after" &&
                 operation == cache::IoOperation::ObjectPublished)) {
                const auto session =
                    destination.parent_path().parent_path().parent_path();
                {
                    std::ofstream ready(root / "ready");
                    ready << session.string();
                }
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return std::make_error_code(std::errc::timed_out);
            }
            return std::error_code{};
        };
    const cache::QueryContext context{
        .module = {"spill-process-test", "probe.pgs"},
        .target = {"riscv32-unknown-elf", "generic-rv32", "", "e-p:32:32", 32,
                   true},
        .compiler_build = "spill-process-probe-v1",
        .semantic_options = "pagos.staging.v1"};
    const CacheConfig cache_config{CacheMode::ReadWrite, root / "semantic"};
    StageAnalyzer analyzer(diagnostics, checker.types(), {}, context,
                           cache_config, config);
    auto module = analyzer.analyze(*ast);
    if (!module || diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }
    std::ostringstream hir;
    hir::print(*module, hir);
    const auto result = hir.str();
    const auto signature = cache::fingerprint(
        "stage-spill-process.v1",
        {reinterpret_cast<const std::uint8_t*>(result.data()), result.size()});
    std::cout << "{\"available\":" << module->provenance->available()
              << ",\"written\":"
              << module->provenance->construction_stats().written_blocks
              << ",\"blocked\":" << analyzer.cache_telemetry().blocked_writes
              << ",\"writes\":" << analyzer.cache_telemetry().writes
              << ",\"fuel\":" << analyzer.stats().fuel_consumed
              << ",\"signature\":\"" << signature.hex() << "\"}\n";
}
