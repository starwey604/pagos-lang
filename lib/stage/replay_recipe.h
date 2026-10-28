#pragma once

#include "../cache/wire.h"
#include "pagos/stage/replay_descriptor.h"

namespace pagos::stage::replay_detail {
// Private, throwing helpers. Public entry points translate ReplayError and
// allocation failures into expected. Scalar descriptor v1's wire is unchanged.
void check_scalar_type(const sema::Type& type, unsigned pointer_bits,
                       std::optional<std::uint32_t> node = {});
void check_nodes(std::span<const ScalarReplayNode> nodes,
                 const sema::Type& result_type, std::uint32_t result_position,
                 std::span<const sema::Type> inputs, unsigned pointer_bits,
                 cache::Limits limits);
void write_nodes(cache::wire::Writer& writer,
                 std::span<const ScalarReplayNode> nodes);
std::vector<ScalarReplayNode> read_nodes(cache::wire::Reader& reader,
                                         unsigned pointer_bits,
                                         std::size_t maximum_nodes = SIZE_MAX);
SourceAnchor source_anchor(const CacheIdentity& identity,
                           const cache::QueryKey& key, source::Span span);
// Iterator is in scope only when building a generator's body. It shadows a
// parameter of the same name and always occupies slot key.arguments.size().
ScalarReplayRecipe
build_recipe(const syntax::Expr& expression, const syntax::Function& function,
             const sema::TypeTable& types, const CacheIdentity& identity,
             const cache::QueryKey& key,
             std::optional<std::string_view> iterator, cache::Limits limits);
} // namespace pagos::stage::replay_detail
