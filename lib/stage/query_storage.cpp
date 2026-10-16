#include "query_storage.h"
#include "../cache/wire.h"

#include <algorithm>
#include <list>

namespace pagos::stage::provenance::detail {
namespace {
CheckpointError corrupt() { return {CheckpointErrorCode::Corrupt, {}}; }
CheckpointError limit() { return {CheckpointErrorCode::LimitExceeded, {}}; }
void require(bool condition) {
    if (!condition)
        throw corrupt();
}
void finish(cache::wire::Reader& reader) {
    if (auto result = reader.finish(); !result)
        throw CheckpointError{result.error() == cache::Error::LimitExceeded
                                  ? CheckpointErrorCode::LimitExceeded
                              : result.error() ==
                                      cache::Error::IncompatibleVersion
                                  ? CheckpointErrorCode::IncompatibleVersion
                                  : CheckpointErrorCode::Corrupt,
                              {}};
}
void storage_error(cache::StoreError error) {
    throw CheckpointError{error.code == cache::StoreErrorCode::LimitExceeded
                              ? CheckpointErrorCode::LimitExceeded
                              : CheckpointErrorCode::StorageFailure,
                          {},
                          error};
}
cache::Digest digest(cache::wire::Reader& reader) {
    cache::Digest value;
    auto bytes = reader.raw(32);
    if (reader.good())
        std::ranges::copy(bytes, value.bytes.begin());
    return value;
}
constexpr std::array widths{12U, 16U, 12U, 8U, 16U, 16U};
} // namespace
struct QueryStorage::Impl {
    cache::TaskStore store;
    QueryPagingConfig config;
    cache::Limits limits;
    std::size_t capacity;
    Directory directory;
    Sizes totals{};
    std::array<std::vector<QueryRecord>, 6> tails;
    struct Cached {
        unsigned table;
        std::size_t segment;
        std::size_t count;
        std::unique_ptr<QueryRecord[]> records;
        std::size_t bytes() const {
            return count * sizeof(QueryRecord) + sizeof(Cached);
        }
    };
    mutable std::list<Cached> cache;
    mutable std::size_t cached{}, peak{};
    mutable QueryPagingStats stats;
    Impl(cache::TaskStore input, QueryPagingConfig cfg, cache::Limits bounds)
        : store(std::move(input)), config(cfg), limits(bounds),
          capacity(cfg.segment_bytes >= 70 ? (cfg.segment_bytes - 54) / 16
                                           : 0) {
        if (!capacity || cfg.segment_bytes > bounds.bytes ||
            cfg.cache_bytes < capacity * sizeof(QueryRecord) + sizeof(Cached) ||
            cfg.cache_bytes > cfg.resident_bytes ||
            capacity > (cfg.resident_bytes - cfg.cache_bytes) /
                           (6 * sizeof(QueryRecord)))
            throw limit();
        std::size_t bytes{};
        for (auto& tail : tails) {
            tail.reserve(capacity);
            bytes += tail.capacity() * sizeof(QueryRecord);
        }
        if (bytes > cfg.resident_bytes - cfg.cache_bytes)
            throw limit();
        peak = bytes;
    }
    std::size_t tail_bytes() const {
        std::size_t total{};
        for (const auto& tail : tails)
            total += tail.capacity() * sizeof(QueryRecord);
        return total;
    }
    void evict_for(std::size_t bytes) const {
        if (bytes > config.cache_bytes)
            throw limit();
        while (cached > config.cache_bytes - bytes) {
            cached -= cache.back().bytes();
            cache.pop_back();
        }
    }
    std::span<const QueryRecord> load(unsigned table, std::size_t index) const {
        for (auto found = cache.begin(); found != cache.end(); ++found) {
            if (found->table == table && found->segment == index) {
                ++stats.hits;
                cache.splice(cache.begin(), cache, found);
                return {cache.front().records.get(), cache.front().count};
            }
        }
        const auto& ref = directory[table].at(index);
        const auto owned = ref.count * sizeof(QueryRecord) + sizeof(Cached);
        evict_for(owned);
        auto payload = store.read(ref.object);
        if (!payload)
            storage_error(payload.error());
        ++stats.reads;
        stats.bytes_read += payload->size();
        cache::wire::Reader reader(*payload, limits);
        cache::wire::header(reader, cache::ObjectKind::QueryStateSegment);
        if (reader.u32() != 2)
            throw CheckpointError{CheckpointErrorCode::IncompatibleVersion, {}};
        require(digest(reader) == store.task() && reader.u8() == table &&
                reader.u32() == ref.first && reader.u32() == ref.count);
        auto records = std::make_unique<QueryRecord[]>(ref.count);
        for (std::size_t i = 0; i < ref.count; ++i) {
            auto& record = records[i];
            if (table >= 4) {
                record = {reader.u64(), reader.u64()};
            } else {
                const auto node = reader.u32(), context = reader.u32();
                record.key = node | (std::uint64_t{context} << 32U);
                record.value = table == 1   ? reader.u64()
                               : table == 3 ? 0
                                            : reader.u32();
                if (table == 1 && record.value != UINT64_MAX &&
                    record.value >= std::numeric_limits<std::size_t>::max())
                    throw limit();
                if (table == 1 && record.value == UINT64_MAX)
                    record.value = std::numeric_limits<std::size_t>::max();
            }
            if (table >= 4) {
                require(record.value < (table == 4 ? totals[1] : totals[0]) &&
                        (table != 5 || record.value));
                if (i)
                    require(records[i - 1].key < record.key);
            }
        }
        finish(reader);
        if (table >= 4)
            require(records[0].key == ref.minimum &&
                    records[ref.count - 1].key == ref.maximum);
        cache.push_front({table, index, ref.count, std::move(records)});
        cached += owned;
        peak = std::max(peak, tail_bytes() + cached);
        return {cache.front().records.get(), cache.front().count};
    }
    void flush(unsigned table, cache::TaskWriter& writer) {
        auto& tail = tails[table];
        if (tail.empty())
            return;
        if (table >= 4) {
            std::ranges::sort(tail, {}, &QueryRecord::key);
            for (std::size_t i = 1; i < tail.size(); ++i)
                require(tail[i - 1].key < tail[i].key);
        }
        const auto first = totals[table] - tail.size();
        cache::wire::Writer output(limits);
        cache::wire::header(output, cache::ObjectKind::QueryStateSegment);
        output.u32(2);
        output.raw(store.task().bytes);
        output.u8(static_cast<std::uint8_t>(table));
        output.count(first);
        output.count(tail.size());
        for (const auto& record : tail) {
            if (table >= 4) {
                output.u64(record.key);
                output.u64(record.value);
            } else {
                output.u32(static_cast<std::uint32_t>(record.key));
                output.u32(static_cast<std::uint32_t>(record.key >> 32U));
                if (table == 1)
                    output.u64(record.value ==
                                       std::numeric_limits<std::size_t>::max()
                                   ? UINT64_MAX
                                   : record.value);
                else if (table != 3)
                    output.u32(static_cast<std::uint32_t>(record.value));
            }
        }
        auto bytes = output.finish();
        if (!bytes)
            throw limit();
        const auto length = bytes->size();
        auto ref = writer.write(cache::ObjectKind::QueryStateSegment,
                                std::move(*bytes));
        if (!ref)
            storage_error(ref.error());
        directory[table].push_back({static_cast<std::uint32_t>(first),
                                    static_cast<std::uint32_t>(tail.size()),
                                    *ref, table >= 4 ? tail.front().key : 0,
                                    table >= 4 ? tail.back().key : 0});
        tail.clear();
        ++stats.writes;
        stats.bytes_written += length;
    }
};
QueryStorage::QueryStorage(cache::TaskStore store, QueryPagingConfig config,
                           cache::Limits limits)
    : impl_(std::make_unique<Impl>(std::move(store), config, limits)) {}
QueryStorage::~QueryStorage() = default;
std::size_t QueryStorage::size(unsigned table) const {
    return impl_->totals.at(table);
}
std::size_t QueryStorage::tail_bytes(unsigned table) const {
    return impl_->tails.at(table).capacity() * sizeof(QueryRecord);
}
std::size_t QueryStorage::cached_bytes() const { return impl_->cached; }
std::size_t QueryStorage::directory_bytes() const {
    std::size_t n{};
    for (auto& table : impl_->directory)
        n += table.capacity() * sizeof(QuerySegment);
    return n;
}
std::size_t QueryStorage::peak_buffers() const { return impl_->peak; }
QueryPagingStats QueryStorage::stats() const { return impl_->stats; }
const QueryStorage::Directory& QueryStorage::directory() const {
    return impl_->directory;
}
QueryPagingConfig QueryStorage::config() const { return impl_->config; }
QueryRecord QueryStorage::get(unsigned table, std::size_t position) const {
    const auto& state = *impl_;
    require(table < 6 && position < state.totals[table]);
    const auto& tail = state.tails[table];
    const auto first = state.totals[table] - tail.size();
    if (position >= first)
        return tail[position - first];
    const auto& directory = state.directory[table];
    auto found =
        std::upper_bound(directory.begin(), directory.end(), position,
                         [](std::size_t value, const QuerySegment& segment) {
                             return value < segment.first;
                         });
    require(found != directory.begin());
    --found;
    require(position - found->first < found->count);
    return state.load(table,
                      static_cast<std::size_t>(
                          found - directory.begin()))[position - found->first];
}
void QueryStorage::append(unsigned table, QueryRecord record) {
    auto& state = *impl_;
    auto& tail = state.tails.at(table);
    if (tail.size() == state.capacity ||
        state.totals[table] >= state.limits.elements ||
        state.totals[table] >= UINT32_MAX)
        throw limit();
    tail.push_back(record);
    ++state.totals[table];
}
std::optional<std::size_t> QueryStorage::find(unsigned table,
                                              std::uint64_t key) const {
    require(table == 4 || table == 5);
    const auto& state = *impl_;
    for (const auto& record : state.tails[table])
        if (record.key == key)
            return static_cast<std::size_t>(record.value);
    const auto& directory = state.directory[table];
    for (std::size_t i = directory.size(); i > 0; --i) {
        const auto& segment = directory[i - 1];
        if (key < segment.minimum || key > segment.maximum)
            continue;
        const auto records = state.load(table, i - 1);
        const auto found =
            std::ranges::lower_bound(records, key, {}, &QueryRecord::key);
        if (found != records.end() && found->key == key)
            return static_cast<std::size_t>(found->value);
    }
    return {};
}
void QueryStorage::prepare() {
    auto& state = *impl_;
    bool full = false;
    for (auto& tail : state.tails)
        full |= tail.size() == state.capacity;
    if (!full)
        return;
    auto head = state.store.head();
    if (!head)
        storage_error(head.error());
    auto writer = state.store.begin(*head ? (**head).generation : 0);
    if (!writer)
        storage_error(writer.error());
    for (unsigned table = 0; table < 6; ++table)
        if (state.tails[table].size() == state.capacity)
            state.flush(table, *writer);
}
void QueryStorage::seal(cache::TaskWriter& writer) {
    for (unsigned table = 0; table < 6; ++table)
        impl_->flush(table, writer);
}
QueryStorage::Sizes QueryStorage::mark() const {
    Sizes result;
    for (unsigned table = 0; table < 6; ++table)
        result[table] = impl_->tails[table].size();
    return result;
}
void QueryStorage::rollback(const Sizes& sizes) noexcept {
    for (unsigned table = 0; table < 6; ++table) {
        auto& tail = impl_->tails[table];
        impl_->totals[table] -= tail.size() - sizes[table];
        tail.resize(sizes[table]);
    }
}
void QueryStorage::restore(Directory directory, Sizes sizes) {
    auto& state = *impl_;
    for (unsigned table = 0; table < 6; ++table) {
        std::size_t next{};
        for (const auto& segment : directory[table]) {
            require(segment.first == next && segment.count &&
                    segment.count <= sizes[table] - next &&
                    segment.object.kind ==
                        cache::ObjectKind::QueryStateSegment &&
                    segment.object.size ==
                        54 + std::uint64_t{segment.count} * widths[table]);
            if (segment.count * sizeof(QueryRecord) + sizeof(Impl::Cached) >
                    state.config.cache_bytes ||
                segment.object.size > state.limits.bytes)
                throw limit();
            if (table >= 4)
                require(segment.minimum <= segment.maximum);
            next += segment.count;
        }
        require(next == sizes[table]);
        if (sizes[table] > state.limits.elements)
            throw limit();
    }
    require(sizes[0] && sizes[4] == sizes[1] && sizes[5] == sizes[0] - 1);
    state.directory = std::move(directory);
    state.totals = sizes;
}
void QueryStorage::check_store(const cache::TaskStore& store) const {
    if (store.task() != impl_->store.task() ||
        store.directory() != impl_->store.directory())
        throw CheckpointError{CheckpointErrorCode::BindingMismatch, {}};
}
void QueryStorage::rebind(const cache::TaskStore& store) {
    check_store(store);
    impl_->store = store;
}
void QueryStorage::evict() const {
    impl_->cache.clear();
    impl_->cached = 0;
}
} // namespace pagos::stage::provenance::detail
