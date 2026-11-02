#pragma once

#include "pagos/codegen/target.h"
#include "pagos/mir/mir.h"

#include <expected>
#include <optional>
#include <string>

namespace pagos::codegen {

// Emission policy is separate from TargetConfig: it cannot change semantic
// layout, Stage results, resource accounting, or persistent Stage cache keys.
enum class OptimizationLevel { Legacy, O0, O1, O2, O3, Os, Oz };
enum class RelocationModel { Pic, Static };
enum class CodeModel { Small, Medium };

struct EmissionOptions {
    // Legacy preserves raw IR and the existing backend's default optimization.
    OptimizationLevel optimization{OptimizationLevel::Legacy};
    std::optional<RelocationModel> relocation; // Unspecified preserves PIC.
    std::optional<CodeModel> code_model; // Unspecified uses LLVM's default.
    // Shared-library objects retain the same C ABI; linking remains external.
    bool shared_library{};
};

class LLVMCodegen {
  public:
    [[nodiscard]] static std::expected<std::string, std::string>
    emit(const mir::Module& mir_module, TargetConfig config = {},
         EmissionOptions options = {});
    [[nodiscard]] static std::expected<std::string, std::string>
    emit_for_target(const mir::Module& mir_module, const TargetLayout& target,
                    EmissionOptions options = {});
    // Binary object bytes, never text printed to a terminal.
    [[nodiscard]] static std::expected<std::string, std::string>
    emit_object_for_target(const mir::Module& mir_module,
                           const TargetLayout& target,
                           EmissionOptions options = {});
};

} // namespace pagos::codegen
