#pragma once

#include "pagos/cache/key.h"
#include "pagos/sema/type_checker.h"

namespace pagos::stage {

struct DefinitionIdentity {
    cache::StableDefinitionId id;
    cache::Digest semantic_fingerprint;
    std::vector<sema::Type> parameters; // Function signatures only.
    sema::Type result;
};
struct SourceAnchor {
    cache::StableDefinitionId definition;
    cache::Digest definition_fingerprint;
    // Preorder structural position, local to the unchanged definition.
    std::uint32_t position{};
    bool operator==(const SourceAnchor&) const = default;
};
struct AnchoredSpan {
    SourceAnchor anchor;
    source::Span span;
};
[[nodiscard]] std::expected<cache::Bytes, cache::Error>
encode_source_anchor(const SourceAnchor& anchor, cache::Limits limits = {});
[[nodiscard]] std::expected<SourceAnchor, cache::Error>
decode_source_anchor(std::span<const std::uint8_t> bytes,
                     cache::Limits limits = {});
// Owns no syntax/HIR pointers. File identity is supplied by QueryContext.
struct CacheIdentity {
    cache::ModuleIdentity module;
    cache::Digest module_fingerprint;
    std::vector<DefinitionIdentity> definitions;
    std::vector<AnchoredSpan> sites;

    [[nodiscard]] const DefinitionIdentity* find(cache::DefinitionKind kind,
                                                 std::string_view name) const;
    [[nodiscard]] std::expected<source::Span, cache::Error>
    resolve(const SourceAnchor& anchor) const;
};

// Requires a successfully checked AST and the matching target's type table.
// Missing semantic facts fail closed; no partial fingerprint is published.
[[nodiscard]] std::expected<CacheIdentity, cache::Error>
build_cache_identity(const syntax::Module& module, const sema::TypeTable& types,
                     const cache::QueryContext& context,
                     cache::Limits limits = {});
[[nodiscard]] std::expected<cache::QueryKey, cache::Error> make_query_key(
    const DefinitionIdentity& function, const cache::QueryContext& context,
    std::vector<cache::StageArgument> arguments,
    cache::QueryKind kind = cache::QueryKind::EvaluatePureSpecialization,
    cache::Limits limits = {});

} // namespace pagos::stage
