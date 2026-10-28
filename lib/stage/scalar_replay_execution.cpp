#include "pagos/stage/scalar_replay_execution.h"
#include "scalar_replay_evaluator.h"

#include <memory>
#include <stdexcept>

namespace pagos::stage {
namespace {
using Code = ScalarReplayExecutionErrorCode;
} // namespace
std::expected<VerifiedScalarReplay, ScalarReplayExecutionError>
execute_scalar_replay(const ScalarReplayRecord& record,
                      ScalarReplayExecutionLimits limits,
                      ScalarReplayDetailRange range,
                      std::stop_token cancellation) {
    ScalarReplayExecutionStats stats;
    const auto fail = [&](Code code, std::optional<std::uint32_t> node = {},
                          std::optional<ReplayError> descriptor = {},
                          std::optional<IntegerError> integer = {}) -> void {
        throw ScalarReplayExecutionError{code, stats, node, descriptor,
                                         integer};
    };
    const auto cancelled = [&](std::optional<std::uint32_t> node = {}) {
        if (cancellation.stop_requested())
            fail(Code::Cancelled, node);
    };
    const auto charge = [&](std::optional<std::uint32_t> node = {}) {
        cancelled(node);
        if (stats.fuel_consumed == limits.fuel)
            fail(Code::FuelExhausted, node);
        ++stats.fuel_consumed;
    };
    try {
        cancelled();
        const auto& descriptor = record.descriptor;
        const auto count = descriptor.nodes.size();
        if (range.first > count ||
            (range.count && *range.count > count - range.first))
            fail(Code::InvalidSelection);
        const auto retained = range.count.value_or(count - range.first);
        if (count > limits.value_bytes / sizeof(ScalarConstant))
            fail(Code::ValueMemoryLimit);
        if (retained > limits.output_records ||
            limits.output_bytes < sizeof(VerifiedScalarReplay) ||
            retained > (limits.output_bytes - sizeof(VerifiedScalarReplay)) /
                           sizeof(ScalarReplayStep))
            fail(Code::OutputLimit);
        cache::Digest binding;
        {
            // No evaluation happens during admission. Release the canonical
            // encoding before allocating the execution value/trace containers.
            auto bytes = encode_replay_descriptor(descriptor, limits.codec);
            if (!bytes)
                fail(Code::InvalidDescriptor, {}, bytes.error());
            binding = cache::fingerprint("pagos.replay-descriptor.v1", *bytes);
            const auto expected =
                replay_value_fingerprint(record.value, limits.codec);
            if (!expected)
                fail(Code::InvalidRecordedValue, {}, expected.error());
            if (record.value.type != descriptor.result_type ||
                *expected != descriptor.expected_result)
                fail(Code::InvalidRecordedValue);
        }
        cancelled();
        // Admission guarantees a nonempty scalar recipe. Zero Fuel does not
        // allocate evaluation/output containers or claim partial completion.
        if (!limits.fuel)
            fail(Code::FuelExhausted, 0);
        auto values = std::make_unique<ScalarConstant[]>(count);
        stats.value_bytes = count * sizeof(ScalarConstant);
        std::vector<ScalarReplayStep> details;
        details.reserve(retained);
        if (details.capacity() >
            (limits.output_bytes - sizeof(VerifiedScalarReplay)) /
                sizeof(ScalarReplayStep))
            fail(Code::OutputLimit);
        stats.output_bytes = sizeof(VerifiedScalarReplay) +
                             details.capacity() * sizeof(ScalarReplayStep);
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = static_cast<std::uint32_t>(i);
            charge(index);
            auto value = replay_detail::evaluate(
                descriptor.nodes[i], descriptor.specialization.arguments,
                values.get());
            if (!value)
                fail(Code::ArithmeticFailure, index, {}, value.error());
            values[i] = *value;
            ++stats.nodes_evaluated;
            if (i >= range.first && i - range.first < retained) {
                details.push_back({index, *value});
                ++stats.detail_records;
            }
        }
        charge(); // Verification is an explicit independent Fuel step.
        cache::TypedValue value{descriptor.result_type,
                                from_scalar(values[count - 1])};
        const auto actual = replay_value_fingerprint(value, limits.codec);
        if (!actual)
            fail(Code::InvalidDescriptor, {}, actual.error());
        if (*actual != descriptor.expected_result || value != record.value)
            fail(Code::ResultMismatch);
        cancelled();
        return VerifiedScalarReplay{binding, std::move(value),
                                    std::move(details), stats};
    } catch (const ScalarReplayExecutionError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            ScalarReplayExecutionError{Code::OutOfMemory, stats, {}, {}, {}});
    } catch (const std::length_error&) {
        return std::unexpected(ScalarReplayExecutionError{
            Code::ValueMemoryLimit, stats, {}, {}, {}});
    }
}
} // namespace pagos::stage
