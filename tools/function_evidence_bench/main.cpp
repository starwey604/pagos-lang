#include "build_identity.h"
#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>

int main(int count, char** args) {
    using namespace pagos;
    using namespace stage::provenance;
    using Clock = std::chrono::steady_clock;
    if (count != 4)
        return 1;
    const std::string mode(args[1]);
    if (mode != "publish" && mode != "lazy" && mode != "eager")
        return 1;
    auto source = source::SourceFile::load(args[2]);
    if (!source)
        return 1;
    source::DiagnosticEngine diagnostics(*source);
    auto begin = Clock::now();
    syntax::Lexer lexer(*source, diagnostics);
    auto tokens = lexer.tokenize();
    syntax::Parser parser(tokens, diagnostics);
    auto ast = parser.parse_module();
    sema::TypeChecker checker(diagnostics, 32);
    if (!ast || !checker.check(*ast))
        return 1;
    cache::QueryContext context{
        .module = {"function-evidence-benchmark", "module.pgs"},
        .target = {"riscv32-unknown-elf", "generic-rv32", "", "e-p:32:32", 32,
                   true},
        .compiler_build = pagos_cache_build_id,
        .semantic_options = "pagos.staging.v1"};
    auto identity = stage::build_cache_identity(*ast, checker.types(), context);
    if (!identity)
        return 1;
    auto checked = Clock::now();
    const auto ns = [](auto start, auto end) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count();
    };
    const cache::FileStore store(args[3]);
    if (mode == "publish") {
        stage::StageAnalyzer analyzer(diagnostics, checker.types(), {},
                                      context);
        auto module = analyzer.analyze(*ast);
        if (!module || diagnostics.has_error()) {
            diagnostics.render(std::cerr);
            return 1;
        }
        auto built = Clock::now();
        auto published = stage::FunctionEvidence::publish(
            store, *module->provenance, *identity, context);
        if (!published)
            return 1;
        std::cout << "{\"checked_ns\":" << ns(begin, checked)
                  << ",\"stage_ns\":" << ns(checked, built)
                  << ",\"publish_ns\":" << ns(built, Clock::now())
                  << ",\"recorded_nodes\":"
                  << module->provenance->graph().stats().nodes << "}\n";
        return 0;
    }
    auto loaded =
        stage::FunctionEvidence::load(store, std::move(*identity), context);
    identity = std::unexpected(cache::Error::InvalidContext);
    if (!loaded)
        return 1;
    auto opened = Clock::now();
    auto prepared = stage::FunctionEvidenceView::open(
        std::move(*loaded), mode == "eager" ? 0 : 4 * 1024 * 1024);
    loaded = std::unexpected(
        cache::StoreError{cache::StoreErrorCode::NotFound, {}, {}});
    if (!prepared)
        return 1;
    std::optional<stage::FunctionEvidenceView> evidence(std::move(*prepared));
    prepared =
        std::unexpected(EvidenceError{EvidenceErrorCode::MissingBlock, {}, {}});
    auto view = evidence->view();
    const auto root = std::ranges::find(evidence->roots(), std::string("aa"),
                                        &stage::ProvenanceRoot::name);
    if (root == evidence->roots().end())
        return 1;
    const auto root_id = root->node;
    const auto metadata_objects = evidence->metadata_objects_read(),
               metadata_bytes = evidence->metadata_bytes_read();
    const auto virtual_nodes = view->node_count();
    auto ready = Clock::now();
    std::optional<FrozenGraph> graph;
    std::vector<stage::ProvenanceSite> sites;
    EvidenceReadStats eager_stats;
    if (mode == "eager") {
        Builder builder;
        for (const auto& summary : view->summaries()) {
            (void)summary;
            (void)builder.reserve_summary();
        }
        sites.resize(view->node_count() + 1);
        std::optional<NodeLease> previous;
        for (std::uint32_t id = 1; id <= view->node_count(); ++id) {
            auto lease = view->pin({id});
            if (!lease || !lease->site())
                return 1;
            sites[id] = *lease->site();
            const auto& node = lease->node();
            (void)builder.add(node.rule, node.site, lease->edges(),
                              node.payload, node.owner);
            previous =
                std::move(*lease); // Reuse pinned SCC with a zero-size cache.
        }
        for (const auto& summary : view->summaries()) {
            if (summary.value != summary.effects)
                return 1;
            builder.define_summary(summary.id, summary.value);
        }
        graph = std::move(builder).freeze();
        previous.reset();
        eager_stats = view->read_stats();
        evidence.reset();
        view.reset();
    }
    auto restored = Clock::now();
    auto run_query = [&]() {
        auto task =
            graph ? graph->begin_query(root_id) : QueryTask(view, root_id);
        while (task.progress() != QueryProgress::Completed &&
               task.progress() != QueryProgress::Failed)
            (void)task.advance(QuerySlice::unlimited());
        if (task.progress() == QueryProgress::Failed)
            throw std::runtime_error("query failed");
        const auto result = task.result();
        std::vector<std::string> lines;
        for (auto item : result.sources) {
            auto site =
                graph ? sites[item.site] : *view->pin(item.node)->site();
            std::ostringstream line;
            line << site.label << ':' << site.span.begin << ':'
                 << site.span.end;
            for (auto frame = item.context; frame;
                 frame = result.invocations[frame].parent) {
                auto call = result.invocations[frame].call;
                auto location = graph ? sites[graph->node(call).site]
                                      : *view->pin(call)->site();
                line << '/' << location.label << ':' << location.span.begin
                     << ':' << location.span.end;
            }
            lines.push_back(line.str());
        }
        std::ranges::sort(lines);
        std::string signature;
        for (const auto& line : lines)
            signature += line + '\n';
        signature += result.complete ? "complete" : "partial";
        return cache::fingerprint(
                   "pagos.function-query-bench.v1",
                   {reinterpret_cast<const std::uint8_t*>(signature.data()),
                    signature.size()})
            .hex();
    };
    const auto first_signature = run_query();
    auto first = Clock::now();
    const auto first_stats = graph ? eager_stats : view->read_stats();
    const auto second_signature = run_query();
    auto second = Clock::now();
    const auto second_stats = graph ? eager_stats : view->read_stats();
    if (first_signature != second_signature)
        return 1;
    std::cout << "{\"checked_ns\":" << ns(begin, checked)
              << ",\"hot_load_ns\":" << ns(checked, opened)
              << ",\"view_prepare_ns\":" << ns(opened, ready)
              << ",\"eager_restore_ns\":" << ns(ready, restored)
              << ",\"first_query_ns\":" << ns(restored, first)
              << ",\"repeat_query_ns\":" << ns(first, second)
              << ",\"metadata_objects\":" << metadata_objects
              << ",\"metadata_bytes\":" << metadata_bytes
              << ",\"virtual_nodes\":" << virtual_nodes
              << ",\"cold_objects\":" << first_stats.objects_read
              << ",\"cold_bytes\":" << first_stats.payload_bytes_read
              << ",\"repeat_cold_objects\":"
              << second_stats.objects_read - first_stats.objects_read
              << ",\"cached_bytes\":" << second_stats.cached_bytes
              << ",\"live_bytes\":" << second_stats.live_bytes
              << ",\"signature\":\"" << first_signature << "\"}\n";
}
