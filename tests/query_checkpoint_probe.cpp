#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace pagos;
using namespace pagos::stage;
using namespace pagos::stage::provenance;
namespace fs = std::filesystem;
namespace {
cache::QueryContext context() {
    return {.module = {"checkpoint-test", "input.pgs"},
            .target = {"riscv32-unknown-elf", "generic-rv32", "", "e-p:32:32",
                       32, true},
            .compiler_build = "checkpoint-probe-v1",
            .semantic_options = "pagos.staging.v1"};
}
cache::Digest parse_digest(std::string_view text) {
    cache::Digest value;
    if (text.size() != 64)
        throw std::runtime_error("bad digest");
    for (unsigned i = 0; i < 32; ++i) {
        unsigned byte{};
        const auto part = text.substr(i * 2, 2);
        const auto result =
            std::from_chars(part.data(), part.data() + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != part.data() + 2)
            throw std::runtime_error("bad digest");
        value.bytes[i] = static_cast<std::uint8_t>(byte);
    }
    return value;
}
void print_page(const QueryPage& page, bool& first) {
    if (!page.sequence)
        return;
    if (!first)
        std::cout << ',';
    first = false;
    std::cout << "{\"sequence\":" << page.sequence << ",\"records\":[";
    bool first_record = true;
    for (const auto& source : page.sources) {
        if (!first_record)
            std::cout << ',';
        first_record = false;
        std::cout << "\"S:" << source.node.value << ':' << source.site << ':'
                  << source.context << "\"";
    }
    for (const auto& state : page.witness) {
        if (!first_record)
            std::cout << ',';
        first_record = false;
        std::cout << "\"W:" << state.node.value << ':' << state.context << "\"";
    }
    std::cout << "]}";
}
void print_result(const QueryTask& task) {
    const auto result = task.result();
    std::string content;
    for (const auto& source : result.sources)
        content += "S" + std::to_string(source.node.value) + ":" +
                   std::to_string(source.site) + ":" +
                   std::to_string(source.context) + ";";
    for (const auto& state : result.witness)
        content += "W" + std::to_string(state.node.value) + ":" +
                   std::to_string(state.context) + ";";
    for (const auto& invocation : result.invocations)
        content += "I" + std::to_string(invocation.call.value) + ":" +
                   std::to_string(invocation.parent) + ":" +
                   std::to_string(invocation.owner.value) + ";";
    const auto digest = cache::fingerprint(
        "test.query-result",
        {reinterpret_cast<const std::uint8_t*>(content.data()),
         content.size()});
    const auto& stats = task.stats();
    std::cout << ",\"result\":\"" << digest.hex() << "\",\"work\":["
              << stats.admitted_states << ',' << stats.visited_states << ','
              << stats.scanned_edges << ',' << stats.visited_edges << ','
              << stats.bindings << ',' << stats.witness_steps << ','
              << stats.queue_peak << "],\"issues\":" << task.coverage().issues
              << ",\"stage_runs\":0}" << '\n';
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc < 4)
            return 2;
        const std::string original_action = argv[1];
        const bool paged = original_action.starts_with("page-");
        const bool segmented = paged || original_action.starts_with("seg-");
        std::string action = paged       ? original_action.substr(5)
                             : segmented ? original_action.substr(4)
                                         : original_action;
        const QueryPagingConfig paging{
            .segment_bytes = 256, .resident_bytes = 4096, .cache_bytes = 512};
        const fs::path directory = argv[2];
        const auto task_id = cache::fingerprint("probe.logical-query.1", {});
        if (action == "cleanup" || action == "cleanup-all") {
            const auto cleaned = cache::reclaim_tasks(
                directory / "sessions",
                action == "cleanup" ? cache::TaskCleanupPolicy::TerminalOnly
                                    : cache::TaskCleanupPolicy::AllInactive);
            if (!cleaned)
                throw std::runtime_error("cleanup failed");
            std::cout << "{\"reclaimed\":" << cleaned->reclaimed
                      << ",\"active\":" << cleaned->active
                      << ",\"skipped\":" << cleaned->skipped << "}\n";
            return 0;
        }
        if (action == "head" || action == "hold-reader") {
            auto store = cache::TaskStore::open(directory / "sessions", task_id,
                                                cache::TaskAccess::ReadOnly);
            if (!store)
                throw std::runtime_error("task unavailable");
            if (action == "hold-reader") {
                {
                    auto another =
                        cache::TaskStore::open(directory / "sessions", task_id,
                                               cache::TaskAccess::ReadOnly);
                    if (!another)
                        throw std::runtime_error("second reader failed");
                }
                std::ofstream(directory / "ready") << "reader";
                (void)std::cin.get();
            }
            const auto head = store->head();
            if (!head || !*head)
                throw std::runtime_error("head unavailable");
            std::cout << "{\"generation\":" << (**head).generation << "}\n";
            return 0;
        }
        const bool crashing =
            action == "crash-before" || action == "crash-after";
        const bool cancelled = action == "cancel";
        std::optional<cache::TaskStore> task_store;
        if (segmented) {
            cache::TaskStoreOptions options;
            options.store.fault = [&](cache::IoOperation operation,
                                      const fs::path& path) {
                const auto selected =
                    original_action == "seg-crash-before"
                        ? cache::IoOperation::PublishManifest
                        : cache::IoOperation::ManifestPublished;
                if (crashing && path.filename() == "head" &&
                    operation == selected) {
                    std::ofstream(directory / "ready") << "publishing";
                    (void)std::cin.get();
                }
                return std::error_code{};
            };
            auto opened = cache::TaskStore::open(
                directory / "sessions", task_id,
                action == "resume" && !paged ? cache::TaskAccess::ReadOnly
                                             : cache::TaskAccess::ReadWrite,
                options);
            if (!opened)
                throw std::runtime_error("task unavailable");
            task_store = std::move(*opened);
            if (crashing || cancelled)
                action = "save";
        }
        std::ifstream stream(argv[3], std::ios::binary);
        if (!stream)
            throw std::runtime_error("source missing");
        auto source = source::SourceFile::from_text(
            "input.pgs",
            std::string(std::istreambuf_iterator<char>(stream), {}));
        source::DiagnosticEngine diagnostics(source);
        syntax::Lexer lexer(source, diagnostics);
        const auto tokens = lexer.tokenize();
        syntax::Parser parser(tokens, diagnostics);
        const auto module = parser.parse_module();
        sema::TypeChecker checker(diagnostics, 32);
        if (!module || !checker.check(*module) || diagnostics.has_error())
            throw std::runtime_error("source check failed");
        auto identity =
            build_cache_identity(*module, checker.types(), context());
        if (!identity)
            throw std::runtime_error("identity failed");
        if (action == "prepare") {
            StageAnalyzer analyzer(diagnostics, checker.types(), {}, context());
            const auto staged = analyzer.analyze(*module);
            if (!staged || diagnostics.has_error() ||
                !FunctionEvidence::publish(
                    cache::FileStore(directory / "evidence"),
                    *staged->provenance, *identity, context()))
                throw std::runtime_error("publication failed");
            std::cout << "{\"stage_runs\":1}\n";
            return 0;
        }
        // All other actions have no StageAnalyzer path.
        auto closure = FunctionEvidence::load(
            cache::FileStore(directory / "evidence"), *identity, context());
        if (!closure)
            throw std::runtime_error("closure unavailable");
        auto view = FunctionEvidenceView::open(std::move(*closure), 0);
        if (!view)
            throw std::runtime_error("view unavailable");
        auto binding = view->checkpoint_binding(source.text());
        if (!binding || argc < 5)
            return 2;
        const bool witness = std::string_view(argv[4]) == "witness";
        NodeId root;
        for (const auto& item : view->roots())
            if (item.name == "aa")
                root = item.node;
        QueryTask task(*binding, task_id, root, QueryKind::Value, witness);
        if (paged && action == "save") {
            auto started = QueryTask::begin_paged(
                *binding, *task_store, root, QueryKind::Value, witness, paging);
            if (!started)
                throw std::runtime_error("paged query cannot start");
            task = std::move(*started);
        }
        if (action == "resume" && segmented) {
            const auto head = task_store->head();
            if (!head || !*head)
                throw std::runtime_error("head unavailable");
            auto restored =
                paged ? QueryTask::restore_paged(*binding, *task_store, **head,
                                                 root, QueryKind::Value,
                                                 witness, paging)
                      : QueryTask::restore(*binding, *task_store, **head, root,
                                           QueryKind::Value, witness);
            if (!restored)
                throw std::runtime_error("checkpoint cannot resume");
            task = std::move(*restored);
            task.resume();
        } else if (action == "resume") {
            if (argc != 7)
                return 2;
            const cache::ObjectRef ref{cache::ObjectKind::QueryCheckpoint,
                                       parse_digest(argv[5]),
                                       std::stoull(argv[6])};
            auto bytes = cache::FileStore(directory / "tasks").read_object(ref);
            if (!bytes)
                throw std::runtime_error("checkpoint object unavailable");
            auto restored = QueryTask::restore(
                *binding, task_id, root, QueryKind::Value, witness, *bytes);
            if (!restored)
                throw std::runtime_error("checkpoint cannot resume: " +
                                         std::to_string(static_cast<unsigned>(
                                             restored.error().code)));
            task = std::move(*restored);
        }
        std::cout << "{\"pages\":[";
        bool first = true;
        const auto check_buffers = [&] {
            if (!paged)
                return;
            const auto memory = task.state_memory();
            const auto buffers = memory.queue + memory.visited +
                                 memory.contexts + memory.context_index +
                                 memory.sources + memory.witness +
                                 memory.cached;
            if (task.storage_failure() || buffers > paging.resident_bytes ||
                memory.peak_buffers > paging.resident_bytes ||
                memory.cached > paging.cache_bytes)
                throw std::runtime_error("paged query failed its buffer bound");
        };
        if (action == "save") {
            if (argc != 7)
                return 2;
            const auto steps = std::stoull(argv[5]);
            const bool ack = std::string_view(argv[6]) == "ack";
            for (std::uint64_t i = 0; i < steps; ++i) {
                const auto page = task.advance({1, 1, 1, 1});
                check_buffers();
                print_page(page, first);
                if (page.sequence) {
                    if (!ack)
                        break;
                    if (!task.acknowledge(page.sequence))
                        throw std::runtime_error("ack failed");
                }
                if (page.progress == QueryProgress::Completed &&
                    !page.pending_sources && !page.pending_witness)
                    break;
            }
            if (segmented) {
                if (cancelled)
                    task.cancel();
                const auto previous = task_store->head();
                if (!previous)
                    throw std::runtime_error("head unavailable");
                const auto generation = *previous ? (**previous).generation : 0;
                auto head = task.offload(*task_store, generation, 256);
                if (!head)
                    throw std::runtime_error("offload failed");
                const auto memory = task.state_memory();
                const bool released = memory.offloaded && memory.cached == 0 &&
                                      memory.directory == 0 &&
                                      memory.queue + memory.visited +
                                              memory.contexts +
                                              memory.context_index +
                                              memory.sources + memory.witness ==
                                          0;
                if (!released)
                    throw std::runtime_error("state capacity retained");
                std::cout << "],\"digest\":\"" << head->manifest.digest.hex()
                          << "\",\"size\":" << head->manifest.size
                          << ",\"generation\":" << head->generation
                          << ",\"released\":true";
            } else {
                auto bytes = task.checkpoint();
                if (!bytes)
                    throw std::runtime_error("encode failed");
                auto ref = cache::FileStore(directory / "tasks")
                               .write_object(cache::ObjectKind::QueryCheckpoint,
                                             std::move(*bytes));
                if (!ref)
                    throw std::runtime_error("write failed");
                std::cout << "],\"digest\":\"" << ref->digest.hex()
                          << "\",\"size\":" << ref->size;
            }
        } else if (action == "resume" || action == "oracle") {
            for (;;) {
                // Different budgets from the saving process, including output.
                const auto page = task.advance({17, 23, 19, 7});
                check_buffers();
                print_page(page, first);
                if (page.progress == QueryProgress::Failed)
                    throw std::runtime_error("query failed");
                if (page.sequence && !task.acknowledge(page.sequence))
                    throw std::runtime_error("ack failed");
                if (page.progress == QueryProgress::Completed &&
                    !page.pending_sources && !page.pending_witness)
                    break;
            }
            std::cout << ']';
        } else
            return 2;
        if (paged) {
            const auto stats = task.paging_stats();
            std::cout << ",\"paged\":true,\"reads\":" << stats.reads
                      << ",\"writes\":" << stats.writes;
        }
        print_result(task);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
