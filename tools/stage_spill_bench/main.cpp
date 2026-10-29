#include "pagos/stage/provenance_analysis.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_set>

namespace {
struct MemoryReading {
    std::uint64_t rss{}, peak{};
};
MemoryReading memory_reading() {
    MemoryReading result;
    std::ifstream input("/proc/self/status");
    for (std::string line; std::getline(input, line);) {
        if (line.starts_with("VmRSS:") || line.starts_with("VmHWM:")) {
            std::istringstream value(line.substr(line.find(':') + 1));
            value >> (line.starts_with("VmRSS:") ? result.rss : result.peak);
        }
    }
    return result;
}
} // namespace

// Real Stage build in a fresh process. Build-only runs never traverse evidence;
// query runs retain the complete front end and HIR and use the native view.
int main(int count, char** args) {
    using namespace pagos;
    using namespace stage::provenance;
    using Clock = std::chrono::steady_clock;
    if (count != 5 && count != 6)
        return 1;
    const std::string mode(args[1]), phase(args[2]);
    if ((mode != "memory" && mode != "spill") ||
        (phase != "build" && phase != "query" && phase != "census"))
        return 1;
    auto source = source::SourceFile::load(args[3]);
    if (!source)
        return 1;
    const auto begin = Clock::now();
    source::DiagnosticEngine diagnostics(*source);
    syntax::Lexer lexer(*source, diagnostics);
    const auto tokens = lexer.tokenize();
    syntax::Parser parser(tokens, diagnostics);
    const auto ast = parser.parse_module();
    sema::TypeChecker checker(diagnostics, 32);
    if (!ast || !checker.check(*ast))
        return 1;
    const auto checked = Clock::now();
    EvidenceBuildConfig config;
    if (mode == "spill") {
        config.mode = EvidenceBuildMode::Spill;
        config.directory = args[4];
        config.block_bytes = 32UZ * 1024;
        config.resident_bytes = 256UZ * 1024;
        config.cache_bytes = 64UZ * 1024;
        config.failure_policy = SpillFailurePolicy::Unavailable;
    }
    config.measure_memory = phase == "census";
    std::optional<cache::QueryContext> context;
    if (count == 6) {
        if (std::string_view(args[5]) != "identity")
            return 1;
        context = cache::QueryContext{
            .module = {"stage-working-set-benchmark", "module.pgs"},
            .target = {"riscv32-unknown-elf", "generic-rv32", "", "e-p:32:32",
                       32, true},
            .compiler_build = "stage-working-set-bench-v1",
            .semantic_options = "pagos.staging.v1"};
    }
    stage::StageAnalyzer analyzer(diagnostics, checker.types(), {}, context, {},
                                  config);
    const auto module = analyzer.analyze(*ast);
    if (!module || diagnostics.has_error() ||
        !module->provenance->available()) {
        diagnostics.render(std::cerr);
        return 1;
    }
    const auto built = Clock::now();
    const auto build_memory = count == 6 ? memory_reading() : MemoryReading{};
    const auto& analysis = *module->provenance;
    const auto view = analysis.evidence();
    std::size_t states = 0, sources = 0;
    std::string signature;
    bool reset_peak = false;
    MemoryReading query_memory_before;
    if (count == 6 && phase == "query") {
        std::ofstream reset("/proc/self/clear_refs");
        reset << "5";
        reset.close();
        reset_peak = static_cast<bool>(reset);
        query_memory_before = memory_reading();
    }
    const auto query_begin = Clock::now();
    if (phase == "query") {
        const auto root = std::ranges::find(analysis.roots(), std::string("aa"),
                                            &stage::ProvenanceRoot::name);
        if (root == analysis.roots().end())
            return 1;
        QueryTask task(view, root->node);
        while (task.progress() != QueryProgress::Completed) {
            (void)task.advance();
            if (task.progress() == QueryProgress::Failed)
                return 1;
        }
        const auto result = task.result();
        if (!result.complete)
            return 1;
        states = task.stats().visited_states;
        sources = result.sources.size();
        for (const auto& item : result.sources)
            signature += analysis.site(item.site).label + ':' +
                         std::to_string(analysis.site(item.site).span.begin);
    }
    const auto queried = Clock::now();
    const auto query_memory = reset_peak ? memory_reading() : MemoryReading{};
    // Separate census children only: allocating the traversal index must not
    // contaminate build/query RSS or clocks. It observes retained HIR, not peak
    // analyzer/Recorder maps and not freed/allocator-retained memory.
    hir::RuntimePath::Accounting paths;
    std::size_t expressions = 0;
    if (phase == "census") {
        std::vector<hir::ExprPtr> pending{module->result};
        for (const auto& binding : module->bindings) {
            pending.push_back(binding.value);
            if (binding.trace)
                paths.add(binding.trace->path);
        }
        for (const auto& function : module->residual_functions) {
            pending.push_back(function->body);
            pending.insert(pending.end(), function->parameters.begin(),
                           function->parameters.end());
        }
        std::unordered_set<const hir::Expr*> seen;
        while (!pending.empty()) {
            const auto value = std::move(pending.back());
            pending.pop_back();
            if (!value || !seen.insert(value.get()).second)
                continue;
            ++expressions;
            if (value->trace)
                paths.add(value->trace->path);
            pending.insert(pending.end(), value->operands.begin(),
                           value->operands.end());
        }
    }
    std::size_t files = 0, file_bytes = 0, cas_files = 0, cas_bytes = 0;
    if (std::filesystem::exists(args[4]))
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(args[4]))
            if (entry.is_regular_file()) {
                ++files;
                file_bytes += entry.file_size();
                if (entry.path().filename() != "session.lock") {
                    ++cas_files;
                    cas_bytes += entry.file_size();
                }
            }
    stage::ProvenanceMemoryStats metadata;
    stage::StageMemoryStats working;
    if (phase == "census") {
        metadata = analysis.memory_stats();
        working = analyzer.memory_stats();
    }
    // Linux kernel counters include loader/source IO and publication
    // verification reads. Snapshot after file enumeration, before
    // printing/destruction. These are read/write syscall and kernel
    // block-accounting bytes, not device IO; metadata operations and later
    // unlink/destructors are outside this sample.
    std::map<std::string, std::uint64_t> io;
    std::ifstream counters("/proc/self/io");
    for (std::string name; counters >> name;) {
        std::uint64_t value{};
        counters >> value;
        io[name.substr(0, name.size() - 1)] = value;
    }
    const auto read = view->read_stats();
    const auto& build = analysis.construction_stats();
    const auto ns = [](auto start, auto end) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count();
    };
    std::cout
        << "{\"build_peak_rss_kib\":" << build_memory.peak
        << ",\"query_peak_reset\":" << reset_peak
        << ",\"query_baseline_rss_kib\":" << query_memory_before.rss
        << ",\"query_peak_rss_kib\":" << query_memory.peak
        << ",\"io_available\":" << io.contains("rchar")
        << ",\"io_rchar\":" << io["rchar"] << ",\"io_wchar\":" << io["wchar"]
        << ",\"io_syscr\":" << io["syscr"] << ",\"io_syscw\":" << io["syscw"]
        << ",\"io_read_bytes\":" << io["read_bytes"]
        << ",\"io_write_bytes\":" << io["write_bytes"]
        << ",\"io_cancelled_write_bytes\":" << io["cancelled_write_bytes"]
        << ",\"sites_bytes\":" << metadata.sites_bytes
        << ",\"roots_bytes\":" << metadata.roots_bytes
        << ",\"decisions_bytes\":" << metadata.decisions_bytes
        << ",\"scopes_bytes\":" << metadata.scopes_bytes
        << ",\"specialization_bytes\":" << metadata.specialization_bytes
        << ",\"identity_bytes\":" << metadata.identity_bytes
        << ",\"view_directory_bytes\":" << metadata.view_directory_bytes
        << ",\"graph_bytes\":" << metadata.graph_bytes
        << ",\"intern_bytes\":" << metadata.intern_bytes
        << ",\"recorder_maps_bytes\":" << metadata.recorder_maps_bytes
        << ",\"recorder_frames_bytes\":" << metadata.recorder_frames_bytes
        << ",\"definition_identity_bytes\":"
        << metadata.definition_identity_bytes
        << ",\"memo_entries\":" << metadata.memo_entries
        << ",\"expired_memo_entries\":" << metadata.expired_memo_entries
        << ",\"memo_peak_entries\":" << metadata.memo_peak_entries
        << ",\"pruned_memo_entries\":" << metadata.pruned_memo_entries
        << ",\"analyzer_tables_before\":" << working.before_release.tables_bytes
        << ",\"analyzer_arguments_before\":"
        << working.before_release.arguments_bytes
        << ",\"analyzer_cache_before\":" << working.before_release.cache_bytes
        << ",\"analyzer_tables_after\":" << working.after_release.tables_bytes
        << ",\"analyzer_arguments_after\":"
        << working.after_release.arguments_bytes
        << ",\"analyzer_cache_after\":" << working.after_release.cache_bytes
        << ",\"checked_ns\":" << ns(begin, checked)
        << ",\"stage_ns\":" << ns(checked, built)
        << ",\"query_ns\":" << ns(query_begin, queried)
        << ",\"nodes\":" << view->node_count()
        << ",\"hir_expressions\":" << expressions
        << ",\"path_handles\":" << paths.paths
        << ",\"path_logical_steps\":" << paths.logical_steps
        << ",\"path_unique_steps\":" << paths.unique_steps
        << ",\"path_storage_bytes\":" << paths.bytes
        << ",\"sites\":" << analysis.sites().size()
        << ",\"roots\":" << analysis.roots().size()
        << ",\"decisions\":" << analysis.decisions().size()
        << ",\"files\":" << files << ",\"file_bytes\":" << file_bytes
        << ",\"cas_files\":" << cas_files << ",\"cas_bytes\":" << cas_bytes
        << ",\"written_blocks\":" << build.written_blocks
        << ",\"written_payload_bytes\":" << build.payload_bytes_written
        << ",\"build_reads\":" << build.objects_read
        << ",\"build_read_bytes\":" << build.payload_bytes_read
        << ",\"query_reads\":" << read.objects_read
        << ",\"query_read_bytes\":" << read.payload_bytes_read
        << ",\"released_bytes\":" << build.released_active_bytes
        << ",\"peak_owned_bytes\":" << build.peak_owned_bytes
        << ",\"directory_bytes\":" << build.directory_bytes
        << ",\"summary_bytes\":" << build.summary_bytes
        << ",\"query_states\":" << states << ",\"query_sources\":" << sources
        << ",\"fuel\":" << analyzer.stats().fuel_consumed
        << ",\"materialized\":" << (analysis.graph_materialized() ? 1 : 0)
        << ",\"signature\":\"" << signature << "\"}\n";
}
