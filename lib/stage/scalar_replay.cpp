#include "scalar_replay.h"
#include "../cache/wire.h"
#include "replay_recipe.h"

#include <algorithm>
#include <stdexcept>

namespace pagos::stage {
namespace {
[[noreturn]] void unsupported() {
    throw ReplayError{ReplayErrorCode::UnsupportedOperation, {}, {}};
}
ScalarReplayError invalid(ReplayError error) {
    return {ScalarReplayErrorCode::InvalidDescriptor, error, {}};
}
} // namespace
cache::QueryKey scalar_replay_key(cache::QueryKey specialization) {
    specialization.kind = cache::QueryKind::FreezeScalarReplay;
    specialization.context.semantic_options +=
        ";scalar-replay-evidence.v" +
        std::to_string(scalar_replay_evidence_version);
    return specialization;
}
namespace replay_detail {
std::expected<ReplayDescriptor, ReplayError>
recipe(const syntax::Function& function, const sema::TypeTable& types,
       const CacheIdentity& identity, const cache::QueryKey& key,
       cache::Limits limits) {
    try {
        if (function.linkage != syntax::Function::Linkage::Internal ||
            !function.body || !function.body->statements.empty() ||
            !function.body->tail || function.name != key.definition.name ||
            key.kind != cache::QueryKind::EvaluatePureSpecialization)
            unsupported();
        const auto* definition =
            identity.find(cache::DefinitionKind::Function, function.name);
        if (!definition)
            throw ReplayError{ReplayErrorCode::BindingMismatch, {}, {}};
        const auto actual = make_query_key(*definition, key.context,
                                           key.arguments, key.kind, limits);
        if (!actual || *actual != key)
            throw ReplayError{ReplayErrorCode::BindingMismatch, {}, {}};
        auto expression = replay_detail::build_recipe(
            *function.body->tail, function, types, identity, key, {}, limits);
        ReplayDescriptor output{.specialization = key,
                                .verified_inputs = identity.module_fingerprint,
                                .result_site =
                                    std::move(expression.result_site),
                                .result_type = types.resolve(function.result),
                                .nodes = std::move(expression.nodes)};
        const auto bytes = encode_replay_descriptor(output, limits);
        if (!bytes)
            return std::unexpected(bytes.error());
        return output;
    } catch (const ReplayError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            ReplayError{ReplayErrorCode::OutOfMemory, {}, {}});
    } catch (const std::length_error&) {
        return std::unexpected(
            ReplayError{ReplayErrorCode::LimitExceeded, {}, {}});
    }
}
cache::Digest semantic(std::span<const std::uint8_t> value,
                       std::span<const std::uint8_t> descriptor) {
    // Hash each canonical payload first: fixed-size join, independent domains.
    cache::Bytes joined;
    for (const auto hash :
         {cache::fingerprint("pagos.scalar-replay.value.v1", value),
          cache::fingerprint("pagos.scalar-replay.descriptor.v1", descriptor)})
        joined.insert(joined.end(), hash.bytes.begin(), hash.bytes.end());
    return cache::fingerprint("pagos.scalar-replay.evidence.v1", joined);
}
std::expected<ScalarReplayRecord, ScalarReplayError>
load(const cache::FileStore& store, const CacheIdentity& identity,
     const ReplayDescriptor& current, cache::Limits limits) {
    try {
        auto candidate = store.lookup(scalar_replay_key(current.specialization),
                                      current.verified_inputs);
        if (!candidate)
            return std::unexpected(ScalarReplayError{
                candidate.error().code == cache::StoreErrorCode::NotFound
                    ? ScalarReplayErrorCode::MissingDescriptor
                    : ScalarReplayErrorCode::StorageFailure,
                {},
                candidate.error()});
        const cache::StoredArtifact *value = nullptr, *replay = nullptr;
        for (const auto& artifact : candidate->artifacts) {
            if (!artifact.required || !artifact.bytes)
                return std::unexpected(
                    invalid({ReplayErrorCode::InvalidFormat, {}, {}}));
            if (artifact.name == "value" &&
                artifact.object.kind == cache::ObjectKind::Value)
                value = &artifact;
            else if (artifact.name == "replay" &&
                     artifact.object.kind ==
                         cache::ObjectKind::ReplayDescriptor)
                replay = &artifact;
            else
                return std::unexpected(
                    invalid({ReplayErrorCode::InvalidFormat, {}, {}}));
        }
        if (!value || !replay)
            return std::unexpected(ScalarReplayError{
                ScalarReplayErrorCode::MissingDescriptor, {}, {}});
        auto decoded_value = cache::decode_value(*value->bytes, limits);
        auto descriptor = decode_replay_descriptor(*replay->bytes, limits);
        if (!decoded_value || !descriptor)
            return std::unexpected(
                invalid(descriptor ? ReplayError{ReplayErrorCode::InvalidFormat,
                                                 decoded_value.error(),
                                                 {}}
                                   : descriptor.error()));
        if (semantic(*value->bytes, *replay->bytes) !=
            candidate->semantic_result)
            return std::unexpected(
                invalid({ReplayErrorCode::ResultMismatch, {}, {}}));
        auto valid = validate_replay_descriptor(
            &*descriptor, identity, current.specialization,
            current.verified_inputs, *decoded_value, limits);
        if (!valid)
            return std::unexpected(invalid(valid.error()));
        // Match the entire typed source recipe, not merely its anchor or hash.
        if (descriptor->nodes != current.nodes ||
            descriptor->result_site != current.result_site ||
            descriptor->result_type != current.result_type)
            return std::unexpected(
                invalid({ReplayErrorCode::InvalidRecipe, {}, {}}));
        return ScalarReplayRecord{std::move(*descriptor),
                                  std::move(*decoded_value), replay->object};
    } catch (const std::bad_alloc&) {
        return std::unexpected(invalid({ReplayErrorCode::OutOfMemory, {}, {}}));
    } catch (const std::length_error&) {
        return std::unexpected(
            invalid({ReplayErrorCode::LimitExceeded, {}, {}}));
    }
}
} // namespace replay_detail
std::expected<ScalarReplayRecord, ScalarReplayError>
load_scalar_replay(const cache::FileStore& store, const syntax::Module& module,
                   const sema::TypeTable& types, const cache::QueryKey& key,
                   cache::Limits limits) {
    try {
        const auto identity =
            build_cache_identity(module, types, key.context, limits);
        if (!identity)
            return std::unexpected(ScalarReplayError{
                ScalarReplayErrorCode::UnsupportedSource,
                ReplayError{
                    ReplayErrorCode::BindingMismatch, identity.error(), {}},
                {}});
        const auto function = std::ranges::find(
            module.functions, key.definition.name,
            [](const auto& item) -> const std::string& { return item->name; });
        if (function == module.functions.end())
            return std::unexpected(ScalarReplayError{
                ScalarReplayErrorCode::UnsupportedSource, {}, {}});
        auto current =
            replay_detail::recipe(**function, types, *identity, key, limits);
        if (!current)
            return std::unexpected(ScalarReplayError{
                ScalarReplayErrorCode::UnsupportedSource, current.error(), {}});
        return replay_detail::load(store, *identity, *current, limits);
    } catch (const std::bad_alloc&) {
        return std::unexpected(invalid({ReplayErrorCode::OutOfMemory, {}, {}}));
    } catch (const std::length_error&) {
        return std::unexpected(
            invalid({ReplayErrorCode::LimitExceeded, {}, {}}));
    }
}
} // namespace pagos::stage
