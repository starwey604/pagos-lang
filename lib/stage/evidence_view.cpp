#include "pagos/stage/evidence_view.h"

#include "../cache/wire.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace pagos::stage::provenance {
namespace {
using cache::Error;
namespace wire = cache::wire;
constexpr std::uint32_t record_model = 1; // P0.
EvidenceError failure(EvidenceErrorCode code) { return {code, {}, {}}; }
EvidenceError codec_failure(Error error) {
    auto result = failure(error == Error::IncompatibleVersion
                              ? EvidenceErrorCode::IncompatibleVersion
                          : error == Error::LimitExceeded
                              ? EvidenceErrorCode::LimitExceeded
                              : EvidenceErrorCode::CorruptBlock);
    result.codec = error;
    return result;
}
EvidenceError storage_failure(cache::StoreError error) {
    auto code = EvidenceErrorCode::IoFailure;
    switch (error.code) {
    case cache::StoreErrorCode::MissingObject:
    case cache::StoreErrorCode::NotFound:
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
    return {code, error, {}};
}
using Block = EvidenceBlock;
std::expected<std::shared_ptr<const Block>, Error>
decode(std::span<const std::uint8_t> bytes, const EvidenceBlockRef& reference,
       std::size_t summaries, cache::Limits limits) {
    wire::Reader reader(bytes, limits);
    wire::header(reader, cache::ObjectKind::EvidenceBlock);
    if (reader.u32() != evidence_block_version || reader.u32() != record_model)
        reader.fail(Error::IncompatibleVersion);
    if (reader.u32() != reference.first.value)
        reader.fail(Error::InvalidFormat);
    const auto count = reader.count(21);
    if (count != reference.nodes)
        reader.fail(Error::InvalidFormat);
    auto block = std::make_shared<Block>();
    block->nodes.reserve(reader.good() ? count : 0);
    std::uint64_t edge_count = 0;
    for (std::uint32_t index = 0; index < count && reader.good(); ++index) {
        Node node;
        node.rule = static_cast<Rule>(reader.u8());
        node.site = reader.u32();
        node.edge_begin = reader.u32();
        node.edge_count = reader.u32();
        node.payload = reader.u32();
        node.owner = {reader.u32()};
        if (node.rule > Rule::Incomplete || node.edge_begin != edge_count ||
            (node.rule == Rule::Parameter
                 ? !node.owner.value || node.owner.value > summaries ||
                       node.edge_count != 0
                 : node.owner.value != 0) ||
            ((node.rule == Rule::Call || node.rule == Rule::SummaryRef) &&
             (!node.payload || node.payload > summaries)))
            reader.fail(Error::InvalidFormat);
        edge_count += node.edge_count;
        if (edge_count > limits.elements)
            reader.fail(Error::LimitExceeded);
        block->nodes.push_back(node);
    }
    const auto edges = reader.count(5);
    if (edges != edge_count)
        reader.fail(Error::InvalidFormat);
    block->edges.reserve(reader.good() ? edges : 0);
    for (std::uint32_t index = 0; index < edges && reader.good(); ++index)
        block->edges.push_back(
            {{reader.u32()}, static_cast<Relation>(reader.u8())});
    if (reader.good()) {
        for (std::size_t index = 0; index < block->nodes.size(); ++index) {
            const auto& node = block->nodes[index];
            for (const auto& edge :
                 std::span(block->edges)
                     .subspan(node.edge_begin, node.edge_count)) {
                if (!edge.target.value ||
                    edge.target.value >= reference.first.value + index ||
                    edge.relation > Relation::Initializer ||
                    (node.rule == Rule::Call &&
                     edge.relation != Relation::Data))
                    reader.fail(Error::InvalidFormat);
            }
        }
    }
    if (const auto result = reader.finish(); !result)
        return std::unexpected(result.error());
    return block;
}
} // namespace

struct EvidenceView::Impl {
    struct Entry {
        EvidenceBlockRef reference;
        std::shared_ptr<const Block> cached;
        std::weak_ptr<const Block> live;
        std::size_t decoded_bytes{};
        std::uint64_t used{};
    };
    std::optional<FrozenGraph> graph;
    std::optional<cache::FileStore> store;
    Loader loader;
    std::vector<Entry> blocks;
    std::vector<SummaryDescription> summaries;
    std::size_t nodes{};
    std::size_t cache_bytes{};
    cache::Limits limits;
    mutable std::mutex mutex;
    EvidenceReadStats stats;
    std::uint64_t clock{};

    void trim() {
        while (stats.cached_bytes > cache_bytes) {
            auto oldest = blocks.end();
            for (auto entry = blocks.begin(); entry != blocks.end(); ++entry)
                if (entry->cached &&
                    (oldest == blocks.end() || entry->used < oldest->used))
                    oldest = entry;
            if (oldest == blocks.end())
                break;
            stats.cached_bytes -= oldest->decoded_bytes;
            oldest->cached.reset();
        }
    }
};
std::size_t EvidenceView::directory_bytes() const {
    return impl_->blocks.capacity() * sizeof(Impl::Entry) +
           impl_->summaries.capacity() * sizeof(SummaryDescription);
}
std::size_t EvidenceBlock::bytes() const {
    auto size = nodes.capacity() * sizeof(Node) +
                edges.capacity() * sizeof(Edge) +
                sites.capacity() * sizeof(EvidenceSite) +
                site_indices.capacity() * sizeof(std::uint32_t);
    for (const auto& site : sites) {
        // Inline string bytes already live inside the site vector's objects.
        const auto data = reinterpret_cast<std::uintptr_t>(site.label.data());
        const auto object = reinterpret_cast<std::uintptr_t>(&site.label);
        if (data < object || data >= object + sizeof(std::string))
            size += site.label.capacity() + 1;
    }
    return size;
}
std::expected<std::shared_ptr<const EvidenceView>, EvidenceError>
EvidenceView::segmented(std::vector<EvidenceBlockRef> blocks,
                        std::vector<SummaryDescription> summaries,
                        Loader loader, std::size_t cache_bytes,
                        cache::Limits limits) {
    if (!loader)
        return std::unexpected(failure(EvidenceErrorCode::InvalidReference));
    auto view = disk(cache::FileStore({}), std::move(blocks),
                     std::move(summaries), cache_bytes, limits);
    if (view)
        (*view)->impl_->loader = std::move(loader);
    return view;
}
EvidenceView::EvidenceView(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
std::shared_ptr<const EvidenceView> EvidenceView::memory(FrozenGraph graph) {
    auto impl = std::make_shared<Impl>();
    impl->nodes = graph.stats().nodes;
    impl->graph = std::move(graph);
    return std::shared_ptr<const EvidenceView>(
        new EvidenceView(std::move(impl)));
}
std::expected<std::shared_ptr<const EvidenceView>, EvidenceError>
EvidenceView::disk(cache::FileStore store, std::vector<EvidenceBlockRef> blocks,
                   std::vector<SummaryDescription> summaries,
                   std::size_t cache_bytes, cache::Limits limits) {
    try {
        if (blocks.size() > limits.elements ||
            summaries.size() > limits.elements)
            return std::unexpected(failure(EvidenceErrorCode::LimitExceeded));
        auto impl = std::make_shared<Impl>();
        impl->store = std::move(store);
        impl->cache_bytes = cache_bytes;
        impl->limits = limits;
        std::uint64_t end = 1;
        for (const auto& block : blocks) {
            if (block.first.value != end || !block.nodes ||
                (block.object.kind != cache::ObjectKind::EvidenceBlock &&
                 block.object.kind != cache::ObjectKind::EvidenceDetail))
                return std::unexpected(
                    failure(EvidenceErrorCode::InvalidReference));
            if (block.object.size > limits.bytes)
                return std::unexpected(
                    failure(EvidenceErrorCode::LimitExceeded));
            end += block.nodes;
            if (end > UINT32_MAX)
                return std::unexpected(
                    failure(EvidenceErrorCode::LimitExceeded));
            impl->blocks.push_back({block, {}, {}, 0});
        }
        impl->nodes = static_cast<std::size_t>(end - 1);
        // Ownership was transferred by the caller. References now live in the
        // cache entries; release the redundant directory before later work.
        std::vector<EvidenceBlockRef>{}.swap(blocks);
        for (std::size_t index = 0; index < summaries.size(); ++index) {
            const auto& summary = summaries[index];
            if (summary.id.value != index + 1 || !summary.value.value ||
                summary.value.value >= end || !summary.effects.value ||
                summary.effects.value >= end)
                return std::unexpected(
                    failure(EvidenceErrorCode::InvalidReference));
        }
        impl->summaries = std::move(summaries);
        return std::shared_ptr<const EvidenceView>(
            new EvidenceView(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(EvidenceErrorCode::OutOfMemory));
    }
}
std::expected<NodeLease, EvidenceError> EvidenceView::pin(NodeId id) const {
    if (!id.value || id.value > impl_->nodes)
        return std::unexpected(failure(EvidenceErrorCode::InvalidReference));
    if (impl_->graph)
        return NodeLease(impl_, &impl_->graph->node(id),
                         impl_->graph->edges(id));
    try {
        // Serialize cold reads and cache publication. Independent tasks share
        // immutable blocks; no buffer can disappear while its lease is alive.
        const std::lock_guard lock(impl_->mutex);
        ++impl_->stats.requests;
        auto found = std::ranges::upper_bound(
            impl_->blocks, id.value, {}, [](const Impl::Entry& entry) {
                return entry.reference.first.value;
            });
        auto& entry = *--found;
        auto block = entry.live.lock();
        if (block)
            ++impl_->stats.cache_hits;
        else if (impl_->loader) {
            auto loaded = impl_->loader(
                static_cast<std::size_t>(&entry - impl_->blocks.data()));
            if (!loaded)
                return std::unexpected(loaded.error());
            block = std::move(*loaded);
            if (!block || block->nodes.size() != entry.reference.nodes ||
                (!block->site_indices.empty() &&
                 block->site_indices.size() != block->nodes.size()))
                return std::unexpected(
                    failure(EvidenceErrorCode::CorruptBlock));
            for (auto site : block->site_indices)
                if (site >= block->sites.size())
                    return std::unexpected(
                        failure(EvidenceErrorCode::CorruptBlock));
            if (entry.reference.disk_backed) {
                ++impl_->stats.objects_read;
                impl_->stats.payload_bytes_read +=
                    static_cast<std::size_t>(entry.reference.object.size);
            }
            entry.live = block;
            entry.decoded_bytes = block->bytes();
        } else {
            auto bytes = impl_->store->read_object(entry.reference.object);
            if (!bytes)
                return std::unexpected(storage_failure(bytes.error()));
            ++impl_->stats.objects_read;
            impl_->stats.payload_bytes_read += bytes->size();
            auto decoded = decode(*bytes, entry.reference,
                                  impl_->summaries.size(), impl_->limits);
            if (!decoded)
                return std::unexpected(codec_failure(decoded.error()));
            block = std::move(*decoded);
            entry.live = block;
            entry.decoded_bytes = block->bytes();
        }
        entry.used = ++impl_->clock;
        if (!entry.cached) {
            entry.cached = block;
            impl_->stats.cached_bytes += entry.decoded_bytes;
        }
        impl_->trim();
        const auto& node = block->nodes[id.value - entry.reference.first.value];
        const auto* site =
            block->site_indices.empty()
                ? nullptr
                : &block->sites[block->site_indices
                                    [id.value - entry.reference.first.value]];
        return NodeLease(
            block, &node,
            std::span(block->edges).subspan(node.edge_begin, node.edge_count),
            site);
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(EvidenceErrorCode::OutOfMemory));
    }
}
std::size_t EvidenceView::node_count() const { return impl_->nodes; }
std::span<const SummaryDescription> EvidenceView::summaries() const {
    return impl_->graph ? impl_->graph->summaries()
                        : std::span(impl_->summaries);
}
const SummaryDescription& EvidenceView::summary(SummaryId id) const {
    const auto entries = summaries();
    if (!id.value || id.value > entries.size())
        throw std::out_of_range("invalid evidence summary");
    return entries[id.value - 1];
}
const FrozenGraph* EvidenceView::memory_graph() const {
    return impl_->graph ? &*impl_->graph : nullptr;
}
EvidenceReadStats EvidenceView::read_stats() const {
    const std::lock_guard lock(impl_->mutex);
    auto stats = impl_->stats;
    for (const auto& entry : impl_->blocks)
        if (const auto block = entry.live.lock())
            stats.live_bytes += entry.decoded_bytes;
    return stats;
}
void EvidenceView::evict() const {
    const std::lock_guard lock(impl_->mutex);
    for (auto& entry : impl_->blocks)
        entry.cached.reset();
    impl_->stats.cached_bytes = 0;
}
std::expected<cache::Bytes, Error>
encode_evidence_block(const EvidenceBlock& block, NodeId first,
                      std::size_t summaries, cache::Limits limits) {
    if (!first.value || block.nodes.empty() ||
        block.nodes.size() > UINT32_MAX - first.value || !block.sites.empty() ||
        !block.site_indices.empty())
        return std::unexpected(Error::InvalidFormat);
    wire::Writer writer(limits);
    wire::header(writer, cache::ObjectKind::EvidenceBlock);
    writer.u32(evidence_block_version);
    writer.u32(record_model);
    writer.u32(first.value);
    writer.count(block.nodes.size());
    std::size_t offset = 0;
    for (std::size_t index = 0; index < block.nodes.size(); ++index) {
        const auto& node = block.nodes[index];
        if (node.rule > Rule::Incomplete || node.edge_begin != offset ||
            node.edge_count > block.edges.size() - offset ||
            (node.rule == Rule::Parameter
                 ? !node.owner.value || node.owner.value > summaries ||
                       node.edge_count != 0
                 : node.owner.value != 0) ||
            ((node.rule == Rule::Call || node.rule == Rule::SummaryRef) &&
             (!node.payload || node.payload > summaries)))
            return std::unexpected(Error::InvalidFormat);
        for (const auto& edge :
             std::span(block.edges).subspan(node.edge_begin, node.edge_count))
            if (!edge.target.value ||
                edge.target.value >= first.value + index ||
                edge.relation > Relation::Initializer ||
                (node.rule == Rule::Call && edge.relation != Relation::Data))
                return std::unexpected(Error::InvalidFormat);
        offset += node.edge_count;
        writer.u8(static_cast<std::uint8_t>(node.rule));
        writer.u32(node.site);
        writer.u32(node.edge_begin);
        writer.u32(node.edge_count);
        writer.u32(node.payload);
        writer.u32(node.owner.value);
    }
    if (offset != block.edges.size())
        return std::unexpected(Error::InvalidFormat);
    writer.count(block.edges.size());
    for (const auto& edge : block.edges) {
        writer.u32(edge.target.value);
        writer.u8(static_cast<std::uint8_t>(edge.relation));
    }
    return writer.finish();
}
std::expected<std::shared_ptr<const EvidenceBlock>, Error>
decode_evidence_block(std::span<const std::uint8_t> bytes,
                      const EvidenceBlockRef& reference, std::size_t summaries,
                      cache::Limits limits) {
    return decode(bytes, reference, summaries, limits);
}
std::expected<cache::Bytes, Error>
encode_evidence_block(const FrozenGraph& graph, NodeId first,
                      std::uint32_t nodes, cache::Limits limits) {
    if (!first.value || !nodes ||
        static_cast<std::uint64_t>(first.value) + nodes - 1 >
            graph.stats().nodes)
        return std::unexpected(Error::InvalidFormat);
    wire::Writer writer(limits);
    wire::header(writer, cache::ObjectKind::EvidenceBlock);
    writer.u32(evidence_block_version);
    writer.u32(record_model);
    writer.u32(first.value);
    writer.count(nodes);
    std::uint64_t edges = 0;
    for (std::uint32_t index = 0; index < nodes; ++index) {
        const auto& node = graph.node({first.value + index});
        writer.u8(static_cast<std::uint8_t>(node.rule));
        writer.u32(node.site);
        if (edges > UINT32_MAX)
            writer.fail(Error::LimitExceeded);
        writer.u32(static_cast<std::uint32_t>(edges));
        writer.u32(node.edge_count);
        writer.u32(node.payload);
        writer.u32(node.owner.value);
        edges += node.edge_count;
    }
    writer.count(static_cast<std::size_t>(edges));
    for (std::uint32_t index = 0; index < nodes; ++index)
        for (const auto& edge : graph.edges({first.value + index})) {
            writer.u32(edge.target.value);
            writer.u8(static_cast<std::uint8_t>(edge.relation));
        }
    return writer.finish();
}
} // namespace pagos::stage::provenance
