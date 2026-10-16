#pragma once

#include "scalar_cache.h"

namespace pagos::stage {
// Closed scalar HIR only. IDs are slots in the current definition, never
// persisted HIR addresses, module symbol numbers or first-caller trace IDs.
[[nodiscard]] std::expected<cache::Bytes, cache::Error>
encode_residual_template(const hir::ExprPtr& body,
                         const syntax::Function& function,
                         const std::vector<hir::ExprPtr>& bindings,
                         const CacheIdentity& identity);
[[nodiscard]] std::expected<hir::ExprPtr, cache::Error>
decode_residual_template(std::span<const std::uint8_t> bytes,
                         const syntax::Function& function,
                         const std::vector<hir::ExprPtr>& bindings,
                         const CacheIdentity& identity,
                         const sema::TypeTable& types);
[[nodiscard]] std::expected<cache::Bytes, cache::Error>
residual_evidence(const hir::ExprPtr& body, const syntax::Function& function,
                  const std::vector<hir::ExprPtr>& bindings,
                  const CacheIdentity& identity,
                  const cache::QueryContext& context);
[[nodiscard]] cache::Digest
residual_digest(std::span<const std::uint8_t> body,
                std::span<const std::uint8_t> recipe,
                std::span<const std::uint8_t> evidence);
} // namespace pagos::stage
