#pragma once

#include "pagos/mir/mir.h"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace pagos::codegen {

struct TargetConfig {
    std::string triple; // Empty selects the host triple, not host C++ layout.
    std::string cpu;    // Empty selects the target's generic CPU.
    std::string features;
    bool operator==(const TargetConfig&) const = default;
};

struct TypeLayout {
    std::uint64_t size_bytes{};
    std::uint64_t alignment_bytes{};
    std::vector<std::uint64_t> field_offsets;
};

// Immutable per-session layout, never a process-global target-dependent cache.
// Layout describes storage only; it is not a C calling-convention classifier.
class TargetLayout {
  public:
    [[nodiscard]] static std::expected<TargetLayout, std::string>
    create(TargetConfig config = {});
    [[nodiscard]] const TargetConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] const std::string& data_layout() const noexcept {
        return layout_;
    }
    [[nodiscard]] unsigned pointer_bits() const;
    [[nodiscard]] bool little_endian() const;
    [[nodiscard]] bool supports_minimal_c_abi() const;
    [[nodiscard]] std::expected<TypeLayout, std::string>
    layout_of(const mir::Type& type) const;

  private:
    TargetLayout(TargetConfig config, std::string layout)
        : config_(std::move(config)), layout_(std::move(layout)) {}
    TargetConfig config_;
    std::string layout_;
};

} // namespace pagos::codegen
