#pragma once

#include "pagos/stage/scalar_replay.h"

#include <stop_token>

namespace pagos::stage {
inline constexpr std::uint32_t scalar_replay_execution_version = 1;
struct ScalarReplayExecutionLimits {
    std::size_t fuel{
        100'000}; // One per attempted node, plus final verification.
    std::size_t value_bytes{8 * 1024 * 1024}; // N * sizeof(ScalarConstant).
    std::size_t output_records{4096};
    // Fixed result object plus retained detail capacity; not rendered bytes.
    std::size_t output_bytes{1024 * 1024};
    cache::Limits codec; // Descriptor admission scratch is separately bounded.
};
struct ScalarReplayDetailRange {
    std::size_t first{};
    std::optional<std::size_t> count; // Absent: all remaining nodes.
};
struct ScalarReplayExecutionStats {
    std::size_t nodes_evaluated{}, fuel_consumed{}, detail_records{};
    // Execution-owned container payloads; no allocator/RSS or input ownership.
    std::size_t value_bytes{}, output_bytes{};
    bool operator==(const ScalarReplayExecutionStats&) const = default;
};
struct ScalarReplayStep {
    std::uint32_t node; // Index into the descriptor bound by the result digest.
    ScalarConstant value;
    bool operator==(const ScalarReplayStep&) const = default;
};
struct VerifiedScalarReplay {
    cache::Digest descriptor;
    cache::TypedValue value;
    std::vector<ScalarReplayStep> details;
    ScalarReplayExecutionStats stats;
};
enum class ScalarReplayExecutionErrorCode {
    InvalidDescriptor,
    InvalidRecordedValue,
    InvalidSelection,
    FuelExhausted,
    ValueMemoryLimit,
    OutputLimit,
    Cancelled,
    ArithmeticFailure,
    ResultMismatch,
    OutOfMemory
};
struct ScalarReplayExecutionError {
    ScalarReplayExecutionErrorCode code;
    ScalarReplayExecutionStats stats;
    // No partial values/details escape through an error.
    std::optional<std::uint32_t> node;
    std::optional<ReplayError> descriptor;
    std::optional<IntegerError> integer;
};
// Use load_scalar_replay first to establish current source correspondence.
// This executor revalidates shape/value binding and verifies the actual math;
// it cannot establish source correspondence from a manually assembled record.
// No Stage, IO, cache writes or callbacks. Cancellation is checked at primitive
// boundaries and around admission/publication, not as a wall-clock deadline.
// One-shot: failed/limited/cancelled runs must restart; no saved continuation.
[[nodiscard]] std::expected<VerifiedScalarReplay, ScalarReplayExecutionError>
execute_scalar_replay(const ScalarReplayRecord& record,
                      ScalarReplayExecutionLimits limits = {},
                      ScalarReplayDetailRange details = {},
                      std::stop_token cancellation = {});
} // namespace pagos::stage
