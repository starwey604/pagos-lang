#include "pagos/stage/provenance_snapshot.h"
#include "../cache/wire.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>

namespace pagos::stage {
namespace {
using namespace provenance;
using cache::Error;
using cache::wire::Reader;
using cache::wire::Writer;
constexpr std::size_t header_size = 9 + 4 + 4 + 32 + 32 + 4;
constexpr std::size_t directory_record_size = 4 + 1 + 8 + 8;
cache::Digest digest(Reader& reader) {
    cache::Digest result;
    const auto bytes = reader.raw(32);
    if (reader.good())
        std::ranges::copy(bytes, result.bytes.begin());
    return result;
}
void blob(Writer& writer, std::span<const std::uint8_t> bytes) {
    writer.count(bytes.size());
    writer.raw(bytes);
}
std::span<const std::uint8_t> blob(Reader& reader) {
    return reader.raw(reader.count());
}
bool stage_valid(hir::Stage stage) {
    return stage == hir::Stage::Comptime || stage == hir::Stage::Runtime;
}
struct Atlas {
    const CacheIdentity& identity;
    std::map<std::pair<std::size_t, std::size_t>, const AnchoredSpan*>
        available;
    std::map<std::pair<std::size_t, std::size_t>, std::uint32_t> span_ids{
        {{0, 0}, 0}};
    explicit Atlas(const CacheIdentity& input) : identity(input) {
        for (const auto& site : input.sites)
            available.try_emplace(std::pair{site.span.begin, site.span.end},
                                  &site);
    }
    std::vector<SourceAnchor> anchors;
    std::vector<source::Span> spans{{}};
    std::vector<std::string> strings;
    std::unordered_map<std::string, std::uint32_t> string_ids;
    std::uint32_t text(const std::string& value) {
        if (const auto found = string_ids.find(value);
            found != string_ids.end())
            return found->second;
        if (strings.size() >= UINT32_MAX)
            throw std::length_error("snapshot strings");
        const auto id = static_cast<std::uint32_t>(strings.size());
        strings.push_back(value);
        string_ids.emplace(value, id);
        return id;
    }
    std::uint32_t span(source::Span value) {
        const auto key = std::pair{value.begin, value.end};
        if (const auto found = span_ids.find(key); found != span_ids.end())
            return found->second;
        const auto found = available.find(key);
        if (found == available.end())
            throw Error::InvalidAnchor;
        if (spans.size() >= UINT32_MAX)
            throw std::length_error("snapshot spans");
        anchors.push_back(found->second->anchor);
        spans.push_back(value);
        const auto id = static_cast<std::uint32_t>(spans.size() - 1);
        span_ids.emplace(key, id);
        return id;
    }
};
cache::StoreError store_error(Error error) {
    return {error == Error::IncompatibleVersion
                ? cache::StoreErrorCode::IncompatibleVersion
            : error == Error::LimitExceeded
                ? cache::StoreErrorCode::LimitExceeded
                : cache::StoreErrorCode::CorruptObject,
            {},
            error};
}
} // namespace
cache::QueryKey
ProvenanceSnapshot::key_for(const CacheIdentity& identity,
                            const cache::QueryContext& context) {
    return {cache::QueryKind::FreezeEvidence,
            context,
            {identity.module, cache::DefinitionKind::Module, "$module"},
            identity.module_fingerprint,
            {}};
}
std::expected<SnapshotArtifacts, Error> ProvenanceSnapshot::encode(
    const ProvenanceAnalysis& analysis, const CacheIdentity& identity,
    const cache::QueryContext& context, const source::SourceFile& source,
    cache::Limits limits) {
    try {
        if (!analysis.available() ||
            (analysis.spilled() && !analysis.validate_details()))
            return std::unexpected(Error::InvalidContext);
        const auto key = key_for(identity, context);
        const auto encoded_key = cache::encode_query_key(key, limits);
        if (!encoded_key)
            return std::unexpected(encoded_key.error());
        if (context.module != identity.module)
            return std::unexpected(Error::InvalidContext);
        Atlas atlas(identity);
        std::vector<std::pair<EvidenceSection, cache::Bytes>> sections;
        const auto save = [&](EvidenceSection kind, Writer& writer) {
            auto bytes = writer.finish();
            if (!bytes)
                throw bytes.error();
            sections.emplace_back(kind, std::move(*bytes));
        };
        Writer ident(limits);
        blob(ident, *encoded_key);
        save(EvidenceSection::Identity, ident);
        Writer roots(limits);
        roots.count(analysis.roots().size());
        for (const auto& root : analysis.roots()) {
            roots.u32(atlas.text(root.name));
            roots.u32(atlas.span(root.span));
            roots.u8(static_cast<std::uint8_t>(root.stage));
            roots.u32(root.node.value);
            roots.boolean(root.requires_comptime);
            roots.u32(root.scope.value);
        }
        save(EvidenceSection::Root, roots);
        Writer summaries(limits);
        summaries.count(analysis.scopes().size());
        for (const auto& scope : analysis.scopes()) {
            summaries.u32(scope.id.value);
            summaries.u32(atlas.text(scope.name));
            const auto port = scope.id.value
                                  ? analysis.graph().summary(scope.id)
                                  : SummaryDescription{};
            summaries.u32(port.value.value);
            summaries.u32(port.effects.value);
        }
        save(EvidenceSection::Summary, summaries);
        Writer nodes(limits), edges(limits);
        const auto stats = analysis.graph().stats();
        nodes.count(stats.nodes);
        edges.count(stats.edges);
        std::size_t edge_begin = 0;
        for (std::uint32_t id = 1; id <= stats.nodes; ++id) {
            const auto& node = analysis.graph().node({id});
            const auto dependencies = analysis.graph().edges({id});
            nodes.u8(static_cast<std::uint8_t>(node.rule));
            nodes.u32(node.site);
            nodes.count(edge_begin);
            nodes.count(dependencies.size());
            nodes.u32(node.payload);
            nodes.u32(node.owner.value);
            edge_begin += dependencies.size();
            for (const auto& edge : dependencies) {
                edges.u32(edge.target.value);
                edges.u8(static_cast<std::uint8_t>(edge.relation));
            }
        }
        if (edge_begin != stats.edges)
            throw Error::InvalidValue;
        save(EvidenceSection::Node, nodes);
        save(EvidenceSection::Edge, edges);
        Writer decisions(limits);
        decisions.count(analysis.decisions().size());
        for (const auto& decision : analysis.decisions()) {
            decisions.u32(decision.node.value);
            decisions.u8(static_cast<std::uint8_t>(decision.result));
            decisions.u8(static_cast<std::uint8_t>(decision.rule));
            decisions.u32(atlas.span(decision.span));
            decisions.u8(static_cast<std::uint8_t>(decision.reason));
            decisions.boolean(decision.member.has_value());
            if (decision.member)
                decisions.u32(*decision.member);
            decisions.u32(decision.scope.value);
        }
        save(EvidenceSection::Decision, decisions);
        Writer sites(limits);
        sites.count(analysis.sites().size());
        for (const auto& site : analysis.sites()) {
            sites.u32(atlas.span(site.span));
            sites.u32(atlas.text(site.label));
        }
        sites.count(atlas.anchors.size());
        for (const auto& anchor : atlas.anchors) {
            const auto encoded = encode_source_anchor(anchor, limits);
            if (!encoded)
                throw encoded.error();
            blob(sites, *encoded);
        }
        save(EvidenceSection::Site, sites);
        Writer strings(limits);
        strings.count(atlas.strings.size());
        for (const auto& item : atlas.strings)
            strings.string(item);
        save(EvidenceSection::String, strings);
        Writer graph(limits);
        cache::wire::header(graph, cache::ObjectKind::ProvenanceSnapshot);
        graph.u32(provenance_format_version);
        graph.u32(provenance_record_model);
        graph.raw(identity.module_fingerprint.bytes);
        graph.raw(cache::fingerprint("pagos.snapshot-context.v1", *encoded_key)
                      .bytes);
        graph.count(sections.size());
        std::uint64_t offset =
            header_size + directory_record_size * sections.size();
        for (const auto& [kind, bytes] : sections) {
            graph.u32(static_cast<std::uint32_t>(kind));
            graph.boolean(true);
            graph.u64(offset);
            graph.u64(bytes.size());
            offset += bytes.size();
        }
        for (const auto& [kind, bytes] : sections) {
            (void)kind;
            graph.raw(bytes);
        }
        auto bytes = graph.finish();
        if (!bytes)
            throw bytes.error();
        const auto semantic =
            cache::fingerprint("pagos.provenance.snapshot.v1", *bytes);
        if (limits.bytes < 32 || bytes->size() > limits.bytes - 32)
            throw Error::LimitExceeded;
        bytes->insert(bytes->end(), semantic.bytes.begin(),
                      semantic.bytes.end());
        Writer positions(limits);
        cache::wire::header(positions, cache::ObjectKind::ProvenancePositions);
        positions.u32(provenance_format_version);
        positions.raw(semantic.bytes);
        positions.raw(cache::fingerprint("pagos.source-revision.v1",
                                         {reinterpret_cast<const std::uint8_t*>(
                                              source.text().data()),
                                          source.text().size()})
                          .bytes);
        positions.string(source.path());
        positions.count(atlas.spans.size());
        for (auto span : atlas.spans) {
            positions.u64(span.begin);
            positions.u64(span.end);
        }
        auto mapping = positions.finish();
        if (!mapping)
            throw mapping.error();
        return SnapshotArtifacts{key, std::move(*bytes), std::move(*mapping),
                                 semantic};
    } catch (const EvidenceUnavailable&) {
        return std::unexpected(Error::InvalidContext);
    } catch (Error error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error::LimitExceeded);
    } catch (const std::length_error&) {
        return std::unexpected(Error::LimitExceeded);
    }
}
std::expected<ProvenanceSnapshot, Error>
ProvenanceSnapshot::open(cache::Bytes bytes, cache::Limits limits) {
    try {
        if (bytes.size() > limits.bytes)
            return std::unexpected(Error::LimitExceeded);
        if (bytes.size() < header_size + 32)
            return std::unexpected(Error::InvalidFormat);
        const auto body = std::span(bytes).first(bytes.size() - 32);
        const auto checksum =
            cache::fingerprint("pagos.provenance.snapshot.v1", body);
        if (!std::ranges::equal(checksum.bytes, std::span(bytes).last(32)))
            return std::unexpected(Error::InvalidFormat);
        Reader reader(body, limits);
        cache::wire::header(reader, cache::ObjectKind::ProvenanceSnapshot);
        if (reader.u32() != provenance_format_version ||
            reader.u32() != provenance_record_model)
            reader.fail(Error::IncompatibleVersion);
        const auto semantic = digest(reader);
        const auto context_hash = digest(reader);
        const auto count = reader.count(directory_record_size);
        ProvenanceSnapshot result;
        result.limits_ = limits;
        std::uint64_t previous = header_size + directory_record_size * count;
        std::array<bool, 9> seen{};
        for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
            const auto kind = reader.u32();
            const auto required = reader.boolean();
            const auto offset = reader.u64();
            const auto size = reader.u64();
            if (offset < previous || offset > body.size() ||
                size > body.size() - offset)
                reader.fail(Error::InvalidFormat);
            if (!kind || kind > 8) {
                if (required)
                    reader.fail(Error::IncompatibleVersion);
            } else {
                if (seen[kind] || !required)
                    reader.fail(Error::InvalidFormat);
                seen[kind] = true;
            }
            previous = offset + size;
            result.sections_.push_back(
                {static_cast<EvidenceSection>(kind), required, offset, size});
        }
        for (unsigned i = 1; i <= 8; ++i)
            if (!seen[i])
                reader.fail(Error::InvalidFormat);
        (void)reader.raw(reader.remaining());
        if (auto status = reader.finish(); !status)
            return std::unexpected(status.error());
        result.bytes_ = std::make_shared<const cache::Bytes>(std::move(bytes));
        Reader key_reader(result.section(EvidenceSection::Identity), limits);
        auto decoded_key = cache::decode_query_key(blob(key_reader), limits);
        if (!decoded_key)
            return std::unexpected(decoded_key.error());
        if (auto status = key_reader.finish(); !status)
            return std::unexpected(status.error());
        result.key_ = std::move(*decoded_key);
        const auto canonical = cache::encode_query_key(result.key_, limits);
        if (!canonical ||
            result.key_.kind != cache::QueryKind::FreezeEvidence ||
            result.key_.definition.kind != cache::DefinitionKind::Module ||
            result.key_.definition.name != "$module" ||
            result.key_.definition_fingerprint != semantic ||
            cache::fingerprint("pagos.snapshot-context.v1", *canonical) !=
                context_hash)
            return std::unexpected(Error::InvalidContext);
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error::LimitExceeded);
    }
}
std::span<const EvidenceSectionInfo> ProvenanceSnapshot::sections() const {
    return sections_;
}
std::span<const std::uint8_t>
ProvenanceSnapshot::section(EvidenceSection kind) const {
    const auto found =
        std::ranges::find(sections_, kind, &EvidenceSectionInfo::kind);
    return found == sections_.end()
               ? std::span<const std::uint8_t>{}
               : std::span(*bytes_).subspan(
                     static_cast<std::size_t>(found->offset),
                     static_cast<std::size_t>(found->size));
}
std::expected<std::shared_ptr<const ProvenanceAnalysis>, Error>
ProvenanceSnapshot::restore(const CacheIdentity& identity,
                            const cache::QueryContext& context) const {
    try {
        if (key_ != key_for(identity, context))
            return std::unexpected(Error::InvalidContext);
        const auto require = [](Reader& reader) {
            if (auto status = reader.finish(); !status)
                throw status.error();
        };
        Reader text_reader(section(EvidenceSection::String), limits_);
        const auto text_count = text_reader.count(4);
        std::vector<std::string> strings;
        for (std::uint32_t i = 0; i < text_count && text_reader.good(); ++i)
            strings.push_back(text_reader.string());
        require(text_reader);
        const auto text = [&](std::uint32_t id) -> const std::string& {
            if (id >= strings.size())
                throw Error::InvalidFormat;
            return strings[id];
        };
        Reader site_reader(section(EvidenceSection::Site), limits_);
        const auto site_count = site_reader.count(8);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> site_refs;
        for (std::uint32_t i = 0; i < site_count && site_reader.good(); ++i) {
            const auto span = site_reader.u32();
            const auto label = site_reader.u32();
            site_refs.emplace_back(span, label);
        }
        using AnchorKey = std::tuple<std::uint8_t, std::string, std::uint32_t>;
        std::map<AnchorKey, const AnchoredSpan*> locations;
        for (const auto& location : identity.sites)
            locations.try_emplace(
                AnchorKey{
                    static_cast<std::uint8_t>(location.anchor.definition.kind),
                    location.anchor.definition.name, location.anchor.position},
                &location);
        const auto anchor_count = site_reader.count(4);
        std::vector<source::Span> spans{{}};
        std::vector<SourceAnchor> decoded_anchors;
        for (std::uint32_t i = 0; i < anchor_count && site_reader.good(); ++i) {
            const auto anchor =
                decode_source_anchor(blob(site_reader), limits_);
            if (!anchor)
                throw anchor.error();
            const auto found = locations.find(
                AnchorKey{static_cast<std::uint8_t>(anchor->definition.kind),
                          anchor->definition.name, anchor->position});
            if (found == locations.end() || found->second->anchor != *anchor)
                throw Error::InvalidAnchor;
            spans.push_back(found->second->span);
            decoded_anchors.push_back(*anchor);
        }
        require(site_reader);
        const auto span = [&](std::uint32_t id) {
            if (id >= spans.size())
                throw Error::InvalidAnchor;
            return spans[id];
        };
        std::vector<ProvenanceSite> sites;
        sites.reserve(site_refs.size());
        for (const auto& [position, label] : site_refs)
            sites.push_back({span(position), text(label)});
        if (sites.empty())
            throw Error::InvalidFormat;
        Reader node_reader(section(EvidenceSection::Node), limits_);
        const auto node_count = node_reader.count(21);
        Reader edge_reader(section(EvidenceSection::Edge), limits_);
        const auto edge_count = edge_reader.count(5);
        std::vector<Edge> edges;
        for (std::uint32_t i = 0; i < edge_count && edge_reader.good(); ++i) {
            const auto target = edge_reader.u32();
            const auto relation = static_cast<Relation>(edge_reader.u8());
            if (!target || target > node_count ||
                relation > Relation::Initializer)
                edge_reader.fail(Error::InvalidFormat);
            edges.push_back({{target}, relation});
        }
        require(edge_reader);
        Reader summary_reader(section(EvidenceSection::Summary), limits_);
        const auto scope_count = summary_reader.count(16);
        if (!scope_count)
            throw Error::InvalidFormat;
        Builder builder;
        std::vector<ProvenanceScope> scopes;
        std::vector<NodeId> ports;
        for (std::uint32_t i = 0; i < scope_count && summary_reader.good();
             ++i) {
            const auto id = summary_reader.u32();
            const auto name = summary_reader.u32();
            const auto value = summary_reader.u32();
            const auto effects = summary_reader.u32();
            if (id != i || value != effects ||
                (i ? !value || value > node_count : value != 0))
                summary_reader.fail(Error::InvalidFormat);
            if (i && builder.reserve_summary().value != i)
                throw Error::InvalidFormat;
            scopes.push_back({{id}, text(name), {}, {}});
            ports.push_back({value});
        }
        require(summary_reader);
        std::size_t next_edge = 0;
        for (std::uint32_t i = 1; i <= node_count && node_reader.good(); ++i) {
            const auto rule = static_cast<Rule>(node_reader.u8());
            const auto site = node_reader.u32();
            const auto begin = node_reader.u32();
            const auto count = node_reader.u32();
            const auto payload = node_reader.u32();
            const auto owner = node_reader.u32();
            if (rule > Rule::Incomplete || site >= sites.size() ||
                begin != next_edge || count > edges.size() - next_edge ||
                owner >= scope_count ||
                (rule == Rule::Parameter ? !owner : owner != 0) ||
                ((rule == Rule::Call || rule == Rule::SummaryRef) &&
                 (!payload || payload >= scope_count)))
                throw Error::InvalidFormat;
            if (rule == Rule::Parameter) {
                const auto anchor_id = site_refs[site].first;
                if (!anchor_id ||
                    decoded_anchors[anchor_id - 1].definition.kind !=
                        cache::DefinitionKind::Function ||
                    decoded_anchors[anchor_id - 1].definition.name !=
                        scopes[owner].name)
                    throw Error::InvalidFormat;
            }
            const auto dependencies =
                std::span(edges).subspan(next_edge, count);
            for (const auto& edge : dependencies)
                if (edge.target.value >= i)
                    throw Error::InvalidFormat;
            if (rule == Rule::Call)
                for (const auto& edge : dependencies)
                    if (edge.relation != Relation::Data)
                        throw Error::InvalidFormat;
            if (builder.add(rule, site, dependencies, payload, {owner}).value !=
                i)
                throw Error::InvalidFormat;
            next_edge += count;
        }
        require(node_reader);
        if (next_edge != edges.size())
            throw Error::InvalidFormat;
        for (std::uint32_t i = 1; i < scope_count; ++i)
            builder.define_summary({i}, ports[i]);
        Reader root_reader(section(EvidenceSection::Root), limits_);
        const auto root_count = root_reader.count(18);
        std::vector<ProvenanceRoot> roots;
        for (std::uint32_t i = 0; i < root_count && root_reader.good(); ++i) {
            const auto name = root_reader.u32();
            const auto location = root_reader.u32();
            const auto stage = static_cast<hir::Stage>(root_reader.u8());
            const auto node = root_reader.u32();
            const auto required = root_reader.boolean();
            const auto owner = root_reader.u32();
            if (!stage_valid(stage) || !node || node > node_count ||
                owner >= scope_count)
                throw Error::InvalidFormat;
            if (required)
                scopes[owner].contracts.push_back(roots.size());
            roots.push_back(
                {text(name), span(location), stage, {node}, required, {owner}});
        }
        require(root_reader);
        Reader decision_reader(section(EvidenceSection::Decision), limits_);
        const auto decision_count = decision_reader.count(16);
        std::vector<StageDecision> decisions;
        std::uint32_t previous = 0;
        for (std::uint32_t i = 0; i < decision_count && decision_reader.good();
             ++i) {
            const auto node = decision_reader.u32();
            const auto stage = static_cast<hir::Stage>(decision_reader.u8());
            const auto rule = static_cast<StageRule>(decision_reader.u8());
            const auto location = decision_reader.u32();
            const auto reason = static_cast<StageReason>(decision_reader.u8());
            const auto has_member = decision_reader.boolean();
            const auto member = has_member
                                    ? std::optional(decision_reader.u32())
                                    : std::nullopt;
            const auto owner = decision_reader.u32();
            if (!node || node <= previous || node > node_count ||
                !stage_valid(stage) || rule > StageRule::ConservativeFallback ||
                reason > StageReason::RecursiveCompletion ||
                owner >= scope_count)
                throw Error::InvalidFormat;
            previous = node;
            scopes[owner].decisions.push_back({node});
            decisions.push_back(
                {{node}, stage, rule, span(location), reason, member, {owner}});
        }
        require(decision_reader);
        return std::shared_ptr<const ProvenanceAnalysis>(new ProvenanceAnalysis(
            std::move(builder).freeze(), std::move(sites), std::move(roots),
            std::move(decisions), std::move(scopes)));
    } catch (Error error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error::LimitExceeded);
    } catch (const std::exception&) {
        return std::unexpected(Error::InvalidFormat);
    }
}
std::expected<void, Error>
ProvenanceSnapshot::validate_positions(std::span<const std::uint8_t> bytes,
                                       const cache::Digest& semantic,
                                       cache::Limits limits) {
    Reader reader(bytes, limits);
    cache::wire::header(reader, cache::ObjectKind::ProvenancePositions);
    if (reader.u32() != provenance_format_version)
        reader.fail(Error::IncompatibleVersion);
    if (digest(reader) != semantic)
        reader.fail(Error::InvalidContext);
    (void)digest(reader);
    (void)reader.string();
    const auto count = reader.count(16);
    for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
        const auto begin = reader.u64();
        const auto end = reader.u64();
        if (begin > end)
            reader.fail(Error::InvalidAnchor);
    }
    return reader.finish();
}
std::expected<cache::PublishResult, cache::StoreError>
publish_snapshot(const cache::FileStore& store,
                 const SnapshotArtifacts& artifacts) {
    const auto opened = ProvenanceSnapshot::open(artifacts.graph);
    if (!opened)
        return std::unexpected(store_error(opened.error()));
    if (opened->key() != artifacts.key || artifacts.graph.size() < 32 ||
        cache::fingerprint(
            "pagos.provenance.snapshot.v1",
            std::span(artifacts.graph).first(artifacts.graph.size() - 32)) !=
            artifacts.semantic)
        return std::unexpected(store_error(Error::InvalidValue));
    if (const auto position = ProvenanceSnapshot::validate_positions(
            artifacts.positions, artifacts.semantic);
        !position)
        return std::unexpected(store_error(position.error()));
    const std::array inputs{cache::ArtifactInput{
        "graph", cache::ObjectKind::ProvenanceSnapshot, artifacts.graph}};
    const auto graph =
        store.publish(artifacts.key, artifacts.key.definition_fingerprint,
                      artifacts.semantic, inputs);
    if (!graph)
        return std::unexpected(graph.error());
    auto position_key = artifacts.key;
    position_key.kind = cache::QueryKind::FreezeEvidencePositions;
    position_key.definition.name = "$module.positions";
    position_key.definition_fingerprint = artifacts.semantic;
    const auto revision =
        cache::fingerprint("pagos.positions-revision.v1", artifacts.positions);
    const std::array positions{cache::ArtifactInput{
        "positions", cache::ObjectKind::ProvenancePositions,
        artifacts.positions}};
    const auto mapping =
        store.publish(position_key, revision, revision, positions);
    if (!mapping)
        return std::unexpected(mapping.error());
    return *graph;
}
std::expected<std::shared_ptr<const ProvenanceAnalysis>, cache::StoreError>
load_snapshot(const cache::FileStore& store, const CacheIdentity& identity,
              const cache::QueryContext& context) {
    auto candidate =
        store.lookup(ProvenanceSnapshot::key_for(identity, context),
                     identity.module_fingerprint);
    if (!candidate)
        return std::unexpected(candidate.error());
    for (auto& artifact : candidate->artifacts)
        if (artifact.name == "graph" &&
            artifact.object.kind == cache::ObjectKind::ProvenanceSnapshot &&
            artifact.bytes && artifact.required) {
            const auto body = std::span(*artifact.bytes)
                                  .first(artifact.bytes->size() >= 32
                                             ? artifact.bytes->size() - 32
                                             : 0);
            if (cache::fingerprint("pagos.provenance.snapshot.v1", body) !=
                candidate->semantic_result)
                return std::unexpected(store_error(Error::InvalidValue));
            auto snapshot =
                ProvenanceSnapshot::open(std::move(*artifact.bytes));
            if (!snapshot)
                return std::unexpected(store_error(snapshot.error()));
            auto result = snapshot->restore(identity, context);
            if (!result)
                return std::unexpected(store_error(result.error()));
            return *result;
        }
    return std::unexpected(store_error(Error::InvalidFormat));
}
} // namespace pagos::stage
