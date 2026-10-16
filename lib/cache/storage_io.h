#pragma once

#include "pagos/cache/store.h"

namespace pagos::cache::detail {
// Shared bounded IO and atomic publication implementation; no semantic keys.
[[nodiscard]] ObjectRef object_reference(ObjectKind kind,
                                         std::span<const std::uint8_t> bytes);
[[nodiscard]] std::expected<Bytes, StoreError>
read_file(const StoreOptions& options, const std::filesystem::path& path,
          std::size_t maximum);
[[nodiscard]] std::expected<void, StoreError>
replace_file(const std::filesystem::path& root, const StoreOptions& options,
             const std::filesystem::path& destination,
             std::span<const std::uint8_t> bytes);
} // namespace pagos::cache::detail
