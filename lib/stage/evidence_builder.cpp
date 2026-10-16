#include "pagos/stage/evidence_builder.h"

#include "evidence_session.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>

namespace pagos::stage::provenance {
namespace {
using Block = EvidenceBlock;
namespace fs = std::filesystem;
constexpr std::size_t envelope_bytes = 49;
EvidenceError error(EvidenceErrorCode code) { return {code, {}, {}}; }
EvidenceError codec_error(cache::Error code) {
    return {code == cache::Error::LimitExceeded
                ? EvidenceErrorCode::LimitExceeded
            : code == cache::Error::IncompatibleVersion
                ? EvidenceErrorCode::IncompatibleVersion
                : EvidenceErrorCode::CorruptBlock,
            {},
            code};
}
EvidenceError store_error(cache::StoreError value) {
    auto code = EvidenceErrorCode::IoFailure;
    switch (value.code) {
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
    return {code, value, value.codec};
}
std::uint32_t checked_id(std::size_t value) {
    if (value >= UINT32_MAX - 1U)
        throw std::length_error("provenance ID capacity exhausted");
    return static_cast<std::uint32_t>(value);
}
struct Ledger {
    std::atomic<std::size_t> live{};
};
struct Tracked {
    std::shared_ptr<const Block> block;
    std::shared_ptr<Ledger> ledger;
    std::size_t bytes;
    Tracked(std::shared_ptr<const Block> value, std::shared_ptr<Ledger> owner)
        : block(std::move(value)), ledger(std::move(owner)),
          bytes(block->bytes()) {
        ledger->live.fetch_add(bytes, std::memory_order_relaxed);
    }
    ~Tracked() { ledger->live.fetch_sub(bytes, std::memory_order_relaxed); }
};
std::shared_ptr<const Block> track(std::shared_ptr<const Block> block,
                                   const std::shared_ptr<Ledger>& ledger) {
    auto owner = std::make_shared<Tracked>(std::move(block), ledger);
    const auto* pointer = owner->block.get();
    return {std::move(owner), pointer};
}
std::size_t capacity(std::size_t current, std::size_t needed) {
    return needed <= current ? current
                             : std::max(needed, current ? current * 2 : 1);
}
} // namespace

struct EvidenceBuilder::Impl {
    explicit Impl(EvidenceBuildConfig options) : config(std::move(options)) {}
    struct Segment {
        EvidenceBlockRef reference;
        std::shared_ptr<const Block> resident, cached;
        std::weak_ptr<const Block> live;
        std::size_t bytes{};
        std::uint64_t used{};
    };
    EvidenceBuildConfig config;
    std::shared_ptr<EvidenceSession> session;
    std::shared_ptr<Ledger> ledger{std::make_shared<Ledger>()};
    std::shared_ptr<Block> active{std::make_shared<Block>()};
    std::vector<Segment> segments;
    std::vector<SummaryDescription> summaries;
    EvidenceBuildStats totals;
    std::optional<EvidenceError> failure, spill_error;
    std::uint64_t clock{};
    std::size_t owned() const {
        return active->bytes() + totals.resident_bytes + totals.cached_bytes;
    }
    void measure() {
        totals.peak_owned_bytes = std::max(totals.peak_owned_bytes, owned());
        totals.peak_live_bytes = std::max(
            totals.peak_live_bytes,
            active->bytes() + ledger->live.load(std::memory_order_relaxed));
    }
    void fail(EvidenceError value) {
        if (!failure)
            failure = value;
        // Already issued leases retain their buffers. A failed builder cannot
        // freeze or publish, so retaining its graph buffers serves no purpose.
        std::vector<Node>{}.swap(active->nodes);
        std::vector<Edge>{}.swap(active->edges);
        for (auto& segment : segments) {
            segment.resident.reset();
            segment.cached.reset();
        }
        totals.resident_bytes = totals.cached_bytes = 0;
    }
    void evict_one() {
        auto oldest = segments.end();
        for (auto it = segments.begin(); it != segments.end(); ++it)
            if (it->cached &&
                (oldest == segments.end() || it->used < oldest->used))
                oldest = it;
        if (oldest != segments.end()) {
            totals.cached_bytes -= oldest->bytes;
            oldest->cached.reset();
        }
    }
    void room(std::size_t extra) {
        while (totals.cached_bytes && (extra > config.resident_bytes ||
                                       owned() > config.resident_bytes - extra))
            evict_one();
    }
    std::expected<void, EvidenceError> start_session() {
        if (session)
            return {};
        if (config.store.fault)
            if (auto ec = config.store.fault(
                    cache::IoOperation::CreateDirectories, config.directory))
                return std::unexpected(
                    store_error({cache::StoreErrorCode::IoFailure, ec, {}}));
        auto created = EvidenceSession::create(config.directory);
        if (!created)
            return std::unexpected(created.error());
        session = std::move(*created);
        return {};
    }
    cache::FileStore store() const {
        return cache::FileStore(session->directory, config.store);
    }
    std::expected<void, EvidenceError> seal() {
        if (failure)
            return std::unexpected(*failure);
        if (active->nodes.empty())
            return {};
        auto next = std::make_shared<Block>();
        Segment segment;
        segment.reference = {
            {checked_id(totals.nodes - active->nodes.size() + 1)},
            checked_id(active->nodes.size()),
            {},
            false};
        segment.reference.object.kind = cache::ObjectKind::EvidenceBlock;
        segment.bytes = active->bytes();
        segments.reserve(capacity(segments.capacity(), segments.size() + 1));
        if (config.mode == EvidenceBuildMode::Spill) {
            auto bytes =
                encode_evidence_block(*active, segment.reference.first,
                                      summaries.size(), config.store.codec);
            if (!bytes && bytes.error() != cache::Error::LimitExceeded)
                throw std::logic_error("inconsistent evidence segment");
            if (!bytes)
                return std::unexpected(codec_error(bytes.error()));
            const auto charge = bytes->size() + envelope_bytes;
            std::expected<cache::ObjectRef, EvidenceError> written =
                std::unexpected(error(EvidenceErrorCode::LimitExceeded));
            if (charge <= config.disk_bytes &&
                totals.charged_disk_bytes <= config.disk_bytes - charge) {
                if (auto started = start_session(); !started)
                    written = std::unexpected(started.error());
                else {
                    // Charge attempted objects too: a post-publication fault
                    // may leave a complete leaf without a successful return.
                    totals.charged_disk_bytes += charge;
                    const auto size = bytes->size();
                    auto result = store().write_object(
                        cache::ObjectKind::EvidenceBlock, std::move(*bytes));
                    if (!result)
                        written = std::unexpected(store_error(result.error()));
                    else {
                        written = *result;
                        ++totals.written_blocks;
                        totals.payload_bytes_written += size;
                    }
                }
            }
            if (written) {
                segment.reference.object = *written;
                segment.reference.disk_backed = true;
                totals.released_active_bytes += segment.bytes;
            } else {
                spill_error = written.error();
                if (config.failure_policy == SpillFailurePolicy::Unavailable)
                    return std::unexpected(written.error());
                ++totals.fallback_blocks;
                segment.resident = track(active, ledger);
                totals.resident_bytes += segment.bytes;
            }
        } else {
            segment.resident = track(active, ledger);
            totals.resident_bytes += segment.bytes;
        }
        segments.push_back(std::move(segment));
        ++totals.sealed_blocks;
        active = std::move(next);
        measure();
        return {};
    }
    std::expected<std::pair<std::shared_ptr<const Block>, std::size_t>,
                  EvidenceError>
    get(NodeId id) {
        if (failure)
            return std::unexpected(*failure);
        if (!id.value || id.value > totals.nodes)
            throw std::out_of_range("invalid evidence builder node");
        const auto active_first = totals.nodes - active->nodes.size() + 1;
        if (id.value >= active_first) {
            auto copy = std::make_shared<Block>();
            auto node = active->nodes[id.value - active_first];
            auto edges = std::span(active->edges)
                             .subspan(node.edge_begin, node.edge_count);
            copy->edges.assign(edges.begin(), edges.end());
            node.edge_begin = 0;
            copy->nodes.push_back(node);
            auto block = track(std::move(copy), ledger);
            measure();
            return std::pair{std::move(block), std::size_t{0}};
        }
        auto found = std::ranges::upper_bound(
            segments, id.value, {},
            [](const auto& segment) { return segment.reference.first.value; });
        auto& segment = *--found;
        auto block = segment.resident ? segment.resident : segment.live.lock();
        if (!block) {
            auto bytes = store().read_object(segment.reference.object);
            if (!bytes)
                return std::unexpected(store_error(bytes.error()));
            ++totals.objects_read;
            totals.payload_bytes_read += bytes->size();
            auto decoded =
                decode_evidence_block(*bytes, segment.reference,
                                      summaries.size(), config.store.codec);
            if (!decoded)
                return std::unexpected(codec_error(decoded.error()));
            block = track(std::move(*decoded), ledger);
            segment.live = block;
            segment.bytes = block->bytes();
        }
        segment.used = ++clock;
        if (!segment.resident && !segment.cached &&
            segment.bytes <= config.cache_bytes) {
            room(segment.bytes);
            while (totals.cached_bytes &&
                   totals.cached_bytes > config.cache_bytes - segment.bytes)
                evict_one();
            if (segment.bytes <= config.resident_bytes &&
                owned() <= config.resident_bytes - segment.bytes) {
                segment.cached = block;
                totals.cached_bytes += segment.bytes;
            }
        }
        measure();
        return std::pair{std::move(block),
                         std::size_t{id.value - segment.reference.first.value}};
    }
};

EvidenceBuilder::EvidenceBuilder(EvidenceBuildConfig config) {
    if ((config.mode != EvidenceBuildMode::Memory &&
         config.mode != EvidenceBuildMode::Spill) ||
        (config.failure_policy != SpillFailurePolicy::KeepMemory &&
         config.failure_policy != SpillFailurePolicy::Unavailable) ||
        !config.block_bytes || config.store.lock_timeout.count() < 0 ||
        config.store.object_bytes >
            std::numeric_limits<std::size_t>::max() - envelope_bytes ||
        (config.mode == EvidenceBuildMode::Spill &&
         (config.directory.empty() ||
          config.directory.native().find('\0') != fs::path::string_type::npos)))
        throw std::invalid_argument("invalid evidence build configuration");
    impl_ = std::make_unique<Impl>(std::move(config));
}
EvidenceBuilder::~EvidenceBuilder() = default;
EvidenceBuilder::EvidenceBuilder(EvidenceBuilder&&) noexcept = default;
EvidenceBuilder&
EvidenceBuilder::operator=(EvidenceBuilder&&) noexcept = default;
EvidenceBuilder::Impl& EvidenceBuilder::state() {
    if (!impl_)
        throw std::logic_error("evidence builder already consumed");
    return *impl_;
}
const EvidenceBuilder::Impl& EvidenceBuilder::state() const {
    if (!impl_)
        throw std::logic_error("evidence builder already consumed");
    return *impl_;
}
std::expected<NodeId, EvidenceError>
EvidenceBuilder::add(Rule rule, SiteId site, std::span<const Edge> inputs,
                     std::uint32_t payload, SummaryId owner) {
    auto& s = state();
    if (s.failure)
        return std::unexpected(*s.failure);
    if (rule > Rule::Incomplete ||
        (rule == Rule::Parameter
             ? !owner.value || owner.value > s.summaries.size() ||
                   !inputs.empty()
             : owner.value != 0) ||
        ((rule == Rule::Call || rule == Rule::SummaryRef) &&
         (!payload || payload > s.summaries.size())))
        throw std::logic_error("invalid evidence node owner or rule");
    for (const auto& edge : inputs)
        if (!edge.target.value || edge.target.value > s.totals.nodes ||
            edge.relation > Relation::Initializer ||
            (rule == Rule::Call && edge.relation != Relation::Data))
            throw std::logic_error("invalid evidence edge");
    const auto id = NodeId{checked_id(s.totals.nodes + 1)};
    try {
        const std::vector<Edge> copied(inputs.begin(), inputs.end());
        const auto fits = [&](std::size_t nodes, std::size_t edges) {
            return nodes <= s.config.store.codec.elements &&
                   edges <= s.config.store.codec.elements &&
                   29ULL + 21ULL * nodes + 5ULL * edges <=
                       std::min(s.config.store.codec.bytes,
                                s.config.store.object_bytes);
        };
        if (!fits(1, copied.size())) {
            s.fail(error(EvidenceErrorCode::LimitExceeded));
            return std::unexpected(*s.failure);
        }
        if (!s.active->nodes.empty() &&
            ((s.active->nodes.size() + 1) * sizeof(Node) +
                     (s.active->edges.size() + copied.size()) * sizeof(Edge) >
                 s.config.block_bytes ||
             !fits(s.active->nodes.size() + 1,
                   s.active->edges.size() + copied.size()))) {
            auto sealed = s.seal();
            if (!sealed) {
                s.fail(sealed.error());
                return std::unexpected(*s.failure);
            }
        }
        auto node_capacity =
            capacity(s.active->nodes.capacity(), s.active->nodes.size() + 1);
        auto edge_capacity = capacity(s.active->edges.capacity(),
                                      s.active->edges.size() + copied.size());
        auto extra = node_capacity * sizeof(Node) +
                     edge_capacity * sizeof(Edge) - s.active->bytes();
        s.room(extra);
        if (extra > s.config.resident_bytes ||
            s.owned() > s.config.resident_bytes - extra) {
            // A low resident quota may seal earlier than the size threshold.
            if (!s.active->nodes.empty()) {
                auto sealed = s.seal();
                if (!sealed) {
                    s.fail(sealed.error());
                    return std::unexpected(*s.failure);
                }
                node_capacity = 1;
                edge_capacity = copied.size();
                extra = sizeof(Node) + edge_capacity * sizeof(Edge);
                s.room(extra);
            }
            if (extra > s.config.resident_bytes ||
                s.owned() > s.config.resident_bytes - extra) {
                s.fail(error(EvidenceErrorCode::LimitExceeded));
                return std::unexpected(*s.failure);
            }
        }
        s.active->nodes.reserve(node_capacity);
        s.active->edges.reserve(edge_capacity);
        if (s.owned() > s.config.resident_bytes) {
            s.fail(error(EvidenceErrorCode::LimitExceeded));
            return std::unexpected(*s.failure);
        }
        const auto offset = checked_id(s.active->edges.size());
        s.active->edges.insert(s.active->edges.end(), copied.begin(),
                               copied.end());
        s.active->nodes.push_back(
            {rule, site, offset, checked_id(copied.size()), payload, owner});
        ++s.totals.nodes;
        s.totals.edges += copied.size();
        s.measure();
        return id;
    } catch (const std::bad_alloc&) {
        s.fail(error(EvidenceErrorCode::OutOfMemory));
        return std::unexpected(*s.failure);
    }
}
std::expected<SummaryId, EvidenceError> EvidenceBuilder::reserve_summary() {
    auto& s = state();
    if (s.failure)
        return std::unexpected(*s.failure);
    if (s.summaries.size() >= s.config.store.codec.elements) {
        s.fail(error(EvidenceErrorCode::LimitExceeded));
        return std::unexpected(*s.failure);
    }
    const auto id = SummaryId{checked_id(s.summaries.size() + 1)};
    try {
        s.summaries.push_back({id, {}, {}});
    } catch (const std::bad_alloc&) {
        s.fail(error(EvidenceErrorCode::OutOfMemory));
        return std::unexpected(*s.failure);
    }
    return id;
}
std::expected<void, EvidenceError>
EvidenceBuilder::define_summary(SummaryId summary, NodeId root) {
    auto& s = state();
    if (s.failure)
        return std::unexpected(*s.failure);
    if (!summary.value || summary.value > s.summaries.size() || !root.value ||
        root.value > s.totals.nodes)
        throw std::out_of_range("invalid evidence summary binding");
    auto& entry = s.summaries[summary.value - 1];
    if (entry.value.value)
        throw std::logic_error("evidence summary already bound");
    entry.value = entry.effects = root;
    return {};
}
std::expected<NodeLease, EvidenceError> EvidenceBuilder::pin(NodeId id) {
    auto& s = state();
    try {
        auto loaded = s.get(id);
        if (!loaded) {
            s.fail(loaded.error());
            return std::unexpected(*s.failure);
        }
        const auto& node = loaded->first->nodes[loaded->second];
        return NodeLease(loaded->first, &node,
                         std::span(loaded->first->edges)
                             .subspan(node.edge_begin, node.edge_count));
    } catch (const std::bad_alloc&) {
        s.fail(error(EvidenceErrorCode::OutOfMemory));
        return std::unexpected(*s.failure);
    }
}
std::expected<void, EvidenceError> EvidenceBuilder::seal() {
    auto& s = state();
    try {
        auto result = s.seal();
        if (!result)
            s.fail(result.error());
        return result;
    } catch (const std::bad_alloc&) {
        s.fail(error(EvidenceErrorCode::OutOfMemory));
        return std::unexpected(*s.failure);
    }
}
std::expected<BuiltEvidence, EvidenceError> EvidenceBuilder::freeze() && {
    auto& s = state();
    if (s.failure)
        return std::unexpected(*s.failure);
    for (const auto& summary : s.summaries)
        if (!summary.value.value)
            throw std::logic_error("unpublished evidence summary");
    if (auto sealed = seal(); !sealed)
        return std::unexpected(sealed.error());
    try {
        evict();
        const auto construction = stats();
        auto shared = std::shared_ptr<Impl>(std::move(impl_));
        std::vector<EvidenceBlockRef> directory;
        for (const auto& segment : shared->segments)
            directory.push_back(segment.reference);
        auto view = EvidenceView::segmented(
            std::move(directory), shared->summaries,
            [shared](std::size_t index)
                -> std::expected<std::shared_ptr<const Block>, EvidenceError> {
                const auto& segment = shared->segments[index];
                if (segment.resident)
                    return segment.resident;
                auto bytes =
                    shared->store().read_object(segment.reference.object);
                if (!bytes)
                    return std::unexpected(store_error(bytes.error()));
                auto decoded = decode_evidence_block(
                    *bytes, segment.reference, shared->summaries.size(),
                    shared->config.store.codec);
                if (!decoded)
                    return std::unexpected(codec_error(decoded.error()));
                return track(std::move(*decoded), shared->ledger);
            },
            std::min(shared->config.cache_bytes,
                     shared->config.resident_bytes -
                         shared->totals.resident_bytes),
            shared->config.store.codec);
        if (!view)
            return std::unexpected(view.error());
        return BuiltEvidence{std::move(*view), construction};
    } catch (const std::bad_alloc&) {
        // impl_ may already have transferred into the temporary shared state.
        return std::unexpected(error(EvidenceErrorCode::OutOfMemory));
    }
}
void EvidenceBuilder::evict() {
    auto& s = state();
    for (auto& segment : s.segments)
        segment.cached.reset();
    s.totals.cached_bytes = 0;
}
EvidenceBuildStats EvidenceBuilder::stats() const {
    const auto& s = state();
    auto result = s.totals;
    result.active_bytes = s.active->bytes();
    result.live_bytes =
        result.active_bytes + s.ledger->live.load(std::memory_order_relaxed);
    result.summaries = s.summaries.size();
    result.directory_bytes = s.segments.capacity() * sizeof(Impl::Segment);
    result.summary_bytes = s.summaries.capacity() * sizeof(SummaryDescription);
    return result;
}
const std::optional<EvidenceError>& EvidenceBuilder::failure() const {
    return state().failure;
}
const std::optional<EvidenceError>& EvidenceBuilder::last_spill_error() const {
    return state().spill_error;
}
fs::path EvidenceBuilder::temporary_directory() const {
    const auto& s = state();
    return s.session ? s.session->directory : fs::path{};
}
} // namespace pagos::stage::provenance
