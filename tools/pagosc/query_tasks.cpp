#include "query_tasks.h"
#include "pagos/stage/provenance_topology.h"
#include "query_task_support.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace pagos::driver {
namespace {
using namespace stage;
using namespace stage::provenance;
using namespace query_task_cli;
template <class T> std::expected<T, std::string> number(std::string_view text) {
    T result{};
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size())
        return std::unexpected("invalid query numeric value: " +
                               std::string(text));
    return result;
}
std::expected<cache::Digest, std::string> task_id(std::string_view text) {
    if (text.size() != 64)
        return std::unexpected("query ID must contain 64 hexadecimal digits");
    cache::Digest id;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
        unsigned value{};
        const auto part = text.substr(i * 2, 2);
        const auto parsed =
            std::from_chars(part.data(), part.data() + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + 2)
            return std::unexpected(
                "query ID must contain 64 hexadecimal digits");
        id.bytes[i] = static_cast<std::uint8_t>(value);
    }
    return id;
}
void quote(std::ostream& output, std::string_view text) {
    constexpr std::string_view hex = "0123456789abcdef";
    output << '"';
    for (const char value : text) {
        const auto byte = static_cast<unsigned char>(value);
        if (value == '"' || value == '\\')
            output << '\\' << value;
        else if (byte < 32)
            output << "\\u00" << hex[byte >> 4U] << hex[byte & 15U];
        else
            output << value;
    }
    output << '"';
}
std::string_view progress_name(QueryProgress progress) {
    switch (progress) {
    case QueryProgress::Running:
        return "running";
    case QueryProgress::Paused:
        return "paused";
    case QueryProgress::Cancelled:
        return "cancelled";
    case QueryProgress::Completed:
        return "completed";
    case QueryProgress::Failed:
        return "failed";
    }
    throw std::runtime_error("invalid query progress");
}
void location(std::ostream& output, const EvidenceView& view, NodeId id) {
    const auto lease = view.pin(id);
    if (!lease || !lease->site())
        throw std::runtime_error("query source location unavailable");
    output << "{\"node\":" << id.value << ",\"label\":";
    quote(output, lease->site()->label);
    output << ",\"begin\":" << lease->site()->span.begin
           << ",\"end\":" << lease->site()->span.end << '}';
}
void envelope(std::ostream& output, const cache::Digest& id,
              std::uint64_t sequence, QueryProgress progress,
              std::string_view mode) {
    output << "{\"schema\":1,\"type\":\"page\",\"task\":\"" << id.hex()
           << "\",\"sequence\":" << sequence << ",\"mode\":";
    quote(output, mode);
    output << ",\"progress\":";
    quote(output, progress_name(progress));
}
void coverage(std::ostream& output, const EvidenceCoverage& value) {
    output << "{\"precision\":";
    quote(output, value.precision == EvidencePrecision::ExactForRecordedModel
                      ? "exact-for-recorded-model"
                  : value.precision == EvidencePrecision::Conservative
                      ? "conservative"
                      : "unresolved");
    output << ",\"issues\":" << value.issues
           << ",\"all_sources\":" << (value.all_sources ? "true" : "false")
           << ",\"witness_found\":" << (value.witness_found ? "true" : "false")
           << '}';
}
std::string detail_json(const QueryTask& task) {
    std::ostringstream output;
    output << ",\"coverage\":";
    coverage(output, task.coverage());
    return output.str();
}
std::string detail_json(const TopologyTask&) {
    return ",\"structural\":true,\"sources_solved\":false";
}
std::string render(const QueryPage& page, const QueryTask& task,
                   const EvidenceView& view, const cache::Digest& id,
                   std::string_view mode, QueryKind channel) {
    std::ostringstream output;
    envelope(output, id, page.sequence, page.progress, mode);
    output << ",\"channel\":";
    quote(output, channel == QueryKind::Value ? "value" : "effects");
    output << ",\"coverage\":";
    coverage(output, page.coverage);
    output << ",\"records\":[";
    bool first = true;
    const auto add = [&](std::string_view kind, NodeId node,
                         std::uint32_t context) {
        if (!first)
            output << ',';
        first = false;
        output << "{\"kind\":";
        quote(output, kind);
        output << ",\"context\":" << context << ",\"site\":";
        location(output, view, node);
        output << ",\"calls\":[";
        bool first_call = true;
        while (context) {
            const auto invocation = task.invocation(context);
            if (invocation.parent >= context)
                throw std::runtime_error("invalid query context ancestry");
            if (!first_call)
                output << ',';
            first_call = false;
            location(output, view, invocation.call);
            context = invocation.parent;
        }
        output << "]}";
    };
    for (const auto& source : page.sources)
        add("source", source.node, source.context);
    for (const auto& state : page.witness)
        add("witness", state.node, state.context);
    output << "]}\n";
    return output.str();
}
std::string render(const TopologyPage& page, const TopologyTask&,
                   const EvidenceView& view, const cache::Digest& id,
                   std::string_view mode, QueryKind channel) {
    std::ostringstream output;
    envelope(output, id, page.sequence, page.progress, mode);
    (void)channel;
    output << ",\"channel\":\"both\",\"structural\":true,\"records\":[";
    bool first = true;
    for (const auto& item : page.records) {
        if (!first)
            output << ',';
        first = false;
        output << "{\"kind\":";
        quote(output, item.kind == TopologyRecordKind::Node   ? "node"
                      : item.kind == TopologyRecordKind::Edge ? "edge"
                                                              : "summary-link");
        output << ",\"from\":";
        location(output, view, item.node);
        if (item.kind != TopologyRecordKind::Node) {
            output << ",\"to\":";
            location(output, view, item.edge.target);
            output << ",\"relation\":";
            quote(output,
                  item.edge.relation == Relation::Data       ? "data"
                  : item.edge.relation == Relation::Control  ? "control"
                  : item.edge.relation == Relation::Contract ? "contract"
                  : item.edge.relation == Relation::Effect   ? "effect"
                                                             : "initializer");
            output << ",\"port\":";
            quote(output, item.port == QueryKind::Value ? "value" : "effects");
        }
        output << '}';
    }
    output << "]}\n";
    return output.str();
}
std::string work_json(const QueryTask& task) {
    const auto& s = task.stats();
    std::ostringstream output;
    output << "{\"admitted_states\":" << s.admitted_states
           << ",\"visited_states\":" << s.visited_states
           << ",\"scanned_edges\":" << s.scanned_edges
           << ",\"visited_edges\":" << s.visited_edges
           << ",\"bindings\":" << s.bindings
           << ",\"witness_steps\":" << s.witness_steps
           << ",\"queue_peak\":" << s.queue_peak << ",\"slices\":" << s.slices
           << '}';
    return output.str();
}
std::string work_json(const TopologyTask& task) {
    const auto& s = task.stats();
    std::ostringstream output;
    output << "{\"nodes\":" << s.nodes << ",\"edges\":" << s.edges
           << ",\"summary_links\":" << s.summary_links
           << ",\"formulas\":" << s.formulas
           << ",\"formula_terms\":" << s.formula_terms
           << ",\"formula_states\":" << s.formula_states
           << ",\"formula_edges\":" << s.formula_edges
           << ",\"formula_reuses\":" << s.formula_reuses
           << ",\"source_families\":" << s.source_families << '}';
    return output.str();
}
template <class Task>
cache::TaskHead persist(Task& task, const cache::TaskStore& store,
                        std::uint64_t generation,
                        const QueryTaskOptions& options, bool offload) {
    const cache::Limits limits{.elements = options.record_limit};
    if (offload)
        return checked(task.offload(store, generation,
                                    options.paging.segment_bytes, limits));
    if constexpr (std::is_same_v<Task, QueryTask>)
        return checked(
            task.save(store, generation, options.paging.segment_bytes, limits));
    else
        return checked(task.save_segmented(
            store, generation, options.paging.segment_bytes, limits));
}
template <class Task>
int execute(Task task, cache::TaskStore store,
            std::optional<cache::TaskHead> head,
            const FunctionEvidenceView& evidence, std::string_view root,
            std::string_view mode, std::size_t slice_size,
            const QueryTaskOptions& options,
            const cache::TaskStoreOptions& bounds, bool stage_ran,
            bool statistics) {
    if (!head) {
        head = persist(task, store, 0, options, false);
        std::cerr << "query-task: id=" << store.task().hex()
                  << " saved initial state\n";
    } else {
        std::cerr << "query-task: loaded id=" << store.task().hex()
                  << " generation=" << head->generation << '\n';
    }
    if (options.action == "cancel" &&
        task.progress() == QueryProgress::Completed)
        throw std::runtime_error("query traversal is complete; acknowledge its "
                                 "page or explicitly discard inactive tasks");
    if (options.acknowledge && !task.acknowledge(*options.acknowledge))
        throw std::runtime_error(
            "acknowledgement does not match a stored query page");
    const auto stored_work = work_json(task);
    const auto stored_detail = detail_json(task);
    std::optional<decltype(task.advance())> page;
    if (options.read_only) {
        page = task.advance(
            {0, 0, 0, 0}); // Only a stored unconfirmed page can be delivered.
    } else {
        if (options.action != "start")
            store = checked(
                cache::TaskStore::open(options.directory, store.task(),
                                       cache::TaskAccess::ReadWrite, bounds));
        if constexpr (std::is_same_v<Task, QueryTask>) {
            if (options.storage == "paged") {
                const auto rebound = task.resume_with_storage(store);
                if (!rebound)
                    throw std::runtime_error(error_text(rebound.error()));
            }
        }
        if (options.action == "cancel")
            task.cancel();
        else {
            task.resume();
            for (std::size_t used = 0;
                 !options.max_slices || used < *options.max_slices; ++used) {
                const auto current = checked(store.head());
                if (!current || current->generation != head->generation)
                    throw std::runtime_error(
                        current &&
                                current->status == cache::TaskStatus::Cancelled
                            ? "query was cancelled by another writer"
                            : "query head changed; resume the saved task "
                              "again");
                page = task.advance(
                    {slice_size, slice_size, slice_size, slice_size});
                if (task.storage_failure())
                    throw std::runtime_error(
                        error_text(*task.storage_failure()));
                if (page->progress == QueryProgress::Failed)
                    throw std::runtime_error(
                        "query evidence or allocation unavailable");
                if (page->sequence ||
                    page->progress == QueryProgress::Completed)
                    break;
            }
        }
    }
    if (task.storage_failure())
        throw std::runtime_error(error_text(*task.storage_failure()));
    if (page && page->progress == QueryProgress::Failed)
        throw std::runtime_error("query evidence unavailable");
    // Build the presentation while value-access contexts are resident. Publish
    // unconfirmed records before writing stdout; stdout never acknowledges
    // them.
    const auto text = page && page->sequence
                          ? render(*page, task, *evidence.view(), store.task(),
                                   mode, options.channel)
                          : "";
    const auto work = options.read_only ? stored_work : work_json(task);
    const auto detail = options.read_only ? stored_detail : detail_json(task);
    if (!options.read_only)
        head = persist(task, store, head->generation, options, true);
    std::ostringstream status;
    status << "{\"schema\":1,\"type\":\"status\",\"task\":\""
           << store.task().hex() << "\",\"generation\":" << head->generation
           << ",\"state\":";
    quote(status, status_name(head->status));
    status << ",\"root\":";
    quote(status, root);
    status << ",\"mode\":";
    quote(status, mode);
    status << ",\"channel\":";
    quote(status, mode == "topology"                    ? "both"
                  : options.channel == QueryKind::Value ? "value"
                                                        : "effects");
    status << ",\"storage\":";
    quote(status, options.storage);
    status << ",\"read_only\":" << (options.read_only ? "true" : "false")
           << ",\"offloaded\":"
           << (task.state_memory().offloaded ? "true" : "false")
           << ",\"acknowledge\":";
    if (page && page->sequence)
        status << page->sequence;
    else
        status << "null";
    status << ",\"stage_runs\":" << (stage_ran ? 1 : 0) << detail
           << ",\"work\":" << work << "}\n";
    std::cout << text << status.str();
    std::cout.flush();
    if (!std::cout)
        throw std::runtime_error(
            "query output failed; the saved page can be replayed");
    std::cerr << "query-task: id=" << store.task().hex()
              << " state=" << status_name(head->status)
              << " generation=" << head->generation << '\n';
    if (page && page->sequence)
        std::cerr << "query-task: confirm page " << page->sequence
                  << " with --query-ack only after committing its records\n";
    if (statistics) {
        const auto read = evidence.view()->read_stats();
        std::cerr << "function-evidence metadata-objects="
                  << evidence.metadata_objects_read()
                  << " metadata-bytes=" << evidence.metadata_bytes_read()
                  << " cold-objects=" << read.objects_read
                  << " cold-bytes=" << read.payload_bytes_read
                  << " cached-bytes=" << read.cached_bytes
                  << " live-bytes=" << read.live_bytes << '\n';
    }
    return head->status == cache::TaskStatus::Completed ||
                   head->status == cache::TaskStatus::Cancelled
               ? 0
               : 3;
}
} // namespace
std::expected<void, std::string>
parse_query_task_option(std::string_view argument, QueryTaskOptions& options) {
    options.specified = true;
    if (argument == "--query-read-only") {
        options.read_only = true;
        return {};
    }
    const auto split = argument.find('=');
    if (split == std::string_view::npos)
        return std::unexpected("query option requires a value: " +
                               std::string(argument));
    const auto name = argument.substr(0, split),
               value = argument.substr(split + 1);
    if (value.empty())
        return std::unexpected("query option requires a non-empty value: " +
                               std::string(name));
    if (name == "--query-dir")
        options.directory = value;
    else if (name == "--query-action") {
        if (value != "start" && value != "resume" && value != "cancel")
            return std::unexpected("invalid query action");
        options.action = value;
    } else if (name == "--query-id") {
        const auto parsed = task_id(value);
        if (!parsed)
            return std::unexpected(parsed.error());
        options.id = *parsed;
    } else if (name == "--query-storage") {
        if (value != "memory" && value != "paged")
            return std::unexpected("invalid query storage mode");
        options.storage = value;
    } else if (name == "--query-channel") {
        if (value != "value" && value != "effects")
            return std::unexpected("invalid query channel");
        options.channel =
            value == "value" ? QueryKind::Value : QueryKind::Effects;
        options.channel_set = true;
    } else if (name == "--query-ack") {
        const auto parsed = number<std::uint64_t>(value);
        if (!parsed)
            return std::unexpected(parsed.error());
        if (!*parsed)
            return std::unexpected("query acknowledgement must be positive");
        options.acknowledge = *parsed;
    } else if (name == "--query-max-slices") {
        options.slices_set = true;
        if (value == "all")
            options.max_slices.reset();
        else {
            const auto parsed = number<std::size_t>(value);
            if (!parsed)
                return std::unexpected(parsed.error());
            options.max_slices = *parsed;
        }
    } else if (name == "--query-disk-bytes" ||
               name == "--query-segment-bytes" ||
               name == "--query-resident-bytes" ||
               name == "--query-cache-bytes" || name == "--query-max-records") {
        const auto parsed = number<std::size_t>(value);
        if (!parsed)
            return std::unexpected(parsed.error());
        if (name == "--query-max-records") {
            if (!*parsed || *parsed > UINT32_MAX)
                return std::unexpected(
                    "query record limit must be between 1 and UINT32_MAX");
            options.record_limit = *parsed;
        } else if (name == "--query-disk-bytes")
            options.disk_bytes = *parsed;
        else if (name == "--query-segment-bytes")
            options.paging.segment_bytes = *parsed;
        else {
            options.paging_set = true;
            if (name == "--query-resident-bytes")
                options.paging.resident_bytes = *parsed;
            else
                options.paging.cache_bytes = *parsed;
        }
    } else
        return std::unexpected("unknown query option: " + std::string(name));
    return {};
}
std::expected<void, std::string>
validate_query_task_options(const QueryTaskOptions& options, bool evidence_read,
                            bool evidence_write, std::string_view root,
                            std::string_view mode) {
    if (!options.enabled())
        return std::unexpected("query options require --query-dir=DIR");
    if (!evidence_read && !evidence_write)
        return std::unexpected(
            "query tasks require function evidence read or write");
    if (root.empty() && mode != "recursive-summary")
        return std::unexpected("query tasks require one --evidence-root=NAME");
    if (mode == "decisions")
        return std::unexpected("decision listings do not use query tasks");
    if (options.action != "start" && (!evidence_read || !options.id))
        return std::unexpected(
            "query resume/cancel requires --evidence-read and --query-id");
    if (options.acknowledge &&
        (options.action != "resume" || options.read_only))
        return std::unexpected(
            "query acknowledgement requires writable resume");
    if (options.read_only && (options.action != "resume" || options.slices_set))
        return std::unexpected("read-only query access only replays/inspects a "
                               "saved task; omit the slice limit");
    if (mode == "topology" &&
        (options.storage == "paged" || options.channel_set))
        return std::unexpected(
            "topology uses resident preparation and both channels");
    if (mode == "recursive-summary" &&
        (options.storage != "memory" || options.channel_set ||
         options.paging_set))
        return std::unexpected(
            "recursive tasks use resident execution; select one "
            "--evidence-channel and omit query channel/buffer options");
    if (options.paging_set && options.storage != "paged")
        return std::unexpected(
            "query buffer options require --query-storage=paged");
    if (options.paging.segment_bytes <
            (mode == "recursive-summary" ? 76 : 72) ||
        options.paging.segment_bytes > cache::Limits{}.bytes)
        return std::unexpected(
            "query segment size is below its record width or above the codec "
            "byte limit");
    return {};
}
int run_query_task(const FunctionEvidenceView& evidence,
                   std::string_view source_text, std::string_view root,
                   std::string_view mode, std::size_t slice,
                   const QueryTaskOptions& options, bool stage_ran,
                   bool statistics) {
    try {
        const ProvenanceRoot* selected = nullptr;
        for (const auto& candidate : evidence.roots())
            if (candidate.name == root) {
                if (selected)
                    throw std::runtime_error(
                        "query root is ambiguous; select a unique binding");
                selected = &candidate;
            }
        if (!selected)
            throw std::runtime_error("query root not found: " +
                                     std::string(root));
        const auto binding = evidence.checkpoint_binding(source_text);
        if (!binding)
            throw std::runtime_error("query binding unavailable");
        const auto id = options.id ? *options.id : fresh_id();
        cache::TaskStoreOptions bounds;
        bounds.disk_bytes = options.disk_bytes;
        bounds.store.codec.elements = options.record_limit;
        const cache::Limits limits{.elements = options.record_limit};
        auto store = checked(cache::TaskStore::open(
            options.directory, id,
            options.action == "start" ? cache::TaskAccess::ReadWrite
                                      : cache::TaskAccess::ReadOnly,
            bounds));
        auto head = checked(store.head());
        if (options.action == "start" && head)
            throw std::runtime_error(
                "query ID already exists; resume it or choose a new ID");
        if (options.action != "start" && !head)
            throw std::runtime_error("query task has no saved head");
        if (mode == "topology") {
            auto task =
                head ? checked(TopologyTask::restore(*binding, store, *head,
                                                     selected->node, limits))
                     : TopologyTask(*binding, id, selected->node);
            return execute(std::move(task), std::move(store), head, evidence,
                           root, mode, slice, options, bounds, stage_ran,
                           statistics);
        }
        const bool witness = mode == "witness";
        auto task =
            head ? (options.storage == "paged"
                        ? checked(QueryTask::restore_paged(
                              *binding, store, *head, selected->node,
                              options.channel, witness, options.paging, limits))
                        : checked(QueryTask::restore(
                              *binding, store, *head, selected->node,
                              options.channel, witness, limits)))
            : options.storage == "paged"
                ? checked(QueryTask::begin_paged(
                      *binding, store, selected->node, options.channel, witness,
                      options.paging, limits))
                : QueryTask(*binding, id, selected->node, options.channel,
                            witness);
        if (task.state_memory().paged != (options.storage == "paged"))
            throw std::runtime_error("query storage mode changed; use the mode "
                                     "used to start this task");
        return execute(std::move(task), std::move(store), head, evidence, root,
                       mode, slice, options, bounds, stage_ran, statistics);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
int reclaim_query_tasks(int argc, char** argv) {
    if ((argc != 3 && argc != 4) ||
        std::string_view(argv[2]).starts_with('-') ||
        (argc == 4 && std::string_view(argv[3]) != "--discard-resumable")) {
        std::cerr << "usage: pagosc reclaim-queries <query-parent> "
                     "[--discard-resumable]\n";
        return 2;
    }
    const auto result = cache::reclaim_tasks(
        argv[2], argc == 4 ? cache::TaskCleanupPolicy::AllInactive
                           : cache::TaskCleanupPolicy::TerminalOnly);
    if (!result) {
        std::cerr << "error: " << error_text(result.error()) << '\n';
        return 1;
    }
    std::cout << "query-cleanup: examined=" << result->examined
              << ", reclaimed=" << result->reclaimed
              << ", active=" << result->active
              << ", skipped=" << result->skipped
              << ", errors=" << result->errors << '\n';
    return result->errors ? 1 : 0;
}
} // namespace pagos::driver
