#pragma once

#include "pagos/sema/type.h"
#include "pagos/value.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pagos::cache {

using Bytes = std::vector<std::uint8_t>;
inline constexpr std::uint32_t format_version = 1;
inline constexpr std::uint32_t query_protocol_version = 1;
inline constexpr std::uint32_t budget_protocol_version = 1;

enum class ObjectKind : std::uint8_t {
    Value = 1,
    QueryKey = 2,
    SourceAnchor = 3,
    ResourceRecipe = 4,
    ProvenanceSnapshot = 5,
    ProvenancePositions = 6,
    ResidualTemplate = 7,
    EvidenceBlock = 8,
    EvidenceRoot = 9,
    EvidenceSummary = 10,
    EvidenceDetail = 11,
    QueryCheckpoint = 12,
    QueryStateSegment = 13,
    QueryTaskManifest = 14,
    ReplayDescriptor = 15,
    GeneratorReplayDescriptor = 16
};
enum class Error {
    UnsupportedType,
    InvalidValue,
    InvalidFormat,
    IncompatibleVersion,
    LimitExceeded,
    InvalidContext,
    MissingType,
    MissingDefinition,
    InvalidAnchor
};

struct Limits {
    std::size_t bytes{16 * 1024 * 1024};
    std::size_t elements{1'000'000};
    std::size_t string_bytes{65'536};
    std::size_t depth{256};
};

struct Digest {
    std::array<std::uint8_t, 32> bytes{};
    bool operator==(const Digest&) const = default;
    [[nodiscard]] std::string hex() const;
};

// Length-prefixed domains prevent hashes from different protocols aliasing.
[[nodiscard]] Digest fingerprint(std::string_view domain,
                                 std::span<const std::uint8_t> bytes);

struct TypedValue {
    sema::Type type;
    Constant value;
    bool operator==(const TypedValue&) const = default;
};

// C1a supports bool, declared integer widths, and concrete scalar arrays.
// Record/pointer identities and deferred/error types are deliberately rejected.
[[nodiscard]] std::expected<Bytes, Error> encode_value(const TypedValue& value,
                                                       Limits limits = {});
[[nodiscard]] std::expected<TypedValue, Error>
decode_value(std::span<const std::uint8_t> bytes, Limits limits = {});

} // namespace pagos::cache
