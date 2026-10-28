#include "pagos/stage/generator_replay_execution.h"
#include "generator_replay_record.h"
#include "scalar_replay_evaluator.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace pagos::stage {
std::expected<VerifiedGeneratorReplay, GeneratorReplayExecutionError>
execute_generator_replay(const GeneratorReplayRecord& record,
                         std::uint32_t element_index,
                         ScalarReplayExecutionLimits limits,
                         ScalarReplayDetailRange range,
                         std::stop_token cancellation) {
    using Code = GeneratorReplayExecutionErrorCode;
    using Phase = GeneratorReplayPhase;
    GeneratorReplayExecutionStats stats;
    auto phase = Phase::Admission;
    const auto fail = [&](Code code, std::optional<std::uint32_t> node = {},
                          std::optional<ReplayError> error = {},
                          std::optional<IntegerError> integer = {}) -> void {
        throw GeneratorReplayExecutionError{code, stats, phase,
                                            node, error, integer};
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
        const auto& d = record.admission.descriptor();
        const auto count = d.element.nodes.size();
        if (element_index >= d.result_type.length || range.first > count ||
            (range.count && *range.count > count - range.first))
            fail(Code::InvalidSelection);
        const auto retained = range.count.value_or(count - range.first);
        const auto maximum =
            std::max({count, d.begin.nodes.size(), d.end.nodes.size()});
        if (maximum > limits.value_bytes / sizeof(ScalarConstant))
            fail(Code::ValueMemoryLimit);
        if (retained > limits.output_records ||
            limits.output_bytes < sizeof(VerifiedGeneratorReplay) ||
            retained > (limits.output_bytes - sizeof(VerifiedGeneratorReplay)) /
                           sizeof(ScalarReplayStep))
            fail(Code::OutputLimit);
        const auto valid = generator_detail::validate(
            record.admission, record.observation, record.value, limits.codec);
        if (!valid)
            fail(Code::InvalidRecord, {}, valid.error());
        cancelled();
        if (!limits.fuel)
            fail(Code::FuelExhausted);
        auto values = std::make_unique<ScalarConstant[]>(maximum);
        stats.value_bytes = maximum * sizeof(ScalarConstant);
        std::vector<ScalarReplayStep> details;
        details.reserve(retained);
        if (details.capacity() >
            (limits.output_bytes - sizeof(VerifiedGeneratorReplay)) /
                sizeof(ScalarReplayStep))
            fail(Code::OutputLimit);
        stats.output_bytes = sizeof(VerifiedGeneratorReplay) +
                             details.capacity() * sizeof(ScalarReplayStep);
        const auto evaluate = [&](const ScalarReplayRecipe& recipe,
                                  std::optional<IntegerValue> iterator) {
            for (std::size_t i = 0; i < recipe.nodes.size(); ++i) {
                const auto index = static_cast<std::uint32_t>(i);
                charge(index);
                auto result = replay_detail::evaluate(
                    recipe.nodes[i], d.specialization.arguments, values.get(),
                    iterator);
                if (!result)
                    fail(Code::ArithmeticFailure, index, {}, result.error());
                values[i] = *result;
                if (phase == Phase::Element) {
                    ++stats.element_nodes_evaluated;
                    if (i >= range.first && i - range.first < retained) {
                        details.push_back({index, *result});
                        ++stats.detail_records;
                    }
                } else
                    ++stats.bound_nodes_evaluated;
            }
            return values[recipe.nodes.size() - 1];
        };
        phase = Phase::Begin;
        const auto first = std::get<IntegerValue>(evaluate(d.begin, {})).bits();
        charge();
        if (first != record.observation.first)
            fail(Code::BoundsMismatch);
        phase = Phase::End;
        const auto last = std::get<IntegerValue>(evaluate(d.end, {})).bits();
        charge();
        if (last != record.observation.last || last <= first ||
            last - first != d.result_type.length)
            fail(Code::BoundsMismatch);
        // Increasing u32 bounds plus index < length guarantee no overflow.
        const auto iteration =
            static_cast<std::uint32_t>(first + element_index);
        phase = Phase::Element;
        const auto result = evaluate(d.element, IntegerValue(iteration));
        charge();
        ScalarConstant expected;
        if (d.element.result_type == sema::TypeKind::Bool)
            expected = static_cast<bool>(
                std::get<std::vector<bool>>(record.value.value)[element_index]);
        else
            expected = std::get<std::vector<IntegerValue>>(
                record.value.value)[element_index];
        if (result != expected)
            fail(Code::ResultMismatch);
        cancelled();
        return VerifiedGeneratorReplay{
            record.observation.descriptor,
            record.observation.expected_result,
            element_index,
            iteration,
            {d.element.result_type, from_scalar(result)},
            std::move(details),
            stats};
    } catch (const GeneratorReplayExecutionError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(GeneratorReplayExecutionError{
            Code::OutOfMemory, stats, phase, {}, {}, {}});
    } catch (const std::length_error&) {
        return std::unexpected(GeneratorReplayExecutionError{
            Code::ValueMemoryLimit, stats, phase, {}, {}, {}});
    }
}
} // namespace pagos::stage
