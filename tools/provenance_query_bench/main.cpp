#include "pagos/stage/provenance_topology.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace pagos::stage::provenance;
using Clock = std::chrono::steady_clock;
auto ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}
void run(std::string_view mode, std::string_view workload, unsigned size) {
    const auto start = Clock::now();
    Builder builder;
    auto root = builder.add(Rule::Origin, 1);
    std::vector<NodeId> path;
    std::size_t expected = 1;
    if (workload == "chain") {
        for (unsigned i = 0; i < size; ++i)
            root = builder.add(Rule::Use, 2, std::array{Edge{root}});
    } else if (workload == "fan-in") {
        std::vector<Edge> edges;
        edges.reserve(size);
        for (unsigned i = 0; i < size; ++i)
            edges.push_back({builder.add(Rule::Origin, i + 1)});
        root = builder.add(Rule::Strict, 2, edges);
        expected = size;
    } else if (workload == "diamond") {
        if (size > 40)
            throw std::invalid_argument("diamond depth exceeds 40");
        for (unsigned i = 0; i < size; ++i) {
            const auto a = builder.add(Rule::Use, 2, std::array{Edge{root}});
            const auto b = builder.add(Rule::Use, 3, std::array{Edge{root}});
            root = builder.add(Rule::Strict, 4, std::array{Edge{a}, Edge{b}});
        }
    } else if (workload == "calls") {
        if (size > 40 ||
            (size > 16 && mode != "selected" && mode != "topology"))
            throw std::invalid_argument(
                "large call DAG requires topology or selected mode");
        auto summary = builder.reserve_summary();
        builder.define_summary(summary, root);
        for (unsigned i = 0; i < size; ++i) {
            const auto a = builder.call(summary, {}, 2);
            const auto b = builder.call(summary, {}, 3);
            path.push_back(a);
            const auto next = builder.reserve_summary();
            builder.define_summary(
                next,
                builder.add(Rule::Strict, 4, std::array{Edge{a}, Edge{b}}));
            summary = next;
        }
        root = builder.call(summary, {}, 5);
        path.push_back(root);
        std::ranges::reverse(path);
        expected =
            mode == "selected" ? 1 : static_cast<std::size_t>(1ULL << size);
    } else
        throw std::invalid_argument("unknown workload");
    const auto graph = std::move(builder).freeze();
    const auto built = Clock::now();
    std::shared_ptr<const FormulaIndex> index;
    std::size_t pages = 0, records = 0;
    if (mode == "compressed" || mode == "selected" || mode == "topology") {
        auto topology = graph.begin_topology(root);
        while (topology.progress() != QueryProgress::Completed) {
            const auto page = topology.advance();
            if (page.progress == QueryProgress::Failed)
                throw std::runtime_error("topology failed");
            ++pages;
            records += page.records.size();
        }
        index = topology.index();
    }
    const auto prepared = Clock::now();
    QueryResult result;
    QueryStats stats;
    if (mode == "legacy") {
        result = graph.query(root);
        stats.visited_states = result.visited_states;
        stats.visited_edges = result.visited_edges;
    } else if (mode != "topology") {
        auto task = mode == "selected"     ? index->begin_invocation(path)
                    : mode == "compressed" ? index->begin_sources(root)
                                           : graph.begin_query(root);
        while (task.progress() != QueryProgress::Completed) {
            const auto page = task.advance();
            if (page.progress == QueryProgress::Failed)
                throw std::runtime_error("query failed");
        }
        result = task.result();
        stats = task.stats();
    }
    const auto queried = Clock::now();
    if (mode != "topology" &&
        !(mode == "legacy" && workload == "chain" && size >= 1'000'000) &&
        (!result.complete || result.sources.size() != expected))
        throw std::runtime_error("source mismatch");
    if (mode == "selected" &&
        (workload != "calls" || result.invocations.size() != path.size() + 1))
        throw std::runtime_error("selected context mismatch");
    const auto topology = index ? index->stats() : TopologyStats{};
    const auto store = graph.stats();
    std::cout << "{\"mode\":\"" << mode << "\",\"case\":\"" << workload
              << "\",\"size\":" << size << ",\"build_ns\":" << ns(start, built)
              << ",\"prepare_ns\":" << ns(built, prepared)
              << ",\"query_ns\":" << ns(prepared, queried)
              << ",\"nodes\":" << store.nodes << ",\"edges\":" << store.edges
              << ",\"summaries\":" << store.summaries
              << ",\"topology_records\":" << records
              << ",\"topology_pages\":" << pages
              << ",\"formulas\":" << topology.formulas
              << ",\"formula_terms\":" << topology.formula_terms
              << ",\"source_families\":" << topology.source_families
              << ",\"formula_reuses\":" << topology.formula_reuses
              << ",\"formula_capacity_bytes\":"
              << topology.formula_capacity_bytes
              << ",\"prepare_states\":" << topology.formula_states
              << ",\"prepare_edges\":" << topology.formula_edges
              << ",\"visited_states\":" << stats.visited_states
              << ",\"scanned_edges\":" << stats.scanned_edges
              << ",\"visited_edges\":" << stats.visited_edges
              << ",\"bindings\":" << stats.bindings
              << ",\"queue_peak\":" << stats.queue_peak
              << ",\"slices\":" << stats.slices
              << ",\"formula_uses\":" << stats.formula_uses
              << ",\"formula_fallbacks\":" << stats.formula_fallbacks
              << ",\"sources\":" << result.sources.size()
              << ",\"contexts\":" << result.contexts << ",\"all_sources\":"
              << (mode != "topology" && result.complete ? "true" : "false")
              << "}\n";
}
} // namespace
int main(int count, char** arguments) {
    try {
        if (count != 4)
            throw std::invalid_argument(
                "usage: pagos_provenance_query_bench MODE CASE SIZE");
        const std::string_view spelling(arguments[3]);
        unsigned size{};
        const auto parsed = std::from_chars(
            spelling.data(), spelling.data() + spelling.size(), size);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != spelling.data() + spelling.size() || !size ||
            size > 2'000'000)
            throw std::invalid_argument("size must be in 1..2000000");
        const std::string_view mode(arguments[1]);
        if (mode != "legacy" && mode != "resumable" && mode != "compressed" &&
            mode != "selected" && mode != "topology")
            throw std::invalid_argument("unknown mode");
        run(mode, arguments[2], size);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
