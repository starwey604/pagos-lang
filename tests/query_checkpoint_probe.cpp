#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/provenance_recursive.h"
#include "pagos/stage/provenance_topology.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

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
int recursive_probe(std::string_view action, const fs::path& directory,
                    const FunctionEvidenceView& view,
                    QueryCheckpointBinding binding, int argc, char** argv) {
    if (argc < 6)
        return 2;
    const bool segmented = action.starts_with("rec-seg-");
    const std::string normalized = segmented
                                       ? "rec-" + std::string(action.substr(8))
                                       : std::string(action);
    action = normalized;
    const auto parent = directory / "recursive-sessions" /
                        (std::string(argv[4]) + "-" + argv[5]);
    if (action == "rec-cleanup-all" || action == "rec-cleanup") {
        const auto cleaned = cache::reclaim_tasks(
            parent, action == "rec-cleanup-all"
                        ? cache::TaskCleanupPolicy::AllInactive
                        : cache::TaskCleanupPolicy::TerminalOnly);
        if (!cleaned)
            throw std::runtime_error("recursive cleanup failed");
        std::cout << "{\"reclaimed\":" << cleaned->reclaimed
                  << ",\"skipped\":" << cleaned->skipped << "}\n";
        return 0;
    }
    SummaryId selected;
    for (const auto& summary : view.summary_catalog())
        if (view.scopes()[summary.id.value].name == argv[4])
            selected = summary.id;
    if (!selected.value)
        throw std::runtime_error("recursive summary missing");
    const auto channel = std::string_view(argv[5]) == "effects"
                             ? QueryKind::Effects
                             : QueryKind::Value;
    const auto id = cache::fingerprint("probe.recursive-task.1", {});
    RecursiveSummaryTask task(binding, id, selected, channel);
    cache::FileStore store(directory / "recursive-state");
    if (action == "rec-resume") {
        if (argc != 8)
            return 2;
        if (segmented) {
            auto session =
                cache::TaskStore::open(parent, id, cache::TaskAccess::ReadOnly);
            if (!session)
                throw std::runtime_error("recursive session unavailable");
            auto head = session->head();
            if (!head || !*head)
                throw std::runtime_error("recursive head unavailable");
            if ((**head).manifest.digest != parse_digest(argv[6]) ||
                (**head).manifest.size != std::stoull(argv[7]))
                throw std::runtime_error("recursive head changed");
            auto restored = RecursiveSummaryTask::restore(
                binding, *session, **head, selected, channel);
            if (!restored)
                throw std::runtime_error("recursive cannot resume");
            task = std::move(*restored);
        } else {
            auto bytes = store.read_object(
                {cache::ObjectKind::QueryTaskManifest, parse_digest(argv[6]),
                 std::stoull(argv[7])});
            if (!bytes)
                throw std::runtime_error("recursive object unavailable");
            auto restored = RecursiveSummaryTask::restore(binding, id, selected,
                                                          channel, *bytes);
            if (!restored)
                throw std::runtime_error("recursive cannot resume");
            task = std::move(*restored);
        }
        task.resume();
    }
    if (action == "rec-save" || action == "rec-save-phase") {
        if (argc != 8)
            return 2;
        const auto cut = std::stoull(argv[6]);
        if (action == "rec-save")
            (void)task.advance(static_cast<std::size_t>(cut));
        else {
            for (std::size_t i = 0; i < 10000; ++i) {
                const auto bytes = task.checkpoint();
                if (!bytes)
                    throw std::runtime_error("recursive checkpoint failed");
                if (bytes->at(159) == cut)
                    break;
                if (task.progress() == QueryProgress::Completed)
                    throw std::runtime_error("recursive phase unreachable");
                (void)task.advance(1);
            }
        }
        if (std::string_view(argv[7]) == "cancel")
            task.cancel();
        auto bytes = task.checkpoint();
        if (!bytes)
            throw std::runtime_error("recursive checkpoint failed");
        const auto phase = bytes->at(159);
        if (segmented) {
            auto session = cache::TaskStore::open(parent, id,
                                                  cache::TaskAccess::ReadWrite);
            if (!session)
                throw std::runtime_error("recursive session unavailable");
            auto old = session->head();
            if (!old)
                throw std::runtime_error("recursive head unavailable");
            auto head =
                task.offload(*session, *old ? (**old).generation : 0, 128);
            if (!head || !task.state_memory().offloaded)
                throw std::runtime_error("recursive offload failed");
            std::cout << "{\"digest\":\"" << head->manifest.digest.hex()
                      << "\",\"size\":" << head->manifest.size
                      << ",\"phase\":" << static_cast<unsigned>(phase)
                      << ",\"stage_runs\":0,\"steps\":" << task.stats().steps
                      << ",\"released\":true,\"segments\":"
                      << task.storage_stats().segment_writes << "}\n";
            return 0;
        }
        auto ref = store.write_object(cache::ObjectKind::QueryTaskManifest,
                                      std::move(*bytes));
        if (!ref)
            throw std::runtime_error("recursive publication failed");
        std::cout << "{\"digest\":\"" << ref->digest.hex()
                  << "\",\"size\":" << ref->size
                  << ",\"phase\":" << static_cast<unsigned>(phase)
                  << ",\"stage_runs\":0,\"steps\":" << task.stats().steps
                  << "}\n";
        return 0;
    }
    while (task.progress() == QueryProgress::Running ||
           task.progress() == QueryProgress::Paused)
        (void)task.advance(13);
    if (task.progress() != QueryProgress::Completed)
        throw std::runtime_error("recursive query failed");
    const auto result = task.result();
    std::ostringstream content;
    content << static_cast<unsigned>(result.precision) << ':' << result.issues
            << ':' << result.instances_merged;
    for (const auto& atom : result.atoms)
        content << "A" << static_cast<unsigned>(atom.kind) << ':' << atom.owner
                << ':' << atom.slot << ':' << atom.node << ';';
    for (const auto& link : task.links())
        content << "L" << link.from.value << ':' << link.to.value << ':'
                << static_cast<unsigned>(link.kind) << ':'
                << static_cast<unsigned>(link.relation) << ':' << link.slot
                << ';';
    for (const auto& component : task.components()) {
        content << "C" << component.cyclic << ':';
        for (const auto node : component.nodes)
            content << node.value << ',';
        content << ':';
        for (const auto summary : component.summaries)
            content << summary.value << ',';
        content << ';';
    }
    const auto text = content.str();
    const auto hash = cache::fingerprint(
        "test.recursive-result",
        {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    const auto& work = task.stats();
    std::cout << "{\"result\":\"" << hash.hex() << "\",\"work\":[" << work.steps
              << ',' << work.nodes << ',' << work.links << ','
              << work.components << ',' << work.cyclic_components << ','
              << work.evaluations << ',' << work.facts_added << ','
              << work.queue_peak << "],\"stage_runs\":0}\n";
    return 0;
}
int topology_probe(std::string_view action, const fs::path& directory,
                   const FunctionEvidenceView& view,
                   QueryCheckpointBinding binding, NodeId root, int argc,
                   char** argv) {
    const bool segmented = action.starts_with("topo-seg-");
    const std::string normalized = segmented
                                       ? "topo-" + std::string(action.substr(9))
                                       : std::string(action);
    action = normalized;
    const auto id = cache::fingerprint("probe.symbolic-topology.1", {});
    TopologyTask task(binding, id, root);
    std::optional<cache::TaskStore> store;
    if (action != "topo-oracle") {
        auto opened = cache::TaskStore::open(
            directory / "sessions", id,
            action == "topo-resume" ? cache::TaskAccess::ReadOnly
                                    : cache::TaskAccess::ReadWrite);
        if (!opened)
            throw std::runtime_error("topology store unavailable");
        store = std::move(*opened);
    }
    if (action == "topo-resume") {
        const auto head = store->head();
        if (!head || !*head)
            throw std::runtime_error("topology head missing");
        auto restored = TopologyTask::restore(binding, *store, **head, root);
        if (!restored)
            throw std::runtime_error("topology cannot resume");
        task = std::move(*restored);
        task.resume();
    }
    std::cout << "{\"pages\":[";
    bool first = true;
    const auto print = [&](const TopologyPage& page) {
        if (!page.sequence)
            return;
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"sequence\":" << page.sequence << ",\"records\":[";
        bool first_record = true;
        for (const auto& item : page.records) {
            if (!first_record)
                std::cout << ',';
            first_record = false;
            std::cout << '\"' << static_cast<unsigned>(item.kind) << ':'
                      << item.node.value << ':' << item.edge.target.value << ':'
                      << static_cast<unsigned>(item.edge.relation) << ':'
                      << item.summary.value << ':'
                      << static_cast<unsigned>(item.port) << '\"';
        }
        std::cout << "]}";
    };
    if (action == "topo-save" || action == "topo-save-formula") {
        if (argc != 6)
            return 2;
        if (action == "topo-save-formula") {
            for (unsigned step = 0; !task.stats().formula_states; ++step) {
                const auto page = task.advance({1, 1, 1, 1});
                if (page.progress == QueryProgress::Failed || step > 100000)
                    throw std::runtime_error("formula preparation unavailable");
                print(page);
                if (page.sequence && !task.acknowledge(page.sequence))
                    throw std::runtime_error("formula prefix ack failed");
            }
        }
        const auto steps = std::stoull(argv[4]);
        const bool ack = std::string_view(argv[5]) == "ack";
        for (std::uint64_t i = 0; i < steps; ++i) {
            const auto page = task.advance({1, 1, 1, 1});
            if (page.progress == QueryProgress::Failed)
                throw std::runtime_error("topology failed");
            print(page);
            if (page.sequence) {
                if (!ack)
                    break;
                if (!task.acknowledge(page.sequence))
                    throw std::runtime_error("topology ack failed");
            }
            if (page.progress == QueryProgress::Completed)
                break;
        }
        const auto previous = store->head();
        if (!previous)
            throw std::runtime_error("topology head unavailable");
        const auto generation = *previous ? (**previous).generation : 0;
        auto head = segmented ? task.offload(*store, generation, 128)
                              : task.save(*store, generation);
        if (segmented &&
            (!task.state_memory().offloaded || task.state_memory().queue ||
             task.state_memory().formula_records ||
             task.state_memory().delivery))
            throw std::runtime_error(
                "topology capacities retained after offload");
        if (!head)
            throw std::runtime_error("topology save failed");
    } else if (action == "topo-resume" || action == "topo-oracle") {
        for (unsigned step = 0; step < 100000; ++step) {
            const auto page = task.advance({17, 23, 19, 7});
            if (page.progress == QueryProgress::Failed)
                throw std::runtime_error("topology failed");
            print(page);
            if (page.sequence && !task.acknowledge(page.sequence))
                throw std::runtime_error("topology ack failed");
            if (page.progress == QueryProgress::Completed)
                break;
            if (step == 99999)
                throw std::runtime_error("topology stuck");
        }
    } else
        return 2;
    std::cout << ']';
    std::string formulas;
    if (const auto index = task.index()) {
        for (const auto& formula : index->formulas()) {
            formulas += std::to_string(formula.root.value) + ":" +
                        std::to_string(static_cast<unsigned>(formula.kind)) +
                        ":";
            for (auto term : formula.terms)
                formulas += std::to_string(term.value) + ",";
            formulas += std::string(formula.fallback) + ";";
        }
        for (auto kind : {QueryKind::Value, QueryKind::Effects}) {
            auto compressed = index->begin_sources(root, kind);
            auto native = QueryTask(view.view(), root, kind);
            (void)compressed.advance(QuerySlice::unlimited());
            (void)native.advance(QuerySlice::unlimited());
            const auto instances = [](const QueryResult& result) {
                std::vector<std::string> values;
                for (const auto& source : result.sources) {
                    auto value = std::to_string(source.node.value) + ":" +
                                 std::to_string(source.site);
                    for (auto frame = source.context; frame;
                         frame = result.invocations[frame].parent)
                        value +=
                            ":" + std::to_string(
                                      result.invocations[frame].call.value);
                    values.push_back(std::move(value));
                }
                std::ranges::sort(values);
                return values;
            };
            if (instances(compressed.result()) != instances(native.result()) ||
                compressed.result().complete != native.result().complete)
                throw std::runtime_error(
                    "restored formulas changed query results");
        }
    }
    const auto result = cache::fingerprint(
        "test.symbolic-formulas",
        {reinterpret_cast<const std::uint8_t*>(formulas.data()),
         formulas.size()});
    const auto& stats = task.stats();
    std::cout << ",\"result\":\"" << result.hex() << "\",\"work\":["
              << stats.nodes << ',' << stats.edges << ',' << stats.summary_links
              << ',' << stats.formulas << ',' << stats.formula_terms << ','
              << stats.formula_states << ',' << stats.formula_edges << ','
              << stats.formula_reuses << ',' << stats.source_families
              << "],\"stage_runs\":0}\n";
    return 0;
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
        if (original_action.starts_with("rec-"))
            return recursive_probe(original_action, directory, *view, *binding,
                                   argc, argv);
        const bool witness = std::string_view(argv[4]) == "witness";
        NodeId root;
        for (const auto& item : view->roots())
            if (item.name == "aa")
                root = item.node;
        if (original_action.starts_with("topo-"))
            return topology_probe(original_action, directory, *view, *binding,
                                  root, argc, argv);
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
