#pragma once

#include "../cache/wire.h"
#include "pagos/stage/provenance_snapshot.h"
#include "recursive_state.h"

namespace pagos::stage::provenance::recursive_wire {
inline constexpr std::uint32_t manifest_version = 5;
inline constexpr std::uint32_t segmented_version = 6;
inline constexpr std::string_view checksum_domain = "pagos.recursive-task.v1";
inline void require(bool value) {
    if (!value)
        throw CheckpointError{CheckpointErrorCode::Corrupt, {}};
}
inline CheckpointError codec_error(cache::Error error) {
    return {error == cache::Error::LimitExceeded
                ? CheckpointErrorCode::LimitExceeded
            : error == cache::Error::IncompatibleVersion
                ? CheckpointErrorCode::IncompatibleVersion
                : CheckpointErrorCode::Corrupt,
            {}};
}
inline CheckpointError store_error(cache::StoreError error) {
    return {error.code == cache::StoreErrorCode::LimitExceeded
                ? CheckpointErrorCode::LimitExceeded
                : CheckpointErrorCode::StorageFailure,
            {},
            error};
}
inline std::span<const std::uint8_t>
verified(std::span<const std::uint8_t> bytes, cache::Limits limits) {
    if (bytes.size() > limits.bytes)
        throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
    require(bytes.size() >= 32);
    const auto payload = bytes.first(bytes.size() - 32);
    const auto hash = cache::fingerprint(checksum_domain, payload);
    require(std::ranges::equal(hash.bytes, bytes.last(32)));
    return payload;
}
inline cache::Bytes sealed(std::expected<cache::Bytes, cache::Error> bytes,
                           cache::Limits limits) {
    if (!bytes)
        throw codec_error(bytes.error());
    if (limits.bytes < 32 || bytes->size() > limits.bytes - 32)
        throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
    const auto hash = cache::fingerprint(checksum_domain, *bytes);
    bytes->insert(bytes->end(), hash.bytes.begin(), hash.bytes.end());
    return std::move(*bytes);
}
inline cache::TaskStatus task_status(QueryProgress progress) {
    return progress == QueryProgress::Completed
               ? cache::TaskStatus::AwaitingDelivery
           : progress == QueryProgress::Cancelled ? cache::TaskStatus::Cancelled
                                                  : cache::TaskStatus::Paused;
}
inline void finish(cache::wire::Reader& reader) {
    if (auto result = reader.finish(); !result)
        throw codec_error(result.error());
}
inline cache::Digest digest(cache::wire::Reader& reader) {
    cache::Digest value;
    const auto bytes = reader.raw(value.bytes.size());
    if (reader.good())
        std::ranges::copy(bytes, value.bytes.begin());
    return value;
}
inline std::size_t size(cache::wire::Reader& reader) {
    const auto value = reader.u64();
    if (value > std::numeric_limits<std::size_t>::max())
        throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
    return static_cast<std::size_t>(value);
}
inline void index(cache::wire::Writer& writer, std::size_t value) {
    writer.u64(value == absent ? UINT64_MAX : value);
}
inline std::size_t index(cache::wire::Reader& reader) {
    const auto value = reader.u64();
    if (value == UINT64_MAX)
        return absent;
    if (value >= std::numeric_limits<std::size_t>::max())
        throw CheckpointError{CheckpointErrorCode::LimitExceeded, {}};
    return static_cast<std::size_t>(value);
}
inline void atom(cache::wire::Writer& writer, const RecursiveAtom& value) {
    writer.u8(static_cast<std::uint8_t>(value.kind));
    writer.u32(value.owner);
    writer.u32(value.slot);
    writer.u32(value.node);
}
inline RecursiveAtom atom(cache::wire::Reader& reader) {
    RecursiveAtom value{static_cast<RecursiveAtomKind>(reader.u8()),
                        reader.u32(), reader.u32(), reader.u32()};
    require(value.kind == RecursiveAtomKind::Parameter ||
            value.kind == RecursiveAtomKind::SourceFamily);
    return value;
}
template <class F> void counters(RecursiveStats& stats, F f) {
    f(stats.steps);
    f(stats.slices);
    f(stats.nodes);
    f(stats.links);
    f(stats.components);
    f(stats.cyclic_components);
    f(stats.evaluations);
    f(stats.facts_added);
    f(stats.queue_peak);
}
template <class Set, class Iterator>
void position(cache::wire::Writer& writer, const Set& values,
              Iterator iterator) {
    writer.boolean(iterator != values.end());
    if (iterator != values.end())
        atom(writer, *iterator);
}
inline std::optional<RecursiveAtom> position(cache::wire::Reader& reader) {
    return reader.boolean() ? std::optional(atom(reader)) : std::nullopt;
}
template <class Set>
auto locate(const Set& values, const std::optional<RecursiveAtom>& value) {
    if (!value)
        return values.end();
    const auto found = values.find(*value);
    require(found != values.end());
    return found;
}
} // namespace pagos::stage::provenance::recursive_wire
