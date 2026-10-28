#pragma once

#include "pagos/cache/store.h"
#include "pagos/stage/generator_replay.h"
#include "pagos/stage/scalar_replay.h"

namespace pagos::stage {
inline constexpr std::uint32_t generator_replay_evidence_version = 1;
// Observed by the real evaluator, not inferred from the resulting array.
struct GeneratorReplayObservation {
    std::uint32_t version{generator_replay_evidence_version};
    cache::Digest descriptor;
    std::uint32_t first{}, last{};
    cache::Digest expected_result; // Canonical typed ARRAY value fingerprint.
    bool operator==(const GeneratorReplayObservation&) const = default;
};
struct GeneratorReplayRecord {
    GeneratorReplayAdmission admission;
    GeneratorReplayObservation observation;
    cache::TypedValue value;
    cache::ObjectRef object; // Observation CAS reference, for diagnostics.
};
[[nodiscard]] cache::QueryKey generator_replay_key(cache::QueryKey key);
[[nodiscard]] std::expected<cache::Bytes, ReplayError>
encode_generator_replay_observation(const GeneratorReplayObservation& value,
                                    cache::Limits limits = {});
[[nodiscard]] std::expected<GeneratorReplayObservation, ReplayError>
decode_generator_replay_observation(std::span<const std::uint8_t> bytes,
                                    cache::Limits limits = {});
[[nodiscard]] std::expected<cache::Digest, ReplayError>
generator_replay_value_fingerprint(const cache::TypedValue& value,
                                   cache::Limits limits = {});
// Checked source is re-admitted. No Stage, arithmetic, writes or fallback to
// an ordinary C2 array. Does not claim the recipe's math matches recorded data:
// the separately budgeted executor verifies bounds and the selected element.
[[nodiscard]] std::expected<GeneratorReplayRecord, ScalarReplayError>
load_generator_replay(const cache::FileStore& store,
                      const syntax::Module& module,
                      const sema::TypeTable& types,
                      const cache::QueryKey& specialization,
                      cache::Limits limits = {});
} // namespace pagos::stage
