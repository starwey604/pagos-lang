#pragma once

#include "pagos/hir/hir.h"
#include "pagos/mir/mir.h"

#include <expected>
#include <string>

namespace pagos::mir {

[[nodiscard]] std::expected<Module, std::string>
lower(const hir::Module& module);

} // namespace pagos::mir
