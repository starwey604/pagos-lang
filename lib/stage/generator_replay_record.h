#pragma once

#include "pagos/stage/generator_replay_record.h"

namespace pagos::stage::generator_detail {
[[nodiscard]] cache::Digest semantic(std::span<const std::uint8_t> value,
                                     std::span<const std::uint8_t> descriptor,
                                     std::span<const std::uint8_t> observation);
[[nodiscard]] std::expected<void, ReplayError>
validate(const GeneratorReplayAdmission& admission,
         const GeneratorReplayObservation& observation,
         const cache::TypedValue& value, cache::Limits limits);
// Admission has already been established from the current checked source.
[[nodiscard]] std::expected<GeneratorReplayRecord, ScalarReplayError>
load(const cache::FileStore& store, const GeneratorReplayAdmission& admission,
     cache::Limits limits = {});
} // namespace pagos::stage::generator_detail
