#include "pagos/stage/function_evidence.h"
#include "../cache/wire.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace pagos::stage {
namespace {
using cache::Error;
using cache::ObjectKind;
using cache::wire::Reader;
using cache::wire::Writer;
using namespace provenance;
constexpr auto import_bit = std::uint32_t{1} << 31U;
constexpr std::uint32_t record_model = 1;
cache::StoreError failure(Error error) {
    return {error == Error::IncompatibleVersion
                ? cache::StoreErrorCode::IncompatibleVersion
            : error == Error::LimitExceeded
                ? cache::StoreErrorCode::LimitExceeded
                : cache::StoreErrorCode::CorruptObject,
            {},
            error};
}
void require(bool valid, Error error = Error::InvalidFormat) {
    if (!valid)
        throw error;
}
cache::Bytes finish(Writer& writer) {
    auto result = writer.finish();
    if (!result)
        throw result.error();
    return std::move(*result);
}
void finish(Reader& reader) {
    if (auto result = reader.finish(); !result)
        throw result.error();
}
std::uint32_t checked_count(Reader& reader, std::size_t minimum = 1) {
    const auto count = reader.count(minimum);
    if (!reader.good())
        finish(reader);
    return count;
}
void blob(Writer& writer, std::span<const std::uint8_t> bytes) {
    writer.count(bytes.size());
    writer.raw(bytes);
}
std::span<const std::uint8_t> blob(Reader& reader) {
    return reader.raw(checked_count(reader));
}
cache::Digest digest(Reader& reader) {
    cache::Digest result;
    auto bytes = reader.raw(32);
    if (reader.good())
        std::ranges::copy(bytes, result.bytes.begin());
    return result;
}
void reference(Writer& writer, const cache::ObjectRef& ref) {
    writer.u8(static_cast<std::uint8_t>(ref.kind));
    writer.raw(ref.digest.bytes);
    writer.u64(ref.size);
}
cache::ObjectRef reference(Reader& reader, ObjectKind kind) {
    cache::ObjectRef ref{static_cast<ObjectKind>(reader.u8()), digest(reader),
                         reader.u64()};
    if (ref.kind != kind || ref.size < 9)
        reader.fail(Error::InvalidFormat);
    if (ref.size > reader.limits().bytes)
        reader.fail(Error::LimitExceeded);
    return ref;
}
void unit_header(Writer& writer, ObjectKind kind) {
    cache::wire::header(writer, kind);
    writer.u32(function_evidence_version);
    writer.u32(record_model);
}
void unit_header(Reader& reader, ObjectKind kind) {
    cache::wire::header(reader, kind);
    if (reader.u32() != function_evidence_version ||
        reader.u32() != record_model)
        reader.fail(Error::IncompatibleVersion);
}
struct Anchors {
    const CacheIdentity& identity;
    std::map<std::pair<std::size_t, std::size_t>, const SourceAnchor*> spans;
    explicit Anchors(const CacheIdentity& id) : identity(id) {
        for (const auto& item : identity.sites)
            spans.try_emplace(std::pair{item.span.begin, item.span.end},
                              &item.anchor);
    }
    void write(Writer& writer, source::Span span) const {
        writer.boolean(span.begin != 0 || span.end != 0);
        if (span.begin == 0 && span.end == 0)
            return;
        auto found = spans.find({span.begin, span.end});
        require(found != spans.end(), Error::InvalidAnchor);
        auto bytes = encode_source_anchor(*found->second, writer.limits());
        require(bytes.has_value(),
                bytes ? Error::InvalidAnchor : bytes.error());
        blob(writer, *bytes);
    }
};
source::Span anchor(Reader& reader, const CacheIdentity& identity,
                    std::optional<SourceAnchor>* source_anchor = nullptr,
                    const FunctionEvidence* evidence = nullptr) {
    if (!reader.boolean())
        return {};
    auto decoded = decode_source_anchor(blob(reader), reader.limits());
    if (!decoded)
        throw decoded.error();
    if (source_anchor)
        *source_anchor = *decoded;
    auto span = evidence ? evidence->resolve_anchor(*decoded)
                         : identity.resolve(*decoded);
    if (!span)
        throw span.error();
    return *span;
}
void key(Writer& writer, const cache::QueryKey& value) {
    auto bytes = cache::encode_query_key(value, writer.limits());
    if (!bytes)
        throw bytes.error();
    blob(writer, *bytes);
}
cache::QueryContext evidence_context(cache::QueryContext context) {
    context.semantic_options += ";pagos.function-evidence.v1;p0";
    return context;
}
cache::QueryKey key(Reader& reader, const CacheIdentity& identity,
                    const cache::QueryContext& context) {
    const auto bytes = blob(reader);
    auto decoded = cache::decode_query_key(bytes, reader.limits());
    if (!decoded)
        throw decoded.error();
    auto canonical = cache::encode_query_key(*decoded, reader.limits());
    require(canonical && std::ranges::equal(*canonical, bytes));
    require(decoded->context == evidence_context(context),
            Error::InvalidContext);
    if (decoded->definition.kind == cache::DefinitionKind::Module) {
        require(decoded->kind == cache::QueryKind::FreezeEvidence &&
                decoded->definition.name == "$module.unit" &&
                decoded->definition_fingerprint == identity.module_fingerprint);
    } else {
        require(decoded->kind == cache::QueryKind::FreezeFunctionEvidence);
        const auto* definition = identity.find(cache::DefinitionKind::Function,
                                               decoded->definition.name);
        require(definition && definition->id == decoded->definition &&
                    definition->semantic_fingerprint ==
                        decoded->definition_fingerprint,
                Error::MissingDefinition);
        require(definition->parameters.size() == decoded->arguments.size());
        for (std::size_t i = 0; i < decoded->arguments.size(); ++i)
            require(definition->parameters[i] == decoded->arguments[i].type);
    }
    return std::move(*decoded);
}
cache::Digest key_digest(const cache::QueryKey& key_value) {
    auto hash = cache::query_fingerprint(key_value);
    if (!hash)
        throw hash.error();
    return *hash;
}
struct Unit {
    EvidenceMember member;
    std::vector<NodeId> nodes;
    std::map<std::uint32_t, std::uint32_t> local;
    std::vector<std::uint32_t> dependencies;
    std::vector<const StageDecision*> decisions;
    std::vector<const ProvenanceRoot*> roots;
};
std::vector<Unit> units(const ProvenanceAnalysis& analysis,
                        const CacheIdentity& identity,
                        const cache::QueryContext& context,
                        cache::Limits limits) {
    const auto count = analysis.scopes().size();
    require(count <= limits.elements && count < import_bit,
            Error::LimitExceeded);
    std::vector<Unit> result(count);
    std::set<std::string> keys;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& scope = analysis.scopes()[i];
        require(scope.id.value == i);
        auto& unit = result[i];
        if (i == 0) {
            unit.member.key = FunctionEvidence::key_for(identity, context);
            unit.member.key.definition.name = "$module.unit";
        } else {
            require(scope.specialization.has_value(), Error::MissingDefinition);
            const auto& specialization = *scope.specialization;
            const auto* definition =
                identity.find(cache::DefinitionKind::Function,
                              specialization.definition_name);
            require(definition != nullptr &&
                        specialization.definition.has_value(),
                    Error::MissingDefinition);
            require(*specialization.definition == definition->id &&
                        specialization.definition_fingerprint ==
                            definition->semantic_fingerprint,
                    Error::InvalidContext);
            auto query = make_query_key(
                *definition, evidence_context(context),
                specialization.arguments,
                cache::QueryKind::FreezeFunctionEvidence, limits);
            if (!query)
                throw query.error();
            unit.member.key = std::move(*query);
            const auto& summary = analysis.graph().summary(scope.id);
            unit.member.value = summary.value;
            unit.member.effects = summary.effects;
        }
        require(keys.insert(key_digest(unit.member.key).hex()).second,
                Error::InvalidContext);
    }
    for (const auto& decision : analysis.decisions()) {
        require(decision.scope.value < count);
        result[decision.instantiated ? 0 : decision.scope.value]
            .decisions.push_back(&decision);
    }
    for (const auto& root : analysis.roots())
        result[0].roots.push_back(&root);
    std::size_t total_nodes = 0;
    for (auto& unit : result) {
        std::vector<NodeId> pending;
        if (unit.member.value.value)
            pending.push_back(unit.member.value);
        if (unit.member.effects.value)
            pending.push_back(unit.member.effects);
        for (auto decision : unit.decisions)
            pending.push_back(decision->node);
        for (auto root : unit.roots)
            pending.push_back(root->node);
        std::set<std::uint32_t> seen, dependencies;
        unit.member.recorded_complete = true;
        while (!pending.empty()) {
            auto id = pending.back();
            pending.pop_back();
            if (!seen.insert(id.value).second)
                continue;
            require(++total_nodes <= limits.elements, Error::LimitExceeded);
            const auto& node = analysis.graph().node(id);
            unit.member.recorded_complete &= node.rule != Rule::Incomplete;
            if (node.rule == Rule::Call || node.rule == Rule::SummaryRef)
                dependencies.insert(node.payload);
            if (node.rule == Rule::Parameter)
                dependencies.insert(node.owner.value);
            for (const auto& edge : analysis.graph().edges(id))
                pending.push_back(edge.target);
        }
        for (auto id : seen) {
            unit.nodes.push_back({id});
            unit.local[id] = static_cast<std::uint32_t>(unit.nodes.size());
        }
        unit.member.nodes = static_cast<std::uint32_t>(unit.nodes.size());
        if (unit.member.value.value)
            unit.member.value = {unit.local.at(unit.member.value.value)};
        if (unit.member.effects.value)
            unit.member.effects = {unit.local.at(unit.member.effects.value)};
        for (auto decision : unit.decisions)
            if (decision->scope.value)
                dependencies.insert(decision->scope.value);
        for (auto root : unit.roots)
            if (root->scope.value)
                dependencies.insert(root->scope.value);
        for (auto dependency : dependencies) {
            require(dependency > 0 && dependency < count);
            unit.dependencies.push_back(dependency);
        }
    }
    return result;
}
// Iterative Kosaraju; adjacency is scope-level only, never dynamic call paths.
std::vector<std::vector<std::uint32_t>>
components(const std::vector<Unit>& units_value) {
    std::vector<std::vector<std::uint32_t>> reverse(units_value.size());
    for (std::uint32_t i = 0; i < units_value.size(); ++i)
        for (auto target : units_value[i].dependencies)
            reverse[target].push_back(i);
    std::vector<bool> visited(units_value.size());
    std::vector<std::uint32_t> order;
    for (std::uint32_t root = 0; root < units_value.size(); ++root) {
        if (visited[root])
            continue;
        std::vector<std::pair<std::uint32_t, std::size_t>> stack{{root, 0}};
        visited[root] = true;
        while (!stack.empty()) {
            auto& [id, cursor] = stack.back();
            const auto& edges = units_value[id].dependencies;
            if (cursor < edges.size()) {
                auto next = edges[cursor++];
                if (!visited[next]) {
                    visited[next] = true;
                    stack.emplace_back(next, 0);
                }
            } else {
                order.push_back(id);
                stack.pop_back();
            }
        }
    }
    std::ranges::fill(visited, false);
    std::vector<std::vector<std::uint32_t>> groups;
    for (auto position = order.rbegin(); position != order.rend(); ++position) {
        if (visited[*position])
            continue;
        std::vector<std::uint32_t> group, stack{*position};
        visited[*position] = true;
        while (!stack.empty()) {
            auto id = stack.back();
            stack.pop_back();
            group.push_back(id);
            for (auto next : reverse[id])
                if (!visited[next]) {
                    visited[next] = true;
                    stack.push_back(next);
                }
        }
        std::ranges::sort(group, {}, [&](auto id) {
            return key_digest(units_value[id].member.key).hex();
        });
        groups.push_back(std::move(group));
    }
    // Sources precede dependencies in Kosaraju order; publish leaves first.
    std::ranges::reverse(groups);
    return groups;
}
void decision(Writer& writer, const StageDecision& value, std::uint32_t node,
              EvidenceScopeRef scope, const Anchors& anchors) {
    writer.u32(node);
    writer.u8(static_cast<std::uint8_t>(value.result));
    writer.u8(static_cast<std::uint8_t>(value.rule));
    writer.u8(static_cast<std::uint8_t>(value.reason));
    writer.boolean(value.member.has_value());
    if (value.member)
        writer.u32(*value.member);
    writer.u32(scope);
    anchors.write(writer, value.span);
}
bool stage_valid(hir::Stage value) {
    return value == hir::Stage::Comptime || value == hir::Stage::Runtime;
}
EvidenceDecision decision(Reader& reader, const CacheIdentity& identity,
                          const FunctionEvidence& evidence) {
    EvidenceDecision value;
    value.node = {reader.u32()};
    value.result = static_cast<hir::Stage>(reader.u8());
    value.rule = static_cast<StageRule>(reader.u8());
    value.reason = static_cast<StageReason>(reader.u8());
    if (reader.boolean())
        value.member = reader.u32();
    value.scope = reader.u32();
    value.span = anchor(reader, identity, nullptr, &evidence);
    if (!stage_valid(value.result) ||
        value.rule > StageRule::ConservativeFallback ||
        value.reason > StageReason::RecursiveCompletion)
        reader.fail(Error::InvalidFormat);
    return value;
}
bool scope_valid(EvidenceScopeRef scope, const EvidenceGroup& group) {
    return scope == 0 || ((scope & import_bit) ? (scope & ~import_bit) > 0 &&
                                                     (scope & ~import_bit) <=
                                                         group.imports.size()
                                               : scope <= group.members.size());
}
EvidenceGroup decode_summary(std::span<const std::uint8_t> bytes,
                             const cache::ObjectRef& object,
                             const CacheIdentity& identity,
                             const cache::QueryContext& context,
                             cache::Limits limits,
                             const FunctionEvidence& evidence) {
    Reader reader(bytes, limits);
    unit_header(reader, ObjectKind::EvidenceSummary);
    if (!reader.good())
        finish(reader);
    require(digest(reader) == identity.module_fingerprint,
            Error::InvalidContext);
    EvidenceGroup group;
    group.object = object;
    group.detail = reference(reader, ObjectKind::EvidenceDetail);
    const auto imports = checked_count(reader, 41 + 4 + 32);
    std::string previous_import;
    for (std::uint32_t i = 0; i < imports && reader.good(); ++i) {
        EvidenceImport imported{reference(reader, ObjectKind::EvidenceSummary),
                                reader.u32(), digest(reader)};
        require(imported.key.hex() > previous_import);
        previous_import = imported.key.hex();
        group.imports.push_back(std::move(imported));
    }
    const auto count = checked_count(reader, 4);
    require(count > 0 && count < import_bit);
    group.members.reserve(count);
    std::string previous_member;
    for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
        EvidenceMember member;
        member.key = key(reader, identity, context);
        const auto hash = key_digest(member.key).hex();
        require(hash > previous_member);
        previous_member = hash;
        member.nodes = reader.u32();
        if (member.nodes > limits.elements)
            reader.fail(Error::LimitExceeded);
        member.value = {reader.u32()};
        member.effects = {reader.u32()};
        member.recorded_complete = reader.boolean();
        require(member.key.definition.kind == cache::DefinitionKind::Module
                    ? member.value.value == 0 && member.effects.value == 0
                    : member.value.value > 0 && member.effects.value > 0);
        require(member.value.value <= member.nodes &&
                member.effects.value <= member.nodes);
        const auto decisions = checked_count(reader, 4 + 1 + 1 + 1 + 1 + 4 + 1);
        for (std::uint32_t j = 0; j < decisions && reader.good(); ++j) {
            auto value = decision(reader, identity, evidence);
            require(value.node.value > 0 && value.node.value <= member.nodes);
            if (member.key.definition.kind == cache::DefinitionKind::Function)
                require(value.scope == i + 1);
            member.decisions.push_back(std::move(value));
        }
        const auto roots = checked_count(reader, 4 + 4 + 1 + 1 + 4 + 1);
        require(roots == 0 ||
                member.key.definition.kind == cache::DefinitionKind::Module);
        for (std::uint32_t j = 0; j < roots && reader.good(); ++j) {
            EvidenceRoot root;
            root.name = reader.string();
            root.node = {reader.u32()};
            root.stage = static_cast<hir::Stage>(reader.u8());
            root.requires_comptime = reader.boolean();
            root.scope = reader.u32();
            root.span = anchor(reader, identity, nullptr, &evidence);
            require(root.node.value > 0 && root.node.value <= member.nodes &&
                    stage_valid(root.stage));
            member.roots.push_back(std::move(root));
        }
        group.members.push_back(std::move(member));
    }
    finish(reader);
    for (const auto& member : group.members) {
        for (const auto& value : member.decisions)
            require(scope_valid(value.scope, group));
        for (const auto& root : member.roots)
            require(scope_valid(root.scope, group));
    }
    return group;
}
} // namespace

cache::QueryKey FunctionEvidence::key_for(const CacheIdentity& identity,
                                          const cache::QueryContext& context) {
    return {cache::QueryKind::FreezeEvidence,
            evidence_context(context),
            {identity.module, cache::DefinitionKind::Module, "$module.units"},
            identity.module_fingerprint,
            {}};
}
FunctionEvidence::FunctionEvidence(cache::FileStore store,
                                   CacheIdentity identity, cache::Limits limits)
    : store_(std::move(store)), identity_(std::move(identity)),
      limits_(limits) {
    for (std::size_t i = 0; i < identity_.sites.size(); ++i) {
        const auto& anchor = identity_.sites[i].anchor;
        anchors_.emplace(
            std::tuple{static_cast<unsigned>(anchor.definition.kind),
                       anchor.definition.name, anchor.position},
            i);
    }
}
std::expected<source::Span, Error>
FunctionEvidence::resolve_anchor(const SourceAnchor& anchor) const {
    const auto found =
        anchors_.find({static_cast<unsigned>(anchor.definition.kind),
                       anchor.definition.name, anchor.position});
    if (found == anchors_.end() ||
        identity_.sites[found->second].anchor != anchor)
        return std::unexpected(Error::InvalidAnchor);
    return identity_.sites[found->second].span;
}

std::expected<cache::PublishResult, cache::StoreError>
FunctionEvidence::publish(const cache::FileStore& store,
                          const ProvenanceAnalysis& analysis,
                          const CacheIdentity& identity,
                          const cache::QueryContext& context,
                          cache::Limits limits) {
    try {
        if (!analysis.available() ||
            (analysis.spilled() && !analysis.validate_details()))
            return std::unexpected(cache::StoreError{
                cache::StoreErrorCode::InvalidRequest, {}, {}});
        require(identity.module == context.module &&
                    analysis.identity().has_value(),
                Error::InvalidContext);
        require(analysis.identity()->context == context &&
                    analysis.identity()->module_fingerprint ==
                        identity.module_fingerprint,
                Error::InvalidContext);
        const auto root_key = key_for(identity, context);
        const auto root_key_bytes = cache::encode_query_key(root_key, limits);
        if (!root_key_bytes)
            throw root_key_bytes.error();
        auto frozen = units(analysis, identity, context, limits);
        auto grouped = components(frozen);
        Anchors anchors(identity);
        std::vector<EvidenceCatalogEntry> catalog(frozen.size());
        for (const auto& group : grouped) {
            std::map<std::uint32_t, EvidenceScopeRef> references;
            for (std::uint32_t i = 0; i < group.size(); ++i)
                references[group[i]] = i + 1;
            std::set<std::uint32_t> imported_scopes;
            for (auto id : group)
                for (auto target : frozen[id].dependencies)
                    if (!references.contains(target))
                        imported_scopes.insert(target);
            std::vector<std::uint32_t> imports(imported_scopes.begin(),
                                               imported_scopes.end());
            std::ranges::sort(imports, {}, [&](auto id) {
                return key_digest(frozen[id].member.key).hex();
            });
            Writer detail(limits), summary(limits);
            unit_header(detail, ObjectKind::EvidenceDetail);
            detail.raw(identity.module_fingerprint.bytes);
            detail.count(group.size());
            std::uint32_t slot = 0;
            for (auto imported : imports)
                references[imported] = import_bit | ++slot;
            const auto scope_ref = [&](std::uint32_t scope) {
                return scope == 0 ? 0U : references.at(scope);
            };
            for (auto id : group) {
                const auto& unit = frozen[id];
                // Bind the cold block to the exact member key as well as its
                // directory.
                detail.raw(key_digest(unit.member.key).bytes);
                std::map<SiteId, std::uint32_t> site_ids;
                std::vector<SiteId> sites;
                for (auto node_id : unit.nodes) {
                    auto site_id = analysis.graph().node(node_id).site;
                    if (!site_ids.contains(site_id)) {
                        sites.push_back(site_id);
                        site_ids[site_id] =
                            static_cast<std::uint32_t>(sites.size());
                    }
                }
                detail.count(sites.size());
                for (auto site_id : sites) {
                    const auto& site = analysis.site(site_id);
                    anchors.write(detail, site.span);
                    detail.string(site.label);
                }
                detail.count(unit.nodes.size());
                std::size_t edges = 0;
                for (auto node_id : unit.nodes) {
                    const auto& node = analysis.graph().node(node_id);
                    detail.u8(static_cast<std::uint8_t>(node.rule));
                    detail.u32(site_ids.at(node.site));
                    detail.count(edges);
                    detail.u32(node.edge_count);
                    detail.u32(node.rule == Rule::Call ||
                                       node.rule == Rule::SummaryRef
                                   ? scope_ref(node.payload)
                                   : node.payload);
                    detail.u32(node.owner.value ? scope_ref(node.owner.value)
                                                : 0);
                    edges += node.edge_count;
                }
                detail.count(edges);
                for (auto node_id : unit.nodes)
                    for (auto edge : analysis.graph().edges(node_id)) {
                        detail.u32(unit.local.at(edge.target.value));
                        detail.u8(static_cast<std::uint8_t>(edge.relation));
                    }
            }
            auto detail_ref =
                store.write_object(ObjectKind::EvidenceDetail, finish(detail));
            if (!detail_ref)
                return std::unexpected(detail_ref.error());
            unit_header(summary, ObjectKind::EvidenceSummary);
            summary.raw(identity.module_fingerprint.bytes);
            reference(summary, *detail_ref);
            summary.count(imports.size());
            for (auto imported : imports) {
                const auto& entry = catalog[imported];
                require(entry.summary.kind == ObjectKind::EvidenceSummary);
                reference(summary, entry.summary);
                summary.u32(entry.member);
                summary.raw(entry.key.bytes);
            }
            summary.count(group.size());
            for (auto id : group) {
                const auto& unit = frozen[id];
                key(summary, unit.member.key);
                summary.u32(unit.member.nodes);
                summary.u32(unit.member.value.value);
                summary.u32(unit.member.effects.value);
                summary.boolean(unit.member.recorded_complete);
                summary.count(unit.decisions.size());
                for (auto value : unit.decisions)
                    decision(summary, *value, unit.local.at(value->node.value),
                             scope_ref(value->scope.value), anchors);
                summary.count(unit.roots.size());
                for (auto root : unit.roots) {
                    summary.string(root->name);
                    summary.u32(unit.local.at(root->node.value));
                    summary.u8(static_cast<std::uint8_t>(root->stage));
                    summary.boolean(root->requires_comptime);
                    summary.u32(scope_ref(root->scope.value));
                    anchors.write(summary, root->span);
                }
            }
            auto summary_ref = store.write_object(ObjectKind::EvidenceSummary,
                                                  finish(summary));
            if (!summary_ref)
                return std::unexpected(summary_ref.error());
            for (std::uint32_t i = 0; i < group.size(); ++i)
                catalog[group[i]] = {*summary_ref, i,
                                     key_digest(frozen[group[i]].member.key)};
        }
        Writer root(limits);
        unit_header(root, ObjectKind::EvidenceRoot);
        blob(root, *root_key_bytes);
        root.raw(identity.module_fingerprint.bytes);
        root.count(catalog.size());
        // Catalog order is canonical by stable key, independent of SummaryId.
        std::ranges::sort(catalog, {},
                          [](const auto& entry) { return entry.key.hex(); });
        for (const auto& entry : catalog) {
            reference(root, entry.summary);
            root.u32(entry.member);
            root.raw(entry.key.bytes);
        }
        auto bytes = finish(root);
        const auto semantic =
            cache::fingerprint("pagos.function-evidence-root.v1", bytes);
        const std::array artifacts{cache::ArtifactInput{
            "root", ObjectKind::EvidenceRoot, std::move(bytes), true}};
        return store.publish(root_key, identity.module_fingerprint, semantic,
                             artifacts);
    } catch (const EvidenceUnavailable& unavailable) {
        return std::unexpected(unavailable.error().storage.value_or(
            cache::StoreError{cache::StoreErrorCode::IoFailure,
                              {},
                              unavailable.error().codec}));
    } catch (Error error) {
        return std::unexpected(failure(error));
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            cache::StoreError{cache::StoreErrorCode::OutOfMemory, {}, {}});
    }
}

std::expected<FunctionEvidence, cache::StoreError>
FunctionEvidence::load(cache::FileStore store, CacheIdentity identity,
                       const cache::QueryContext& context,
                       cache::Limits limits) {
    try {
        FunctionEvidence result(std::move(store), std::move(identity), limits);
        require(context.module == result.identity_.module,
                Error::InvalidContext);
        const auto root_key = key_for(result.identity_, context);
        auto candidate = result.store_.lookup_references(
            root_key, result.identity_.module_fingerprint);
        if (!candidate)
            return std::unexpected(candidate.error());
        require(candidate->artifacts.size() == 1 &&
                candidate->artifacts[0].name == "root" &&
                candidate->artifacts[0].required &&
                candidate->artifacts[0].object.kind ==
                    ObjectKind::EvidenceRoot);
        auto bytes = result.store_.read_object(candidate->artifacts[0].object);
        if (!bytes)
            return std::unexpected(bytes.error());
        require(cache::fingerprint("pagos.function-evidence-root.v1", *bytes) ==
                candidate->semantic_result);
        ++result.metadata_objects_;
        result.metadata_bytes_ += bytes->size();
        Reader reader(*bytes, limits);
        unit_header(reader, ObjectKind::EvidenceRoot);
        if (!reader.good())
            finish(reader);
        auto decoded_key = cache::decode_query_key(blob(reader), limits);
        require(decoded_key && *decoded_key == root_key, Error::InvalidContext);
        require(digest(reader) == result.identity_.module_fingerprint,
                Error::InvalidContext);
        const auto count = checked_count(reader, 41 + 4 + 32);
        require(count > 0);
        std::string previous;
        for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
            EvidenceCatalogEntry entry{
                reference(reader, ObjectKind::EvidenceSummary), reader.u32(),
                digest(reader)};
            require(entry.key.hex() > previous);
            previous = entry.key.hex();
            result.catalog_.push_back(entry);
        }
        finish(reader);
        std::vector<cache::ObjectRef> pending;
        for (const auto& entry : result.catalog_)
            pending.push_back(entry.summary);
        std::map<std::string, cache::ObjectRef> seen;
        std::size_t total_members = 0, total_nodes = 0;
        while (!pending.empty()) {
            auto ref = pending.back();
            pending.pop_back();
            auto [position, inserted] = seen.try_emplace(ref.digest.hex(), ref);
            if (!inserted) {
                require(position->second == ref);
                continue;
            }
            require(seen.size() <= limits.elements, Error::LimitExceeded);
            auto summary_bytes = result.store_.read_object(ref);
            if (!summary_bytes)
                return std::unexpected(summary_bytes.error());
            ++result.metadata_objects_;
            result.metadata_bytes_ += summary_bytes->size();
            auto group = decode_summary(*summary_bytes, ref, result.identity_,
                                        context, limits, result);
            total_members += group.members.size();
            require(total_members <= limits.elements, Error::LimitExceeded);
            for (const auto& member : group.members) {
                total_nodes += member.nodes;
                require(total_nodes <= limits.elements, Error::LimitExceeded);
            }
            for (const auto& imported : group.imports)
                pending.push_back(imported.summary);
            result.groups_.push_back(std::move(group));
        }
        std::set<std::string> member_keys;
        std::size_t modules = 0;
        for (const auto& group : result.groups_)
            for (const auto& value : group.members) {
                require(member_keys.insert(key_digest(value.key).hex()).second);
                modules +=
                    value.key.definition.kind == cache::DefinitionKind::Module
                        ? 1U
                        : 0U;
            }
        require(modules == 1 && member_keys.size() == result.catalog_.size());
        for (const auto& entry : result.catalog_)
            (void)result.member(entry);
        for (const auto& group : result.groups_) {
            for (const auto& imported : group.imports)
                (void)result.member(
                    {imported.summary, imported.member, imported.key});
            const auto valid_owner = [&](EvidenceScopeRef scope) {
                if (scope == 0)
                    return;
                const auto& target =
                    (scope & import_bit)
                        ? result.member(
                              {group.imports[(scope & ~import_bit) - 1].summary,
                               group.imports[(scope & ~import_bit) - 1].member,
                               group.imports[(scope & ~import_bit) - 1].key})
                        : group.members[scope - 1];
                require(target.key.definition.kind ==
                        cache::DefinitionKind::Function);
            };
            for (const auto& member : group.members) {
                for (const auto& value : member.decisions)
                    valid_owner(value.scope);
                for (const auto& root : member.roots)
                    valid_owner(root.scope);
            }
        }
        return result;
    } catch (Error error) {
        return std::unexpected(failure(error));
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            cache::StoreError{cache::StoreErrorCode::OutOfMemory, {}, {}});
    }
}
const EvidenceMember&
FunctionEvidence::member(const EvidenceCatalogEntry& entry) const {
    auto found =
        std::ranges::find(groups_, entry.summary, &EvidenceGroup::object);
    require(found != groups_.end() && entry.member < found->members.size());
    const auto& result = found->members[entry.member];
    require(key_digest(result.key) == entry.key);
    return result;
}

std::expected<std::vector<EvidenceMemberDetail>, cache::StoreError>
FunctionEvidence::read_detail(const EvidenceGroup& group) const {
    try {
        // Only accept a group from this validated closure, not arbitrary
        // metadata.
        const auto found =
            std::ranges::find(groups_, group.object, &EvidenceGroup::object);
        require(found != groups_.end() && &*found == &group);
        auto bytes = store_.read_object(group.detail);
        if (!bytes)
            return std::unexpected(bytes.error());
        Reader reader(*bytes, limits_);
        unit_header(reader, ObjectKind::EvidenceDetail);
        if (!reader.good())
            finish(reader);
        require(digest(reader) == identity_.module_fingerprint,
                Error::InvalidContext);
        const auto count = checked_count(reader, 32 + 4 + 4 + 4);
        require(count == group.members.size());
        std::vector<EvidenceMemberDetail> details;
        for (std::uint32_t member_index = 0;
             member_index < count && reader.good(); ++member_index) {
            const auto& member_value = group.members[member_index];
            require(digest(reader) == key_digest(member_value.key));
            EvidenceMemberDetail detail;
            detail.sites.push_back({});
            std::vector<std::optional<SourceAnchor>> site_anchors(1);
            const auto sites = checked_count(reader, 1 + 4);
            for (std::uint32_t i = 0; i < sites && reader.good(); ++i) {
                std::optional<SourceAnchor> site_anchor;
                auto span = anchor(reader, identity_, &site_anchor, this);
                detail.sites.push_back({span, reader.string()});
                site_anchors.push_back(std::move(site_anchor));
            }
            const auto nodes = checked_count(reader, 21);
            require(nodes == member_value.nodes);
            detail.nodes.reserve(nodes);
            std::uint64_t edge_count = 0;
            bool recorded_complete = true;
            for (std::uint32_t i = 0; i < nodes && reader.good(); ++i) {
                Node node;
                node.rule = static_cast<Rule>(reader.u8());
                node.site = reader.u32();
                node.edge_begin = reader.u32();
                node.edge_count = reader.u32();
                node.payload = reader.u32();
                node.owner = {reader.u32()};
                require(node.rule <= Rule::Incomplete && node.site > 0 &&
                        node.site <= sites && node.edge_begin == edge_count);
                require(node.rule == Rule::Parameter
                            ? node.edge_count == 0 && node.owner.value != 0 &&
                                  scope_valid(node.owner.value, group)
                            : node.owner.value == 0);
                if (node.rule == Rule::Call || node.rule == Rule::SummaryRef)
                    require(node.payload != 0 &&
                            scope_valid(node.payload, group));
                if (node.rule == Rule::Parameter || node.rule == Rule::Call ||
                    node.rule == Rule::SummaryRef) {
                    const auto owner = node.rule == Rule::Parameter
                                           ? node.owner.value
                                           : node.payload;
                    const auto& target =
                        (owner & import_bit)
                            ? this->member(
                                  {group.imports[(owner & ~import_bit) - 1]
                                       .summary,
                                   group.imports[(owner & ~import_bit) - 1]
                                       .member,
                                   group.imports[(owner & ~import_bit) - 1]
                                       .key})
                            : group.members[owner - 1];
                    require(target.key.definition.kind ==
                            cache::DefinitionKind::Function);
                    const auto runtime_parameters = std::ranges::count(
                        target.key.arguments, cache::ArgumentStage::Runtime,
                        &cache::StageArgument::stage);
                    if (node.rule == Rule::Parameter) {
                        require(
                            site_anchors[node.site].has_value() &&
                            site_anchors[node.site]->definition ==
                                target.key.definition &&
                            site_anchors[node.site]->definition_fingerprint ==
                                target.key.definition_fingerprint);
                        require(node.payload <
                                static_cast<std::uint64_t>(runtime_parameters));
                    } else if (node.rule == Rule::Call)
                        require(node.edge_count ==
                                static_cast<std::uint64_t>(runtime_parameters));
                    else
                        require(node.edge_count == 0);
                }
                recorded_complete &= node.rule != Rule::Incomplete;
                edge_count += node.edge_count;
                require(edge_count <= limits_.elements, Error::LimitExceeded);
                detail.nodes.push_back(node);
            }
            const auto edges = checked_count(reader, 5);
            require(edges == edge_count &&
                    recorded_complete == member_value.recorded_complete);
            detail.edges.reserve(edges);
            for (std::uint32_t i = 0; i < edges && reader.good(); ++i)
                detail.edges.push_back(
                    {{reader.u32()}, static_cast<Relation>(reader.u8())});
            if (reader.good()) {
                for (std::size_t i = 0; i < detail.nodes.size(); ++i) {
                    const auto& node = detail.nodes[i];
                    for (const auto& edge :
                         std::span(detail.edges)
                             .subspan(node.edge_begin, node.edge_count)) {
                        require(edge.target.value > 0 &&
                                edge.target.value <= i &&
                                edge.relation <= Relation::Initializer);
                        if (node.rule == Rule::Call)
                            require(edge.relation == Relation::Data);
                    }
                }
            }
            details.push_back(std::move(detail));
        }
        finish(reader);
        return details;
    } catch (Error error) {
        return std::unexpected(failure(error));
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            cache::StoreError{cache::StoreErrorCode::OutOfMemory, {}, {}});
    }
}
} // namespace pagos::stage
