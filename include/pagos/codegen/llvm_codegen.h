#pragma once

#include "pagos/codegen/target.h"
#include "pagos/mir/mir.h"

#include <expected>
#include <string>

namespace pagos::codegen {

class LLVMCodegen {
  public:
    [[nodiscard]] static std::expected<std::string, std::string>
    emit(const mir::Module& mir_module, TargetConfig config = {});
    [[nodiscard]] static std::expected<std::string, std::string>
    emit_for_target(const mir::Module& mir_module, const TargetLayout& target);
};

} // namespace pagos::codegen
