#pragma once

#include "pagos/stage/generator_replay_record.h"
#include "pagos/stage/scalar_replay_execution.h"

namespace pagos::stage {
inline constexpr std::uint32_t generator_replay_execution_version = 1;
struct GeneratorReplayExecutionStats {
    std::size_t bound_nodes_evaluated{}, element_nodes_evaluated{};
    std::size_t fuel_consumed{}, detail_records{}, value_bytes{},
        output_bytes{};
    bool operator==(const GeneratorReplayExecutionStats&) const = default;
};
enum class GeneratorReplayPhase { Admission, Begin, End, Element };
enum class GeneratorReplayExecutionErrorCode {
    InvalidRecord,
    InvalidSelection,
    FuelExhausted,
    ValueMemoryLimit,
    OutputLimit,
    Cancelled,
    ArithmeticFailure,
    BoundsMismatch,
    ResultMismatch,
    OutOfMemory
};
struct GeneratorReplayExecutionError {
    GeneratorReplayExecutionErrorCode code;
    GeneratorReplayExecutionStats stats;
    GeneratorReplayPhase phase;
    std::optional<std::uint32_t> node;
    std::optional<ReplayError> record;
    std::optional<IntegerError> integer;
};
struct VerifiedGeneratorReplay {
    cache::Digest descriptor;
    cache::Digest recorded_result;
    std::uint32_t element_index{}, iteration_value{};
    cache::TypedValue value;
    std::vector<ScalarReplayStep> details; // Only the selected element's nodes.
    GeneratorReplayExecutionStats stats;
};
// Requires source admission (load_generator_replay establishes it), verifies
// recorded binding, computes both bounds and exactly one selected element.
// Index is zero-based relative to the array, not the iteration variable.
// Uses scalar limits: fuel is one per attempted node plus each of three final
// verifications; values are max(recipe.nodes) * sizeof(ScalarConstant), output
// is sizeof(VerifiedGeneratorReplay) + retained detail capacity. Codec scratch
// and the caller-owned input array are separately bounded by codec limits.
// No Stage, IO, writes, effects, continuation or partial values on failure.
[[nodiscard]] std::expected<VerifiedGeneratorReplay,
                            GeneratorReplayExecutionError>
execute_generator_replay(const GeneratorReplayRecord& record,
                         std::uint32_t element_index,
                         ScalarReplayExecutionLimits limits = {},
                         ScalarReplayDetailRange details = {},
                         std::stop_token cancellation = {});
} // namespace pagos::stage
