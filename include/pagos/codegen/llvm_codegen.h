#pragma once

#include "pagos/mir/mir.h"

#include <expected>
#include <string>

namespace pagos::codegen {

class LLVMCodegen {
  public:
    [[nodiscard]] static std::expected<std::string, std::string>
    emit(const mir::Module& mir_module);
};

} // namespace pagos::codegen
