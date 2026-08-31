#pragma once

#include "pagos/hir/hir.h"

#include <expected>
#include <string>

namespace pagos::codegen {

class LLVMCodegen {
  public:
    [[nodiscard]] static std::expected<std::string, std::string>
    emit(const hir::Module& hir_module);
};

} // namespace pagos::codegen
