#pragma once

#include "pagos/stage/scalar_replay.h"

namespace pagos::stage::replay_detail {
// Draft expected_result is zero until successful real Stage evaluation.
[[nodiscard]] std::expected<ReplayDescriptor, ReplayError>
recipe(const syntax::Function& function, const sema::TypeTable& types,
       const CacheIdentity& identity, const cache::QueryKey& specialization,
       cache::Limits limits = {});
[[nodiscard]] std::expected<ScalarReplayRecord, ScalarReplayError>
load(const cache::FileStore& store, const CacheIdentity& identity,
     const ReplayDescriptor& current, cache::Limits limits = {});
[[nodiscard]] cache::Digest semantic(std::span<const std::uint8_t> value,
                                     std::span<const std::uint8_t> descriptor);
} // namespace pagos::stage::replay_detail
