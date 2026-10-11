#include "pagos/hir/hir.h"
#include "pagos/stage/provenance_prototype.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
// This single-threaded lab counts C++ allocations, not LLVM's direct malloc
// calls. Snapshots below isolate construction and query phases from startup.
struct AllocationCounters {
    std::uint64_t calls{};
    std::uint64_t bytes{};
};
AllocationCounters allocation_counters;
} // namespace

void* operator new(std::size_t bytes) {
    if (auto* memory = std::malloc(bytes == 0 ? 1 : bytes)) {
        ++allocation_counters.calls;
        allocation_counters.bytes += bytes;
        return memory;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {
using namespace pagos::stage::prototype;
using Clock = std::chrono::steady_clock;

struct Observation {
    std::size_t sources{};
    std::size_t states{};
    std::size_t edges{};
    std::size_t contexts{};
    std::size_t witness_steps{};
    std::uint64_t checksum{};
    bool complete{};
};

class Graph {
  public:
    explicit Graph(Storage storage) : store_(storage) {}
    NodeId add(Rule rule, SiteId site, std::span<const Edge> edges = {},
               std::uint32_t payload = 0, SummaryId owner = {},
               Sharing sharing = Sharing::Unique) {
        return store_.add(rule, site, edges, payload, owner, sharing);
    }
    SummaryId reserve_summary() { return store_.reserve_summary(); }
    void define_summary(SummaryId summary, NodeId root) {
        store_.define_summary(summary, root);
    }
    NodeId parameter(SummaryId owner, std::uint32_t slot, SiteId site) {
        return store_.parameter(owner, slot, site);
    }
    NodeId call(SummaryId summary, std::span<const NodeId> arguments,
                SiteId site) {
        return store_.call(summary, arguments, site);
    }
    NodeId summary_ref(SummaryId summary, SiteId site) {
        return store_.summary_ref(summary, site);
    }
    NodeId project(NodeId aggregate, std::uint32_t member, SiteId site) {
        return store_.project(aggregate, member, site);
    }
    Observation observe(NodeId root, bool witness) const {
        const auto result =
            store_.query(root, QueryKind::Value, witness, 10'000'000);
        std::uint64_t checksum = 0;
        for (const auto& source : result.sources)
            checksum += source.site;
        return {.sources = result.sources.size(),
                .states = result.visited_states,
                .edges = result.visited_edges,
                .contexts = result.contexts,
                .witness_steps = result.witness.size(),
                .checksum = checksum,
                .complete = result.complete};
    }
    StoreStats stats() const { return store_.stats(); }

  private:
    Store store_;
};

// Representation baseline, not another StageAnalyzer. This uses the real
// RuntimeTrace and mirrors first-source selection, path copying and symbolic
// substitution. It deliberately cannot supply a complete multi-source answer.
class Paths {
  public:
    NodeId add(Rule rule, SiteId site, std::span<const Edge> edges = {},
               std::uint32_t payload = 0, SummaryId owner = {},
               Sharing = Sharing::Unique) {
        std::optional<pagos::hir::RuntimeTrace> trace;
        const auto name = label(site);
        if (rule == Rule::Origin || rule == Rule::RuntimeBoundary ||
            rule == Rule::SummaryRef) {
            trace = pagos::hir::RuntimeTrace{.origin_span = {site, site},
                                             .origin_name = name,
                                             .path = {name}};
        } else if (rule == Rule::Parameter) {
            trace = pagos::hir::RuntimeTrace{
                .origin_span = {site, site},
                .origin_name = name,
                .path = {name},
                .parameter_id =
                    (static_cast<std::size_t>(owner.value) << 32U) | payload};
        } else {
            for (const auto& edge : edges) {
                if (edge.relation == Relation::Initializer ||
                    edge.relation == Relation::Effect)
                    continue;
                if (traces_.at(edge.target.value)) {
                    trace = traces_[edge.target.value];
                    trace->path.push_back(name);
                    break;
                }
            }
        }
        const auto id = NodeId{static_cast<std::uint32_t>(traces_.size())};
        traces_.push_back(std::move(trace));
        if (rule == Rule::Aggregate) {
            std::vector<NodeId> members;
            for (const auto& edge : edges)
                members.push_back(edge.target);
            members_.emplace(id.value, std::move(members));
        }
        return id;
    }
    SummaryId reserve_summary() {
        const auto id =
            SummaryId{static_cast<std::uint32_t>(summaries_.size())};
        summaries_.push_back({});
        return id;
    }
    void define_summary(SummaryId summary, NodeId root) {
        summaries_.at(summary.value) = root;
    }
    NodeId parameter(SummaryId owner, std::uint32_t slot, SiteId site) {
        return add(Rule::Parameter, site, {}, slot, owner);
    }
    NodeId call(SummaryId summary, std::span<const NodeId> arguments,
                SiteId site) {
        auto trace = traces_.at(summaries_.at(summary.value).value);
        if (trace && trace->parameter_id) {
            const auto slot = *trace->parameter_id & 0xffffffffULL;
            auto actual = traces_.at(arguments[slot].value);
            if (actual) {
                actual->path.insert(actual->path.end(), trace->path.begin(),
                                    trace->path.end());
                trace = std::move(actual);
            } else {
                trace.reset();
            }
        }
        if (trace)
            trace->path.push_back(label(site));
        const auto id = NodeId{static_cast<std::uint32_t>(traces_.size())};
        traces_.push_back(std::move(trace));
        return id;
    }
    NodeId summary_ref(SummaryId summary, SiteId site) {
        return add(Rule::SummaryRef, site, {}, summary.value);
    }
    NodeId project(NodeId aggregate, std::uint32_t member, SiteId site) {
        const auto found = members_.find(aggregate.value);
        const auto selected =
            found == members_.end() ? aggregate : found->second.at(member);
        return add(Rule::Projection, site, std::array{Edge{selected}});
    }
    Observation observe(NodeId root, bool witness) const {
        const auto& trace = traces_.at(root.value);
        const bool source = trace && !trace->parameter_id;
        return {.sources = source ? 1U : 0U,
                .states = 1,
                .witness_steps = witness && trace ? trace->path.size() : 0,
                .checksum = source ? trace->origin_span.begin : 0,
                .complete = false};
    }
    StoreStats stats() const {
        auto bytes = traces_.capacity() * sizeof(traces_[0]);
        for (const auto& trace : traces_)
            if (trace)
                bytes += trace->path.capacity() * sizeof(std::string);
        for (const auto& [unused, members] : members_) {
            (void)unused;
            bytes += members.capacity() * sizeof(NodeId);
        }
        return {.nodes = traces_.size() - 1,
                .summaries = summaries_.size() - 1,
                .node_capacity_bytes = bytes,
                .summary_capacity_bytes =
                    summaries_.capacity() * sizeof(NodeId)};
    }

  private:
    static std::string label(SiteId site) {
        return "site_" + std::to_string(site);
    }
    std::vector<std::optional<pagos::hir::RuntimeTrace>> traces_{std::nullopt};
    std::vector<NodeId> summaries_{NodeId{}};
    std::unordered_map<std::uint32_t, std::vector<NodeId>> members_;
};

struct Workload {
    NodeId root;
    std::vector<NodeId> bindings;
    std::size_t expected_sources{};
};

template <typename Model>
Workload build(Model& model, std::string_view name, std::uint32_t size) {
    Workload workload;
    workload.bindings.reserve(size);
    SiteId site = 1;
    if (name == "chain" || name == "unique") {
        auto current = model.add(Rule::Origin, site++);
        const auto fixed = name == "unique"
                               ? model.add(Rule::Literal, site++, {}, 4)
                               : NodeId{};
        for (std::uint32_t index = 0; index < size; ++index) {
            if (name == "unique")
                current = model.add(Rule::Strict, site++,
                                    std::array{Edge{current}, Edge{fixed}}, 0,
                                    {}, Sharing::Eligible);
            else
                current =
                    model.add(Rule::Use, site++, std::array{Edge{current}});
            workload.bindings.push_back(current);
        }
        workload.root = current;
        workload.expected_sources = 1;
        return workload;
    }
    std::vector<NodeId> outputs;
    outputs.reserve(size);
    if (name == "fan-in") {
        for (std::uint32_t index = 0; index < size; ++index)
            outputs.push_back(model.add(Rule::Origin, site++));
        workload.bindings = outputs;
        while (outputs.size() > 1) {
            std::vector<NodeId> next;
            next.reserve((outputs.size() + 1) / 2);
            for (std::size_t index = 0; index < outputs.size(); index += 2) {
                if (index + 1 == outputs.size()) {
                    next.push_back(outputs[index]);
                } else {
                    next.push_back(
                        model.add(Rule::Strict, site++,
                                  std::array{Edge{outputs[index]},
                                             Edge{outputs[index + 1]}}));
                }
            }
            outputs = std::move(next);
        }
        workload.root = outputs.front();
        workload.expected_sources = size;
        return workload;
    }
    if (name == "repeated") {
        const auto source = model.add(Rule::Origin, site++);
        const auto fixed = model.add(Rule::Literal, site++, {}, 4);
        const auto template_site = site++;
        for (std::uint32_t index = 0; index < size; ++index) {
            const auto proof = model.add(Rule::Strict, template_site,
                                         std::array{Edge{source}, Edge{fixed}},
                                         0, {}, Sharing::Eligible);
            outputs.push_back(
                model.add(Rule::Use, site++, std::array{Edge{proof}}));
        }
        workload.expected_sources = 1;
    } else if (name == "calls") {
        const auto summary = model.reserve_summary();
        auto body = model.parameter(summary, 0, site++);
        for (std::uint32_t depth = 0; depth < 64; ++depth)
            body = model.add(Rule::Use, site++, std::array{Edge{body}});
        model.define_summary(summary, body);
        for (std::uint32_t index = 0; index < size; ++index) {
            const auto argument = model.add(Rule::Origin, site++);
            outputs.push_back(
                model.call(summary, std::array{argument}, site++));
        }
        workload.expected_sources = size;
    } else if (name == "aggregate") {
        std::vector<Edge> fields;
        fields.reserve(size);
        for (std::uint32_t index = 0; index < size; ++index)
            fields.push_back(
                {model.add(index % 2 == 0 ? Rule::Literal : Rule::Origin,
                           site++, {}, index)});
        const auto aggregate = model.add(Rule::Aggregate, site++, fields);
        for (std::uint32_t index = 0; index < size; ++index)
            outputs.push_back(model.project(aggregate, index, site++));
        workload.expected_sources = size / 2;
    } else if (name == "recursive") {
        std::vector<SummaryId> summaries;
        summaries.reserve(size);
        for (std::uint32_t index = 0; index < size; ++index)
            summaries.push_back(model.reserve_summary());
        for (std::uint32_t index = 0; index < size; ++index) {
            const auto source = model.add(Rule::Origin, site++);
            const auto reference =
                model.summary_ref(summaries[(index + 1) % size], site++);
            const auto body =
                model.add(Rule::Strict, site++,
                          std::array{Edge{source}, Edge{reference}});
            model.define_summary(summaries[index], body);
        }
        workload.root = model.summary_ref(summaries.front(), site++);
        // Paths cannot resolve a cyclic summary; its standalone reference is
        // a conservative synthetic origin, matching its limited capability.
        workload.expected_sources = size;
        workload.bindings.push_back(workload.root);
        return workload;
    } else {
        throw std::invalid_argument("unknown workload");
    }
    workload.bindings = outputs;
    std::vector<Edge> inputs;
    inputs.reserve(outputs.size());
    for (const auto output : outputs)
        inputs.push_back({output});
    workload.root = model.add(Rule::Aggregate, site++, inputs);
    return workload;
}

struct Options {
    std::string storage{"append"};
    std::string workload{"chain"};
    std::string query{"sources"};
    std::uint32_t size{1024};
    std::uint32_t iterations{1};
};

std::uint32_t number(std::string_view text) {
    std::uint32_t value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() ||
        value == 0 || value > 1'000'000)
        throw std::invalid_argument("expected integer in 1..1000000");
    return value;
}

Options parse(int count, char** arguments) {
    Options options;
    for (int index = 1; index < count; index += 2) {
        if (index + 1 >= count)
            throw std::invalid_argument("expected option/value pairs");
        const std::string_view name(arguments[index]);
        const std::string_view value(arguments[index + 1]);
        if (name == "--storage")
            options.storage = value;
        else if (name == "--case")
            options.workload = value;
        else if (name == "--query")
            options.query = value;
        else if (name == "--size")
            options.size = number(value);
        else if (name == "--iterations")
            options.iterations = number(value);
        else
            throw std::invalid_argument("unknown option");
    }
    if (options.storage != "paths" && options.storage != "append" &&
        options.storage != "intern")
        throw std::invalid_argument("unknown storage");
    if (options.query != "build" && options.query != "witness" &&
        options.query != "sources" && options.query != "batch")
        throw std::invalid_argument("unknown query");
    return options;
}

template <typename Model> void run(Model& model, const Options& options) {
    const auto initial_allocations = allocation_counters;
    const auto begin = Clock::now();
    const auto workload = build(model, options.workload, options.size);
    const auto built = Clock::now();
    const auto built_allocations = allocation_counters;
    Observation last;
    std::uint64_t checksum = 0;
    std::size_t total_states = 0;
    if (options.query != "build") {
        for (std::uint32_t iteration = 0; iteration < options.iterations;
             ++iteration) {
            if (options.query == "batch") {
                for (const auto binding : workload.bindings) {
                    last = model.observe(binding, true);
                    checksum += last.checksum;
                    total_states += last.states;
                }
            } else {
                last = model.observe(workload.root, options.query == "witness");
                checksum += last.checksum;
                total_states += last.states;
            }
        }
    }
    const auto queried = Clock::now();
    const auto queried_allocations = allocation_counters;
    if (options.storage != "paths" && options.query == "sources" &&
        (!last.complete || last.sources != workload.expected_sources))
        throw std::runtime_error("incorrect or incomplete source query");
    const auto stats = model.stats();
    const auto build_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(built - begin)
            .count();
    const auto query_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(queried - built)
            .count();
    std::cout << "{\"storage\":\"" << options.storage << "\",\"case\":\""
              << options.workload << "\",\"query\":\"" << options.query
              << "\",\"size\":" << options.size
              << ",\"iterations\":" << options.iterations
              << ",\"build_ns\":" << build_ns << ",\"query_ns\":" << query_ns
              << ",\"build_cpp_allocations\":"
              << built_allocations.calls - initial_allocations.calls
              << ",\"query_cpp_allocations\":"
              << queried_allocations.calls - built_allocations.calls
              << ",\"build_cpp_requested_bytes\":"
              << built_allocations.bytes - initial_allocations.bytes
              << ",\"query_cpp_requested_bytes\":"
              << queried_allocations.bytes - built_allocations.bytes
              << ",\"nodes\":" << stats.nodes << ",\"edges\":" << stats.edges
              << ",\"intern_hits\":" << stats.intern_hits
              << ",\"store_capacity_bytes\":"
              << stats.node_capacity_bytes + stats.edge_capacity_bytes +
                     stats.summary_capacity_bytes + stats.intern_capacity_bytes
              << ",\"intern_capacity_bytes\":" << stats.intern_capacity_bytes
              << ",\"sources\":" << last.sources
              << ",\"expected_sources\":" << workload.expected_sources
              << ",\"sources_complete\":" << (last.complete ? "true" : "false")
              << ",\"visited_states\":" << total_states
              << ",\"contexts\":" << last.contexts
              << ",\"witness_steps\":" << last.witness_steps
              << ",\"checksum\":" << checksum
              << ",\"node_bytes\":" << sizeof(Node)
              << ",\"edge_bytes\":" << sizeof(Edge)
              << ",\"trace_bytes\":" << sizeof(pagos::hir::RuntimeTrace)
              << "}\n";
}

} // namespace

int main(int count, char** arguments) {
    try {
        const auto options = parse(count, arguments);
        if (options.storage == "paths") {
            Paths model;
            run(model, options);
        } else {
            Graph model(options.storage == "intern" ? Storage::SelectiveIntern
                                                    : Storage::Append);
            run(model, options);
        }
    } catch (const std::exception& error) {
        std::cerr << "provenance benchmark: " << error.what() << '\n';
        return 1;
    }
}
