#include "build_identity.h"
#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/provenance_topology.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace pagos;
using namespace pagos::stage;
using namespace pagos::stage::provenance;
namespace fs = std::filesystem;
namespace {
using Clock = std::chrono::steady_clock;
auto elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                start)
        .count();
}
auto work(const TopologyStats& s) {
    return std::array{s.nodes,         s.edges,          s.summary_links,
                      s.formulas,      s.formula_terms,  s.formula_states,
                      s.formula_edges, s.formula_reuses, s.source_families};
}
auto work(const QueryStats& s) {
    return std::array{s.admitted_states, s.visited_states, s.scanned_edges,
                      s.visited_edges,   s.bindings,       s.witness_steps,
                      s.queue_peak};
}
std::size_t owned(TopologyStateMemory s) {
    return s.queue + s.visited + s.formula_records + s.formula_terms +
           s.formula_index + s.calls + s.local_queue + s.local_seen +
           s.families + s.delivery;
}
std::size_t owned(QueryStateMemory s) {
    return s.queue + s.visited + s.contexts + s.context_index + s.sources +
           s.witness + s.cached;
}
void require(bool value) {
    if (!value)
        throw std::runtime_error("benchmark invariant failed");
}
cache::Digest hash(std::string_view value) {
    return cache::fingerprint(
        "query-task-benchmark.result",
        {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
struct Measurement {
    long long prefix_ns{}, save_ns{}, restore_ns{}, continue_ns{};
    std::size_t before{}, after_save{}, restored{}, final{}, peak_owned{},
        directory{}, codec{}, logical_bytes{}, cas_added{}, save_segments{},
        restore_segments{}, reads{}, writes{}, paging_reads{}, paging_writes{},
        paging_bytes_read{}, paging_bytes_written{};
    std::vector<std::size_t> final_work;
    cache::Digest result;
};
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 6)
            return 2;
        const std::string mode = argv[1], phase = argv[5];
        const bool topology = mode.starts_with("topology-");
        const bool segmented = mode == "topology-segmented",
                   paged = mode == "native-paged";
        if (mode != "publish" && mode != "native-memory" && !paged &&
            mode != "topology-resident" && !segmented)
            return 2;
        if (phase != "frontier" && phase != "formula")
            return 2;
        auto source = source::SourceFile::load(argv[2]);
        require(source.has_value());
        source::DiagnosticEngine diagnostics(*source);
        syntax::Lexer lexer(*source, diagnostics);
        auto tokens = lexer.tokenize();
        syntax::Parser parser(tokens, diagnostics);
        auto module = parser.parse_module();
        sema::TypeChecker checker(diagnostics, 32);
        require(module && checker.check(*module) && !diagnostics.has_error());
        cache::QueryContext context{
            .module = {"query-task-benchmark", "input.pgs"},
            .target = {"riscv32-unknown-elf", "generic-rv32", "", "e-p:32:32",
                       32, true},
            .compiler_build = pagos_cache_build_id,
            .semantic_options = "pagos.staging.v1"};
        auto identity = build_cache_identity(*module, checker.types(), context);
        require(identity.has_value());
        const cache::FileStore evidence_store(argv[3]);
        if (mode == "publish") {
            StageAnalyzer analyzer(diagnostics, checker.types(), {}, context);
            auto staged = analyzer.analyze(*module);
            require(staged && !diagnostics.has_error());
            require(FunctionEvidence::publish(
                        evidence_store, *staged->provenance, *identity, context)
                        .has_value());
            std::cout << "{\"stage_runs\":1}\n";
            return 0;
        }
        // Query children only load published evidence. Front-end state remains
        // live for every mode; whole-child RSS is not a query-buffer bound.
        const auto open = [&] {
            auto closure =
                FunctionEvidence::load(evidence_store, *identity, context);
            require(closure.has_value());
            auto view = FunctionEvidenceView::open(std::move(*closure),
                                                   4UZ * 1024 * 1024);
            require(view.has_value());
            return std::move(*view);
        };
        const auto id = cache::fingerprint(
            topology ? "benchmark.topology" : "benchmark.native", {});
        Measurement m;
        cache::TaskStoreOptions options;
        options.store.fault = [&](cache::IoOperation operation,
                                  const fs::path&) {
            m.reads += operation == cache::IoOperation::ReadFile ? 1U : 0U;
            m.writes +=
                operation == cache::IoOperation::WriteTemporary ? 1U : 0U;
            return std::error_code{};
        };
        options.disk_bytes = 1024UZ * 1024 * 1024;
        auto store = cache::TaskStore::open(
            argv[4], id, cache::TaskAccess::ReadWrite, options);
        require(store.has_value());
        std::optional<cache::TaskHead> head;
        NodeId root;
        std::vector<std::size_t> prefix_work;
        constexpr QuerySlice slice{64, 512, 64, 256};
        const QueryPagingConfig paging{.segment_bytes = 16384,
                                       .resident_bytes = 262144,
                                       .cache_bytes = 65536};
        {
            auto view = open();
            for (const auto& item : view.roots())
                if (item.name == "aa")
                    root = item.node;
            require(root.value != 0);
            auto binding = view.checkpoint_binding(source->text());
            require(binding.has_value());
            if (topology) {
                TopologyTask task(*binding, id, root);
                const auto start = Clock::now();
                do {
                    const auto page = task.advance(
                        phase == "formula" ? QuerySlice{1, 64, 64, 1024}
                                           : slice);
                    require(page.progress != QueryProgress::Failed);
                    if (page.sequence)
                        require(task.acknowledge(page.sequence));
                    m.peak_owned =
                        std::max(m.peak_owned, owned(task.state_memory()));
                    if (phase != "formula" ||
                        task.stats().formula_states >=
                            std::max(std::size_t{1}, task.stats().nodes / 2) ||
                        page.progress == QueryProgress::Completed)
                        break;
                } while (true);
                m.prefix_ns = elapsed(start);
                const auto counters = work(task.stats());
                prefix_work.assign(counters.begin(), counters.end());
                m.before = owned(task.state_memory());
                const auto saving = Clock::now();
                auto saved = segmented ? task.offload(*store, 0, 16384)
                                       : task.save(*store, 0);
                require(saved.has_value());
                head = *saved;
                m.save_ns = elapsed(saving);
                m.after_save = owned(task.state_memory());
                if (segmented)
                    require(m.after_save == 0);
                const auto io = task.storage_stats();
                m.codec = io.peak_codec_buffers;
                m.logical_bytes = io.logical_bytes;
                m.cas_added = io.cas_bytes_added;
                m.save_segments = io.segment_writes;
            } else {
                auto started =
                    paged ? QueryTask::begin_paged(*binding, *store, root,
                                                   QueryKind::Value, false,
                                                   paging)
                          : std::expected<QueryTask, CheckpointError>(
                                QueryTask(*binding, id, root));
                require(started.has_value());
                auto task = std::move(*started);
                const auto start = Clock::now();
                (void)task.advance({64, 512, 64, 0});
                require(!task.storage_failure() &&
                        task.progress() != QueryProgress::Failed);
                m.prefix_ns = elapsed(start);
                const auto counters = work(task.stats());
                prefix_work.assign(counters.begin(), counters.end());
                m.before = owned(task.state_memory());
                m.directory = task.state_memory().directory;
                const auto saving = Clock::now();
                auto saved =
                    paged ? task.offload(*store, 0) : task.save(*store, 0);
                require(saved.has_value());
                head = *saved;
                m.save_ns = elapsed(saving);
                m.after_save = owned(task.state_memory());
                if (paged)
                    require(m.after_save == 0);
            }
        } // Release the original task/view before measuring restoration.
        {
            auto view = open();
            auto binding = view.checkpoint_binding(source->text());
            require(binding.has_value());
            const auto begin = Clock::now();
            if (topology) {
                auto task =
                    TopologyTask::restore(*binding, *store, *head, root);
                require(task.has_value());
                m.restore_ns = elapsed(begin);
                const auto counters = work(task->stats());
                require(prefix_work == std::vector<std::size_t>(
                                           counters.begin(), counters.end()));
                m.restored = owned(task->state_memory());
                m.codec =
                    std::max(m.codec, task->storage_stats().peak_codec_buffers);
                m.restore_segments = task->storage_stats().segment_reads;
                const auto continuation = Clock::now();
                for (;;) {
                    auto page = task->advance(slice);
                    require(!task->storage_failure() &&
                            page.progress != QueryProgress::Failed);
                    if (page.sequence)
                        require(task->acknowledge(page.sequence));
                    m.peak_owned =
                        std::max(m.peak_owned, owned(task->state_memory()));
                    if (page.progress == QueryProgress::Completed)
                        break;
                }
                m.continue_ns = elapsed(continuation);
                m.final = owned(task->state_memory());
                const auto final = work(task->stats());
                m.final_work.assign(final.begin(), final.end());
                std::string content;
                for (const auto& formula : task->index()->formulas()) {
                    content +=
                        std::to_string(formula.root.value) + ":" +
                        std::to_string(static_cast<unsigned>(formula.kind)) +
                        ":";
                    for (auto term : formula.terms)
                        content += std::to_string(term.value) + ",";
                    content += std::string(formula.fallback) + ";";
                }
                m.result = hash(content);
            } else {
                auto task =
                    paged ? QueryTask::restore_paged(*binding, *store, *head,
                                                     root, QueryKind::Value,
                                                     false, paging)
                          : QueryTask::restore(*binding, *store, *head, root,
                                               QueryKind::Value, false);
                require(task.has_value());
                m.restore_ns = elapsed(begin);
                const auto counters = work(task->stats());
                require(prefix_work == std::vector<std::size_t>(
                                           counters.begin(), counters.end()));
                m.restored = owned(task->state_memory());
                const auto continuation = Clock::now();
                for (;;) {
                    const auto page = task->advance(slice);
                    require(!task->storage_failure() &&
                            page.progress != QueryProgress::Failed);
                    if (page.sequence)
                        require(task->acknowledge(page.sequence));
                    m.peak_owned =
                        std::max(m.peak_owned, owned(task->state_memory()));
                    m.directory =
                        std::max(m.directory, task->state_memory().directory);
                    if (paged)
                        require(owned(task->state_memory()) <=
                                paging.resident_bytes);
                    if (page.progress == QueryProgress::Completed &&
                        !page.pending_sources)
                        break;
                }
                m.continue_ns = elapsed(continuation);
                m.final = owned(task->state_memory());
                const auto io = task->paging_stats();
                m.paging_reads = io.reads;
                m.paging_writes = io.writes;
                m.paging_bytes_read = io.bytes_read;
                m.paging_bytes_written = io.bytes_written;
                const auto final = work(task->stats());
                m.final_work.assign(final.begin(), final.end());
                std::string content;
                const auto result = task->result(); // Detached compatibility
                                                    // copy, after phase clocks.
                for (const auto& item : result.sources) {
                    content += std::to_string(item.node.value) + ":" +
                               std::to_string(item.site);
                    for (auto frame = item.context; frame;
                         frame = result.invocations[frame].parent)
                        content +=
                            ":" + std::to_string(
                                      result.invocations[frame].call.value);
                    content += ";";
                }
                m.result = hash(content);
            }
        }
        std::size_t disk_bytes = 0, files = 0;
        for (const auto& item :
             fs::recursive_directory_iterator(store->directory()))
            if (item.is_regular_file()) {
                disk_bytes += item.file_size();
                ++files;
            }
        std::cout << "{\"stage_runs\":0,\"prefix_ns\":" << m.prefix_ns
                  << ",\"save_ns\":" << m.save_ns
                  << ",\"restore_ns\":" << m.restore_ns
                  << ",\"continue_ns\":" << m.continue_ns
                  << ",\"before_bytes\":" << m.before
                  << ",\"after_save_bytes\":" << m.after_save
                  << ",\"restored_bytes\":" << m.restored
                  << ",\"final_bytes\":" << m.final
                  << ",\"sampled_peak_owned\":" << m.peak_owned
                  << ",\"directory_bytes\":" << m.directory
                  << ",\"codec_buffers\":" << m.codec
                  << ",\"logical_table_bytes\":" << m.logical_bytes
                  << ",\"save_cas_added\":" << m.cas_added
                  << ",\"save_segments\":" << m.save_segments
                  << ",\"restore_segments\":" << m.restore_segments
                  << ",\"task_read_requests\":" << m.reads
                  << ",\"task_write_requests\":" << m.writes
                  << ",\"paging_reads\":" << m.paging_reads
                  << ",\"paging_writes\":" << m.paging_writes
                  << ",\"paging_bytes_read\":" << m.paging_bytes_read
                  << ",\"paging_bytes_written\":" << m.paging_bytes_written
                  << ",\"disk_bytes\":" << disk_bytes << ",\"files\":" << files
                  << ",\"result\":\"" << m.result.hex() << "\",\"work\":[";
        for (std::size_t i = 0; i < m.final_work.size(); ++i) {
            if (i)
                std::cout << ',';
            std::cout << m.final_work[i];
        }
        std::cout << "]}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
