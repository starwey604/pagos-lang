#include "recursive_summaries.h"
#include "pagos/stage/provenance_recursive.h"
#include "query_task_support.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace pagos::driver {
namespace {
using namespace stage;
using namespace stage::provenance;
void quote(std::ostream& out, std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    out << '"';
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (byte < 32)
            out << "\\u00" << hex[byte >> 4U] << hex[byte & 15U];
        else
            out << c;
    }
    out << '"';
}
std::string_view rule_name(Rule rule) {
    constexpr std::array names{"literal",
                               "origin",
                               "runtime-boundary",
                               "use",
                               "strict",
                               "select",
                               "require-comptime",
                               "aggregate",
                               "projection",
                               "parameter",
                               "call",
                               "summary-ref",
                               "effect",
                               "sequence",
                               "incomplete"};
    const auto index = static_cast<std::size_t>(rule);
    return index < names.size() ? names[index] : "unsupported";
}
std::string_view relation_name(Relation relation) {
    constexpr std::array names{"data", "control", "contract", "effect",
                               "initializer"};
    const auto index = static_cast<std::size_t>(relation);
    return index < names.size() ? names[index] : "unsupported";
}
const FunctionSummaryEntry* entry(const FunctionEvidenceView& evidence,
                                  SummaryId id) {
    const auto entries = evidence.summary_catalog();
    return id.value && id.value <= entries.size() &&
                   entries[id.value - 1].id == id
               ? &entries[id.value - 1]
               : nullptr;
}
void key(std::ostream& out, const FunctionEvidenceView& evidence,
         SummaryId id) {
    if (const auto* found = entry(evidence, id))
        quote(out, found->key.hex());
    else
        out << "null";
}
void describe(std::ostream& out, const FunctionEvidenceView& evidence,
              const FunctionSummaryEntry& summary) {
    const auto& scope = evidence.scopes()[summary.id.value];
    out << "{\"key\":";
    quote(out, summary.key.hex());
    out << ",\"name\":";
    quote(out, scope.name);
    out << ",\"arguments\":[";
    bool first = true;
    for (const auto& argument : scope.specialization->arguments) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"type\":";
        quote(out, sema::type_name(argument.type));
        out << ",\"stage\":";
        quote(out, argument.stage == cache::ArgumentStage::Comptime
                       ? "comptime"
                       : "runtime");
        out << ",\"constraint\":" << static_cast<unsigned>(argument.constraint)
            << ",\"value\":";
        if (argument.value) {
            std::ostringstream value;
            print_constant(*argument.value, value);
            quote(out, value.str());
        } else
            out << "null";
        out << '}';
    }
    out << "]}";
}
void work(std::ostream& out, const RecursiveStats& stats) {
    out << "{\"steps\":" << stats.steps << ",\"slices\":" << stats.slices
        << ",\"nodes\":" << stats.nodes << ",\"links\":" << stats.links
        << ",\"components\":" << stats.components
        << ",\"cyclic_components\":" << stats.cyclic_components
        << ",\"evaluations\":" << stats.evaluations
        << ",\"facts_added\":" << stats.facts_added
        << ",\"queue_peak\":" << stats.queue_peak << '}';
}
void result(std::ostream& out, const FunctionEvidenceView& evidence,
            RecursiveSummaryTask& task) {
    const auto value = task.result();
    out << "{\"channel\":";
    quote(out, value.channel == QueryKind::Value ? "value" : "effects");
    out << ",\"progress\":";
    quote(out, value.fixed_point ? "completed" : "paused");
    out << ",\"fixed_point\":" << (value.fixed_point ? "true" : "false")
        << ",\"precision\":";
    quote(out, value.precision == EvidencePrecision::ExactForRecordedModel
                   ? "exact-for-recorded-model"
               : value.precision == EvidencePrecision::Conservative
                   ? "conservative"
                   : "unresolved");
    out << ",\"issues\":" << value.issues << ",\"gaps\":[";
    constexpr std::array names{"incomplete-node",     "missing-summary",
                               "unbound-parameter",   "foreign-scope",
                               "unsupported-rule",    "unsupported-relation",
                               "unavailable-evidence"};
    bool first = true;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (value.issues & (1U << i)) {
            if (!first)
                out << ',';
            first = false;
            quote(out, names[i]);
        }
    out << "],\"instances_merged\":"
        << (value.instances_merged ? "true" : "false")
        << ",\"concrete_sources_complete\":false,\"atoms\":[";
    first = true;
    for (const auto& atom : value.atoms) {
        if (!first)
            out << ',';
        first = false;
        if (atom.kind == RecursiveAtomKind::Parameter) {
            out << "{\"kind\":\"parameter\",\"owner\":";
            key(out, evidence, {atom.owner});
            out << ",\"slot\":" << atom.slot << '}';
        } else
            out << "{\"kind\":\"source-family\",\"node\":" << atom.node << '}';
    }
    out << "],\"work\":";
    work(out, task.stats());
    out << ",\"proof_available\":" << (value.fixed_point ? "true" : "false")
        << ",\"nodes\":[";
    first = true;
    if (value.fixed_point)
        for (const auto& component : task.components())
            for (const auto id : component.nodes) {
                const auto lease = evidence.view()->pin(id);
                if (!lease || !lease->site())
                    throw std::runtime_error(
                        "recursive proof location unavailable");
                if (!first)
                    out << ',';
                first = false;
                const auto& node = lease->node();
                out << "{\"id\":" << id.value << ",\"rule\":";
                quote(out, rule_name(node.rule));
                out << ",\"rule_code\":" << static_cast<unsigned>(node.rule)
                    << ",\"site\":{\"label\":";
                quote(out, lease->site()->label);
                out << ",\"begin\":" << lease->site()->span.begin
                    << ",\"end\":" << lease->site()->span.end << '}'
                    << ",\"owner\":";
                key(out, evidence, node.owner);
                out << ",\"slot\":";
                if (node.rule == Rule::Parameter)
                    out << node.payload;
                else
                    out << "null";
                out << ",\"callee\":";
                key(out, evidence,
                    {node.rule == Rule::Call || node.rule == Rule::SummaryRef
                         ? node.payload
                         : 0});
                const auto decision = std::ranges::lower_bound(
                    evidence.decisions(), id.value, {},
                    [](const auto& item) { return item.node.value; });
                out << ",\"decision\":";
                if (decision != evidence.decisions().end() &&
                    decision->node == id) {
                    out << "{\"stage\":";
                    quote(out, hir::stage_name(decision->result));
                    out << ",\"rule\":" << static_cast<unsigned>(decision->rule)
                        << ",\"reason\":"
                        << static_cast<unsigned>(decision->reason)
                        << ",\"recursive_completion\":"
                        << (decision->reason == StageReason::RecursiveCompletion
                                ? "true"
                                : "false")
                        << '}';
                } else
                    out << "null";
                out << '}';
            }
    out << "],\"links\":[";
    first = true;
    if (value.fixed_point)
        for (const auto& link : task.links()) {
            if (!first)
                out << ',';
            first = false;
            out << "{\"from\":" << link.from.value
                << ",\"to\":" << link.to.value << ",\"kind\":";
            quote(out, link.kind == RecursiveLinkKind::Summary ? "summary"
                       : link.kind == RecursiveLinkKind::Argument
                           ? "argument"
                           : "dependency");
            out << ",\"relation\":";
            quote(out, relation_name(link.relation));
            out << ",\"slot\":" << link.slot << '}';
        }
    out << "],\"components\":[";
    first = true;
    if (value.fixed_point)
        for (const auto& component : task.components()) {
            if (!first)
                out << ',';
            first = false;
            out << "{\"cyclic\":" << (component.cyclic ? "true" : "false")
                << ",\"nodes\":[";
            bool initial = true;
            for (const auto node : component.nodes) {
                if (!initial)
                    out << ',';
                initial = false;
                out << node.value;
            }
            out << "],\"summaries\":[";
            initial = true;
            for (const auto summary : component.summaries) {
                if (!initial)
                    out << ',';
                initial = false;
                key(out, evidence, summary);
            }
            out << "]}";
        }
    out << "]}";
}
// The completed, immutable solver state is the result receipt. Rendering
// excludes invocation-local fields (generation, read-only and stage_runs), so
// (task, sequence) replays byte-for-byte under its strict compiler binding.
void revision_json(std::ostream& out, const FunctionEvidenceRevision& value) {
    out << "{\"closure\":";
    quote(out, value.closure.hex());
    out << ",\"positions\":";
    quote(out, value.positions.hex());
    out << ",\"source_revision\":";
    quote(out, value.source.hex());
    out << '}';
}
int run_recursive_task(const FunctionEvidenceView& evidence,
                       std::string_view source,
                       const FunctionSummaryEntry& summary, std::size_t slice,
                       const RecursiveSummaryOptions& recursive,
                       const QueryTaskOptions& options, bool stage_ran,
                       bool statistics) {
    using namespace query_task_cli;
    const auto binding = evidence.checkpoint_binding(source);
    const auto revision = evidence.revision(source);
    if (!binding || !revision)
        throw std::runtime_error("recursive task binding unavailable");
    const auto id = options.id ? *options.id : fresh_id();
    const auto channel =
        recursive.channel == "value" ? QueryKind::Value : QueryKind::Effects;
    cache::TaskStoreOptions bounds;
    bounds.disk_bytes = options.disk_bytes;
    bounds.store.codec.elements = options.record_limit;
    const cache::Limits limits{.elements = options.record_limit};
    // Validate the full saved binding with a read-only handle before opening
    // an existing task for writing. A mismatch must never create or mutate it.
    auto store = checked(cache::TaskStore::open(
        options.directory, id,
        options.action == "start" ? cache::TaskAccess::ReadWrite
                                  : cache::TaskAccess::ReadOnly,
        bounds));
    auto head = checked(store.head());
    if (options.action == "start" && head)
        throw std::runtime_error("query ID already exists; resume it or choose "
                                 "a new ID");
    if (options.action != "start" && !head)
        throw std::runtime_error("query task has no saved head");
    auto task = head ? checked(RecursiveSummaryTask::restore(
                           *binding, store, *head, summary.id, channel, limits))
                     : RecursiveSummaryTask(*binding, id, summary.id, channel);
    if (!head) {
        head = checked(task.save_segmented(
            store, 0, options.paging.segment_bytes, limits));
        std::cerr << "recursive-task: id=" << id.hex()
                  << " saved initial state\n";
    }
    if (options.action == "cancel" &&
        task.progress() == QueryProgress::Completed)
        throw std::runtime_error("recursive result is complete; acknowledge "
                                 "it or explicitly discard inactive tasks");
    const auto previously_acknowledged = task.acknowledged_sequence();
    if (options.acknowledge && !task.acknowledge_result(*options.acknowledge))
        throw std::runtime_error(
            "acknowledgement does not match a completed recursive result");
    bool changed = previously_acknowledged != task.acknowledged_sequence();
    if (!options.read_only && task.progress() != QueryProgress::Completed) {
        if (options.action != "start")
            store = checked(cache::TaskStore::open(
                options.directory, id, cache::TaskAccess::ReadWrite, bounds));
        if (options.action == "cancel") {
            changed = task.progress() != QueryProgress::Cancelled;
            task.cancel();
        } else {
            // A zero invocation budget preserves the stored state verbatim.
            const auto start_steps = task.stats().steps;
            for (std::size_t used = 0;
                 !options.max_slices || used < *options.max_slices; ++used) {
                auto amount = slice;
                if (recursive.max_steps) {
                    const auto spent = task.stats().steps - start_steps;
                    if (spent >= *recursive.max_steps)
                        break;
                    amount = std::min(amount, *recursive.max_steps - spent);
                }
                const auto current = checked(store.head());
                if (!current || current->generation != head->generation)
                    throw std::runtime_error(
                        "query head changed; resume the saved task again");
                task.resume();
                (void)task.advance(amount);
                changed = true;
                if (task.storage_failure())
                    throw std::runtime_error(
                        error_text(*task.storage_failure()));
                if (task.failure() || task.progress() == QueryProgress::Failed)
                    throw std::runtime_error(
                        "recursive summary evidence unavailable");
                if (task.progress() == QueryProgress::Completed)
                    break;
            }
        }
    }
    std::ostringstream delivery;
    if (task.pending_sequence()) {
        delivery << "{\"schema\":1,\"type\":\"recursive-result\",\"task\":\""
                 << id.hex() << "\",\"sequence\":1,\"evidence\":";
        revision_json(delivery, *revision);
        delivery << ",\"local_ids\":true,\"summary\":";
        describe(delivery, evidence, summary);
        delivery << ",\"result\":";
        result(delivery, evidence, task);
        delivery << "}\n";
    }
    const auto cumulative_work = task.stats();
    // Rendering can fail; no output escapes before the atomic head contains
    // the result. Publication errors (including after rename) require reread.
    if (changed) {
        if (options.action != "start")
            store = checked(cache::TaskStore::open(
                options.directory, id, cache::TaskAccess::ReadWrite, bounds));
        head = checked(task.offload(store, head->generation,
                                    options.paging.segment_bytes, limits));
    }
    std::ostringstream status;
    status
        << "{\"schema\":1,\"type\":\"status\",\"mode\":\"recursive-summary\","
           "\"task\":\""
        << id.hex() << "\",\"generation\":" << head->generation
        << ",\"state\":";
    quote(status, status_name(head->status));
    status << ",\"summary_key\":";
    quote(status, summary.key.hex());
    status << ",\"channel\":";
    quote(status, recursive.channel);
    status << ",\"storage\":\"memory\",\"read_only\":"
           << (options.read_only ? "true" : "false") << ",\"offloaded\":"
           << (task.state_memory().offloaded ? "true" : "false")
           << ",\"continuation_available\":true,\"restart_required\":false,"
              "\"acknowledge\":";
    if (task.pending_sequence())
        status << task.pending_sequence();
    else
        status << "null";
    status << ",\"acknowledged\":" << task.acknowledged_sequence()
           << ",\"stage_runs\":" << (stage_ran ? 1 : 0) << ",\"work\":";
    work(status, cumulative_work);
    status << "}\n";
    std::cout << delivery.str() << status.str();
    std::cout.flush();
    if (!std::cout)
        throw std::runtime_error(
            "recursive output failed; the saved result can be replayed");
    std::cerr << "recursive-task: id=" << id.hex()
              << " state=" << status_name(head->status)
              << " generation=" << head->generation << '\n';
    if (task.pending_sequence())
        std::cerr << "recursive-task: confirm result 1 with --query-ack=1 "
                     "after committing it\n";
    if (statistics) {
        const auto reads = evidence.view()->read_stats();
        std::cerr << "recursive-evidence metadata-objects="
                  << evidence.metadata_objects_read()
                  << " metadata-bytes=" << evidence.metadata_bytes_read()
                  << " cold-objects=" << reads.objects_read
                  << " cold-bytes=" << reads.payload_bytes_read
                  << " cached-bytes=" << reads.cached_bytes
                  << " live-bytes=" << reads.live_bytes << '\n';
    }
    return head->status == cache::TaskStatus::Completed ||
                   head->status == cache::TaskStatus::Cancelled
               ? 0
               : 3;
}
} // namespace
bool recursive_evidence_mode(std::string_view mode) {
    return mode == "summaries" || mode == "recursive-summary";
}
std::expected<void, std::string>
parse_recursive_summary_option(std::string_view argument,
                               RecursiveSummaryOptions& options) {
    options.specified = true;
    const auto value = argument.substr(argument.find('=') + 1);
    if (value.empty())
        return std::unexpected(
            "recursive summary option requires a non-empty value");
    if (argument.starts_with("--evidence-summary="))
        options.name = value;
    else if (argument.starts_with("--evidence-summary-key=")) {
        cache::Digest digest;
        if (value.size() != 64)
            return std::unexpected(
                "summary key must contain 64 hexadecimal digits");
        for (std::size_t i = 0; i < digest.bytes.size(); ++i) {
            unsigned byte{};
            const auto parsed = std::from_chars(
                value.data() + i * 2, value.data() + i * 2 + 2, byte, 16);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + i * 2 + 2)
                return std::unexpected(
                    "summary key must contain 64 hexadecimal digits");
            digest.bytes[i] = static_cast<std::uint8_t>(byte);
        }
        options.key = digest;
    } else if (argument.starts_with("--evidence-channel=")) {
        if (value != "value" && value != "effects" && value != "both")
            return std::unexpected("invalid recursive summary channel");
        options.channel = value;
        options.channel_set = true;
    } else if (argument.starts_with("--recursive-max-steps=")) {
        options.steps_set = true;
        if (value == "all")
            options.max_steps.reset();
        else {
            std::size_t steps{};
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), steps);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size())
                return std::unexpected("invalid recursive step limit");
            options.max_steps = steps;
        }
    } else
        return std::unexpected("unknown recursive summary option");
    return {};
}
std::expected<void, std::string>
validate_recursive_summary_options(const RecursiveSummaryOptions& options,
                                   std::string_view mode, std::string_view root,
                                   bool query_options) {
    if (!recursive_evidence_mode(mode)) {
        if (options.specified)
            return std::unexpected("summary options require summaries or "
                                   "recursive-summary evidence mode");
        return {};
    }
    if (!root.empty() || (query_options && mode == "summaries"))
        return std::unexpected("recursive summary modes do not accept "
                               "evidence-root or query-task options");
    if (query_options && mode == "recursive-summary" &&
        (!options.channel_set || options.channel == "both"))
        return std::unexpected(
            "persistent recursive tasks require one --evidence-channel=value "
            "or effects");
    if (!options.name.empty() && options.key)
        return std::unexpected("choose a summary name or key, not both");
    if (mode == "recursive-summary" && options.name.empty() && !options.key)
        return std::unexpected("recursive-summary requires evidence-summary or "
                               "evidence-summary-key");
    if (mode == "summaries" && (options.channel_set || options.steps_set))
        return std::unexpected(
            "summary catalog does not accept solver budgets or channels");
    return {};
}
int run_recursive_summaries(const FunctionEvidenceView& evidence,
                            std::string_view source, std::string_view mode,
                            std::size_t slice,
                            const RecursiveSummaryOptions& options,
                            bool stage_ran, bool statistics,
                            const QueryTaskOptions& query) {
    try {
        if (!slice || !recursive_evidence_mode(mode))
            throw std::invalid_argument("invalid recursive summary request");
        std::vector<const FunctionSummaryEntry*> matches;
        for (const auto& summary : evidence.summary_catalog())
            if ((options.name.empty() ||
                 evidence.scopes()[summary.id.value].name == options.name) &&
                (!options.key || *options.key == summary.key))
                matches.push_back(&summary);
        if (matches.empty() && (!options.name.empty() || options.key))
            throw std::runtime_error(
                "summary not found in the validated evidence closure");
        if (mode == "recursive-summary" && matches.size() != 1)
            throw std::runtime_error("summary name is ambiguous; list "
                                     "summaries and select a full key");
        if (query.enabled())
            return run_recursive_task(evidence, source, *matches[0], slice,
                                      options, query, stage_ran, statistics);
        const auto revision = evidence.revision(source);
        if (!revision)
            throw std::runtime_error(
                "recursive summary source binding unavailable");
        std::ostringstream output;
        output << "{\"schema\":1,\"type\":";
        quote(output,
              mode == "summaries" ? "summary-catalog" : "recursive-summary");
        output << ",\"evidence\":{\"closure\":";
        quote(output, revision->closure.hex());
        output << ",\"positions\":";
        quote(output, revision->positions.hex());
        output << ",\"source_revision\":";
        quote(output, revision->source.hex());
        output << "},\"local_ids\":true,\"stage_runs\":" << (stage_ran ? 1 : 0)
               << ",\"continuation_available\":false";
        bool completed = true;
        if (mode == "summaries") {
            output << ",\"summaries\":[";
            bool first = true;
            for (const auto* summary : matches) {
                if (!first)
                    output << ',';
                first = false;
                describe(output, evidence, *summary);
            }
            output << ']';
        } else {
            output << ",\"summary\":";
            describe(output, evidence, *matches[0]);
            output << ",\"results\":[";
            bool first = true;
            for (const auto channel : {QueryKind::Value, QueryKind::Effects}) {
                if (options.channel != "both" &&
                    options.channel !=
                        (channel == QueryKind::Value ? "value" : "effects"))
                    continue;
                RecursiveSummaryTask task(evidence.view(), matches[0]->id,
                                          channel);
                while (task.progress() == QueryProgress::Running ||
                       task.progress() == QueryProgress::Paused) {
                    auto amount = slice;
                    if (options.max_steps) {
                        if (task.stats().steps >= *options.max_steps)
                            break;
                        amount = std::min(amount, *options.max_steps -
                                                      task.stats().steps);
                    }
                    (void)task.advance(amount);
                }
                if (task.failure() || task.progress() == QueryProgress::Failed)
                    throw std::runtime_error(
                        "recursive summary evidence unavailable" +
                        (task.failure() ? " (" +
                                              std::to_string(static_cast<int>(
                                                  task.failure()->code)) +
                                              ")"
                                        : ""));
                completed &= task.progress() == QueryProgress::Completed;
                if (!first)
                    output << ',';
                first = false;
                result(output, evidence, task);
            }
            output << ']';
        }
        output << ",\"restart_required\":" << (completed ? "false" : "true")
               << "}\n";
        std::cout << output.str();
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("recursive summary output failed");
        if (statistics) {
            const auto reads = evidence.view()->read_stats();
            std::cerr << "recursive-evidence metadata-objects="
                      << evidence.metadata_objects_read()
                      << " metadata-bytes=" << evidence.metadata_bytes_read()
                      << " cold-objects=" << reads.objects_read
                      << " cold-bytes=" << reads.payload_bytes_read
                      << " cached-bytes=" << reads.cached_bytes
                      << " live-bytes=" << reads.live_bytes << '\n';
        }
        return completed ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
} // namespace pagos::driver
