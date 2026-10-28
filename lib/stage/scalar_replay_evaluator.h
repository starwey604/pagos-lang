#pragma once

#include "pagos/stage/replay_descriptor.h"

namespace pagos::stage::replay_detail {
// Requires prevalidated types, operations and slots. No allocation, Stage or
// IO. Iterator may only be supplied for a generator's admitted element recipe.
std::expected<ScalarConstant, IntegerError> evaluate(
    const ScalarReplayNode& node, std::span<const cache::StageArgument> inputs,
    const ScalarConstant* values, std::optional<IntegerValue> iterator = {});
} // namespace pagos::stage::replay_detail
