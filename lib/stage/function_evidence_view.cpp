#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/provenance_topology.h"

#include <algorithm>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace pagos::stage {
namespace {
using namespace provenance;
constexpr auto import_bit = std::uint32_t{1} << 31U;
EvidenceError storage_error(cache::StoreError error) {
    auto code = EvidenceErrorCode::IoFailure;
    switch (error.code) {
    case cache::StoreErrorCode::MissingObject:
        code = EvidenceErrorCode::MissingBlock;
        break;
    case cache::StoreErrorCode::CorruptObject:
        code = EvidenceErrorCode::CorruptBlock;
        break;
    case cache::StoreErrorCode::IncompatibleVersion:
        code = EvidenceErrorCode::IncompatibleVersion;
        break;
    case cache::StoreErrorCode::LimitExceeded:
        code = EvidenceErrorCode::LimitExceeded;
        break;
    case cache::StoreErrorCode::OutOfMemory:
        code = EvidenceErrorCode::OutOfMemory;
        break;
    default:
        break;
    }
    return {code, error, error.codec};
}
struct Layout {
    NodeId first;
    SummaryId summary;
};
struct Mapping {
    std::shared_ptr<const FunctionEvidence> closure;
    std::vector<std::vector<Layout>> layouts;
    std::map<std::string, Layout> keys;
    std::vector<std::size_t> blocks;
    SummaryId scope(std::size_t group_id, EvidenceScopeRef ref) const {
        if (!ref)
            return {};
        const auto& group = closure->groups()[group_id];
        if (ref & import_bit)
            return keys.at(group.imports[(ref & ~import_bit) - 1].key.hex())
                .summary;
        return layouts[group_id][ref - 1].summary;
    }
    NodeId node(std::size_t group, std::size_t member, NodeId id) const {
        return id.value
                   ? NodeId{layouts[group][member].first.value + id.value - 1}
                   : NodeId{};
    }
    std::expected<std::shared_ptr<const EvidenceBlock>, EvidenceError>
    load(std::size_t index) const {
        const auto group_id = blocks[index];
        const auto& group = closure->groups()[group_id];
        auto details = closure->read_detail(group);
        if (!details)
            return std::unexpected(storage_error(details.error()));
        auto block = std::make_shared<EvidenceBlock>();
        for (std::size_t member = 0; member < details->size(); ++member) {
            auto& detail = (*details)[member];
            const auto site_base =
                static_cast<std::uint32_t>(block->sites.size());
            const auto edge_base =
                static_cast<std::uint32_t>(block->edges.size());
            for (auto& site : detail.sites)
                block->sites.push_back(std::move(site));
            for (std::size_t i = 0; i < detail.nodes.size(); ++i) {
                auto record = detail.nodes[i];
                block->site_indices.push_back(site_base + record.site);
                record.site =
                    node(group_id, member, {static_cast<std::uint32_t>(i + 1)})
                        .value;
                record.edge_begin += edge_base;
                if (record.rule == Rule::Call ||
                    record.rule == Rule::SummaryRef)
                    record.payload = scope(group_id, record.payload).value;
                if (record.owner.value)
                    record.owner = scope(group_id, record.owner.value);
                block->nodes.push_back(record);
            }
            for (auto edge : detail.edges) {
                edge.target = node(group_id, member, edge.target);
                block->edges.push_back(edge);
            }
        }
        return block;
    }
};
} // namespace
std::expected<FunctionEvidenceView, EvidenceError>
FunctionEvidenceView::open(FunctionEvidence evidence, std::size_t cache_bytes) {
    try {
        FunctionEvidenceView result;
        result.closure_ =
            std::make_shared<const FunctionEvidence>(std::move(evidence));
        auto mapping = std::make_shared<Mapping>();
        mapping->closure = result.closure_;
        mapping->layouts.resize(result.closure_->groups().size());
        std::vector<SummaryDescription> summaries;
        // Summary IDs are canonical by catalog key, independently of cold IO.
        std::map<std::string, SummaryId> summary_ids;
        result.scopes_.push_back({{}, "module", {}, {}});
        for (const auto& entry : result.closure_->catalog()) {
            const auto& member = result.closure_->member(entry);
            if (member.key.definition.kind == cache::DefinitionKind::Function) {
                auto id = SummaryId{
                    static_cast<std::uint32_t>(summary_ids.size() + 1)};
                summary_ids.emplace(entry.key.hex(), id);
                summaries.push_back({id, {}, {}});
                result.scopes_.push_back(
                    {id, member.key.definition.name, {}, {}});
                result.scopes_.back().specialization =
                    ProvenanceScope::Specialization{
                        member.key.definition.name, member.key.arguments,
                        member.key.definition,
                        member.key.definition_fingerprint};
            }
        }
        std::vector<EvidenceBlockRef> blocks;
        std::uint64_t next = 1;
        for (std::size_t group_id = 0;
             group_id < result.closure_->groups().size(); ++group_id) {
            const auto& group = result.closure_->groups()[group_id];
            const auto first = next;
            for (const auto& member : group.members) {
                const auto hash = cache::query_fingerprint(member.key)->hex();
                const auto found = summary_ids.find(hash);
                Layout layout{{static_cast<std::uint32_t>(next)},
                              found == summary_ids.end() ? SummaryId{}
                                                         : found->second};
                mapping->layouts[group_id].push_back(layout);
                mapping->keys.emplace(hash, layout);
                next += member.nodes;
                if (next > UINT32_MAX)
                    return std::unexpected(EvidenceError{
                        EvidenceErrorCode::LimitExceeded, {}, {}});
                if (layout.summary.value)
                    summaries[layout.summary.value - 1] = {
                        layout.summary,
                        {layout.first.value + member.value.value - 1},
                        {layout.first.value + member.effects.value - 1}};
            }
            if (next != first) {
                mapping->blocks.push_back(group_id);
                blocks.push_back({{static_cast<std::uint32_t>(first)},
                                  static_cast<std::uint32_t>(next - first),
                                  group.detail});
            }
        }
        for (std::size_t group_id = 0;
             group_id < result.closure_->groups().size(); ++group_id) {
            const auto& group = result.closure_->groups()[group_id];
            for (std::size_t member_id = 0; member_id < group.members.size();
                 ++member_id) {
                const auto& member = group.members[member_id];
                for (const auto& decision : member.decisions) {
                    const auto scope = mapping->scope(group_id, decision.scope);
                    const auto id =
                        mapping->node(group_id, member_id, decision.node);
                    result.decisions_.push_back(
                        {id, decision.result, decision.rule, decision.span,
                         decision.reason, decision.member, scope,
                         member.key.definition.kind ==
                                 cache::DefinitionKind::Module &&
                             scope.value != 0});
                    result.scopes_[scope.value].decisions.push_back(id);
                }
                for (const auto& root : member.roots) {
                    const auto scope = mapping->scope(group_id, root.scope);
                    if (root.requires_comptime)
                        result.scopes_[scope.value].contracts.push_back(
                            result.roots_.size());
                    result.roots_.push_back(
                        {root.name, root.span, root.stage,
                         mapping->node(group_id, member_id, root.node),
                         root.requires_comptime, scope});
                }
            }
        }
        std::ranges::sort(result.decisions_, {},
                          [](const auto& item) { return item.node.value; });
        auto view = EvidenceView::segmented(
            std::move(blocks), std::move(summaries),
            [mapping](std::size_t index) { return mapping->load(index); },
            cache_bytes);
        if (!view)
            return std::unexpected(view.error());
        result.view_ = std::move(*view);
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            EvidenceError{EvidenceErrorCode::OutOfMemory, {}, {}});
    }
}

bool explain_function_evidence(const FunctionEvidenceView& evidence,
                               std::ostream& output, std::ostream& telemetry,
                               std::string_view selected_root,
                               std::string_view mode, std::size_t slice_size,
                               bool statistics) {
    if (!slice_size || (mode != "sources" && mode != "witness" &&
                        mode != "topology" && mode != "decisions"))
        throw std::invalid_argument("invalid function evidence query");
    const QuerySlice slice{slice_size, slice_size, slice_size, slice_size};
    bool found = false, available = true;
    if (mode == "decisions") {
        for (const auto& scope : evidence.scopes()) {
            if (!selected_root.empty() && selected_root != scope.name)
                continue;
            found = true;
            output << "scope " << scope.name << '\n';
            for (const auto& decision : evidence.decisions()) {
                if (decision.scope != scope.id)
                    continue;
                output << "  decision rule="
                       << static_cast<unsigned>(decision.rule)
                       << " stage=" << hir::stage_name(decision.result)
                       << " reason=" << static_cast<unsigned>(decision.reason)
                       << " @" << decision.span.begin << ':'
                       << decision.span.end << '\n';
            }
        }
    }
    for (const auto& root : evidence.roots()) {
        if (mode == "decisions")
            break;
        if (!selected_root.empty() && selected_root != root.name)
            continue;
        found = true;
        output << "binding " << root.name << ": " << hir::stage_name(root.stage)
               << '\n';
        if (root.requires_comptime)
            output << "  contract: Comptime\n";
        if (mode == "topology") {
            EvidenceTopologyTask task(evidence.view(), root.node);
            std::vector<std::string> records;
            while (task.progress() != QueryProgress::Completed &&
                   task.progress() != QueryProgress::Failed) {
                auto page = task.advance(slice);
                for (const auto& record : page.records) {
                    auto lease = evidence.view()->pin(record.node);
                    if (!lease || !lease->site()) {
                        available = false;
                        continue;
                    }
                    std::ostringstream line;
                    line << "  topology " << static_cast<unsigned>(record.kind)
                         << ' ' << lease->site()->label << " @"
                         << lease->site()->span.begin << ':'
                         << lease->site()->span.end;
                    if (record.kind != TopologyRecordKind::Node) {
                        auto target = evidence.view()->pin(record.edge.target);
                        if (!target || !target->site()) {
                            available = false;
                            continue;
                        }
                        line << " -> " << target->site()->label << " @"
                             << target->site()->span.begin << ':'
                             << target->site()->span.end << " relation="
                             << static_cast<unsigned>(record.edge.relation)
                             << " port=" << static_cast<unsigned>(record.port);
                    }
                    records.push_back(line.str());
                }
            }
            std::ranges::sort(records);
            for (const auto& line : records)
                output << line << '\n';
            if (task.progress() == QueryProgress::Failed) {
                output << "  topology: unavailable\n";
                available = false;
            }
            continue;
        }
        for (auto kind : {QueryKind::Value, QueryKind::Effects}) {
            QueryTask task(evidence.view(), root.node, kind, mode == "witness");
            while (task.progress() != QueryProgress::Completed &&
                   task.progress() != QueryProgress::Failed)
                (void)task.advance(slice);
            output << "  " << (kind == QueryKind::Value ? "value" : "effects")
                   << ":";
            if (task.progress() == QueryProgress::Failed) {
                output << " unavailable error="
                       << (task.failure()
                               ? static_cast<int>(task.failure()->code)
                               : -1)
                       << '\n';
                available = false;
                continue;
            }
            const auto result = task.result();
            // Render owned semantic locations; never persist/compare virtual
            // IDs.
            std::vector<std::string> lines;
            bool locations_available = true;
            const auto add = [&](NodeId id, std::uint32_t context) {
                auto lease = evidence.view()->pin(id);
                if (!lease || !lease->site())
                    return false;
                const auto& site = *lease->site();
                std::ostringstream line;
                line << "    " << site.label << " @" << site.span.begin << ':'
                     << site.span.end;
                for (auto frame = context; frame;
                     frame = result.invocations[frame].parent) {
                    auto call =
                        evidence.view()->pin(result.invocations[frame].call);
                    if (!call || !call->site())
                        return false;
                    line << " via " << call->site()->label << " @"
                         << call->site()->span.begin << ':'
                         << call->site()->span.end;
                }
                lines.push_back(line.str());
                return true;
            };
            if (mode == "witness") {
                for (auto state : result.witness)
                    locations_available &= add(state.node, state.context);
            } else {
                for (auto source : result.sources)
                    locations_available &= add(source.node, source.context);
                std::ranges::sort(lines);
            }
            output << " precision="
                   << static_cast<unsigned>(task.coverage().precision)
                   << " issues=" << task.coverage().issues
                   << " all-sources=" << task.coverage().all_sources;
            if (!locations_available) {
                output << " locations=unavailable";
                available = false;
            }
            output << '\n';
            for (const auto& line : lines)
                output << line << '\n';
        }
    }
    if (!found) {
        telemetry << "error: evidence root not found: " << selected_root
                  << '\n';
        available = false;
    }
    if (statistics) {
        const auto stats = evidence.view()->read_stats();
        telemetry << "function-evidence metadata-objects="
                  << evidence.metadata_objects_read()
                  << " metadata-bytes=" << evidence.metadata_bytes_read()
                  << " cold-objects=" << stats.objects_read
                  << " cold-bytes=" << stats.payload_bytes_read
                  << " cached-bytes=" << stats.cached_bytes
                  << " live-bytes=" << stats.live_bytes << '\n';
    }
    return available;
}
} // namespace pagos::stage
