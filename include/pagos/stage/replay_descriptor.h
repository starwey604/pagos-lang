#pragma once

#include "pagos/stage/cache_identity.h"

namespace pagos::stage {
inline constexpr std::uint32_t replay_descriptor_version = 1;
inline constexpr std::uint32_t scalar_recipe_semantics_version = 1;

// Stable wire codes, independent of syntax enum numbering. No calls, memory,
// branches, casts or short-circuit operators are admitted by version 1.
enum class ScalarReplayOp : std::uint8_t {
    Input = 1,
    Constant = 2,
    Negate = 3,
    BitNot = 4,
    Not = 5,
    Add = 6,
    Subtract = 7,
    Multiply = 8,
    Divide = 9,
    Remainder = 10,
    BitAnd = 11,
    BitOr = 12,
    BitXor = 13,
    ShiftLeft = 14,
    ShiftRight = 15,
    Equal = 16,
    NotEqual = 17,
    Less = 18,
    LessEqual = 19,
    Greater = 20,
    GreaterEqual = 21
};
struct ScalarReplayNode {
    ScalarReplayOp operation;
    sema::Type type;
    std::uint32_t position{}; // Structural SourceAnchor in this definition.
    std::optional<std::uint32_t> input; // Original Comptime parameter slot.
    std::optional<Constant> constant;
    std::vector<std::uint32_t> operands; // Zero-based, strictly earlier nodes.
    bool operator==(const ScalarReplayNode&) const = default;
};
// Self-contained immutable straight-line recipe, not a saved operation trace.
// The last node is the result; every node must contribute to it. Stage's future
// adapter must establish that this recipe describes the recorded computation.
struct ReplayDescriptor {
    std::uint32_t version{replay_descriptor_version};
    std::uint32_t semantics{scalar_recipe_semantics_version};
    cache::QueryKey specialization;
    cache::Digest verified_inputs;
    SourceAnchor result_site;
    sema::Type result_type;
    cache::Digest expected_result; // replay_value_fingerprint(TypedValue).
    std::vector<ScalarReplayNode> nodes;
    bool operator==(const ReplayDescriptor&) const = default;
};
enum class ReplayErrorCode {
    MissingDescriptor,
    IncompatibleVersion,
    InvalidFormat,
    BindingMismatch,
    InvalidRecipe,
    UnsupportedType,
    UnsupportedOperation,
    InvalidReference,
    InvalidAnchor,
    ResultMismatch,
    LimitExceeded,
    OutOfMemory
};
struct ReplayError {
    ReplayErrorCode code;
    std::optional<cache::Error> codec;
    std::optional<std::uint32_t> node;
    bool operator==(const ReplayError&) const = default;
};
[[nodiscard]] std::expected<cache::Digest, ReplayError>
replay_value_fingerprint(const cache::TypedValue& value,
                         cache::Limits limits = {});
// Codec admission checks shape, types, references and key/anchor coherence.
// It neither resolves current source positions nor evaluates any operation.
[[nodiscard]] std::expected<cache::Bytes, ReplayError>
encode_replay_descriptor(const ReplayDescriptor& descriptor,
                         cache::Limits limits = {});
[[nodiscard]] std::expected<ReplayDescriptor, ReplayError>
decode_replay_descriptor(std::span<const std::uint8_t> bytes,
                         cache::Limits limits = {});
[[nodiscard]] std::expected<cache::Digest, ReplayError>
replay_descriptor_fingerprint(const ReplayDescriptor& descriptor,
                              cache::Limits limits = {});
// The caller supplies independently verified current dependencies/key/result.
// Success checks structural admission and current binding, not correspondence
// to source semantics or a proof that evaluation will match expected_result.
// No Stage calls, source execution, IO or synthesized fallback for old
// literals.
[[nodiscard]] std::expected<void, ReplayError> validate_replay_descriptor(
    const ReplayDescriptor* descriptor, const CacheIdentity& identity,
    const cache::QueryKey& current, const cache::Digest& verified_inputs,
    const cache::TypedValue& expected_result, cache::Limits limits = {});
} // namespace pagos::stage
