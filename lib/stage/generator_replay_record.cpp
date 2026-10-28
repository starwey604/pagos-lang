#include "generator_replay_record.h"
#include "replay_recipe.h"

#include <algorithm>
#include <stdexcept>

namespace pagos::stage {
namespace {
ReplayError invalid(cache::Error error = cache::Error::InvalidFormat) {
    return {error == cache::Error::IncompatibleVersion
                ? ReplayErrorCode::IncompatibleVersion
            : error == cache::Error::LimitExceeded
                ? ReplayErrorCode::LimitExceeded
                : ReplayErrorCode::InvalidFormat,
            error,
            {}};
}
ScalarReplayError failure(ReplayError error) {
    return {ScalarReplayErrorCode::InvalidDescriptor, error, {}};
}
template <class F>
auto guarded(F operation) -> std::expected<decltype(operation()), ReplayError> {
    try {
        return operation();
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
void shape(const GeneratorReplayObservation& value) {
    if (value.version != generator_replay_evidence_version)
        throw ReplayError{ReplayErrorCode::IncompatibleVersion, {}, {}};
    if (value.last <= value.first)
        throw ReplayError{ReplayErrorCode::InvalidRecipe, {}, {}};
}
} // namespace
cache::QueryKey generator_replay_key(cache::QueryKey key) {
    key.kind = cache::QueryKind::FreezeGeneratorReplay;
    key.context.semantic_options +=
        ";generator-replay-evidence.v" +
        std::to_string(generator_replay_evidence_version);
    return key;
}
std::expected<cache::Bytes, ReplayError>
encode_generator_replay_observation(const GeneratorReplayObservation& value,
                                    cache::Limits limits) {
    return guarded([&] {
        shape(value);
        cache::wire::Writer writer(limits);
        cache::wire::header(writer,
                            cache::ObjectKind::GeneratorReplayObservation);
        writer.u32(value.version);
        writer.raw(value.descriptor.bytes);
        writer.u32(value.first);
        writer.u32(value.last);
        writer.raw(value.expected_result.bytes);
        auto bytes = writer.finish();
        if (!bytes)
            throw invalid(bytes.error());
        return std::move(*bytes);
    });
}
std::expected<GeneratorReplayObservation, ReplayError>
decode_generator_replay_observation(std::span<const std::uint8_t> bytes,
                                    cache::Limits limits) {
    return guarded([&] {
        cache::wire::Reader reader(bytes, limits);
        cache::wire::header(reader,
                            cache::ObjectKind::GeneratorReplayObservation);
        GeneratorReplayObservation result;
        result.version = reader.u32();
        const auto descriptor = reader.raw(32);
        if (reader.good())
            std::ranges::copy(descriptor, result.descriptor.bytes.begin());
        result.first = reader.u32();
        result.last = reader.u32();
        const auto expected = reader.raw(32);
        if (reader.good())
            std::ranges::copy(expected, result.expected_result.bytes.begin());
        const auto finished = reader.finish();
        if (!finished)
            throw invalid(finished.error());
        shape(result);
        return result;
    });
}
std::expected<cache::Digest, ReplayError>
generator_replay_value_fingerprint(const cache::TypedValue& value,
                                   cache::Limits limits) {
    return guarded([&] {
        if (!value.type.is_array())
            throw ReplayError{ReplayErrorCode::UnsupportedType, {}, {}};
        auto bytes = cache::encode_value(value, limits);
        if (!bytes)
            throw invalid(bytes.error());
        return cache::fingerprint("pagos.generator-replay-value.v1", *bytes);
    });
}
namespace generator_detail {
cache::Digest semantic(std::span<const std::uint8_t> value,
                       std::span<const std::uint8_t> descriptor,
                       std::span<const std::uint8_t> observation) {
    cache::Bytes joined;
    for (const auto digest :
         {cache::fingerprint("pagos.generator-replay.value.v1", value),
          cache::fingerprint("pagos.generator-replay.descriptor.v1",
                             descriptor),
          cache::fingerprint("pagos.generator-replay.observation.v1",
                             observation)})
        joined.insert(joined.end(), digest.bytes.begin(), digest.bytes.end());
    return cache::fingerprint("pagos.generator-replay.evidence.v1", joined);
}
std::expected<void, ReplayError>
validate(const GeneratorReplayAdmission& admission,
         const GeneratorReplayObservation& observation,
         const cache::TypedValue& value, cache::Limits limits) {
    auto checked = guarded([&] {
        shape(observation);
        const auto& descriptor = admission.descriptor();
        auto fingerprint =
            generator_replay_descriptor_fingerprint(descriptor, limits);
        if (!fingerprint)
            throw fingerprint.error();
        if (*fingerprint != observation.descriptor)
            throw ReplayError{ReplayErrorCode::BindingMismatch, {}, {}};
        auto expected = generator_replay_value_fingerprint(value, limits);
        if (!expected)
            throw expected.error();
        if (value.type != descriptor.result_type ||
            value.type.length != observation.last - observation.first ||
            *expected != observation.expected_result)
            throw ReplayError{ReplayErrorCode::ResultMismatch, {}, {}};
        return true;
    });
    if (!checked)
        return std::unexpected(checked.error());
    return {};
}
std::expected<GeneratorReplayRecord, ScalarReplayError>
load(const cache::FileStore& store, const GeneratorReplayAdmission& admission,
     cache::Limits limits) {
    try {
        const auto& current = admission.descriptor();
        auto candidate =
            store.lookup(generator_replay_key(current.specialization),
                         current.verified_inputs);
        if (!candidate)
            return std::unexpected(ScalarReplayError{
                candidate.error().code == cache::StoreErrorCode::NotFound
                    ? ScalarReplayErrorCode::MissingDescriptor
                    : ScalarReplayErrorCode::StorageFailure,
                {},
                candidate.error()});
        const cache::StoredArtifact *value = nullptr, *descriptor = nullptr,
                                    *observation = nullptr;
        for (const auto& artifact : candidate->artifacts) {
            if (!artifact.required || !artifact.bytes)
                return std::unexpected(failure(invalid()));
            if (artifact.name == "value" &&
                artifact.object.kind == cache::ObjectKind::Value)
                value = &artifact;
            else if (artifact.name == "generator" &&
                     artifact.object.kind ==
                         cache::ObjectKind::GeneratorReplayDescriptor)
                descriptor = &artifact;
            else if (artifact.name == "observation" &&
                     artifact.object.kind ==
                         cache::ObjectKind::GeneratorReplayObservation)
                observation = &artifact;
            else
                return std::unexpected(failure(invalid()));
        }
        if (!value || !descriptor || !observation)
            return std::unexpected(ScalarReplayError{
                ScalarReplayErrorCode::MissingDescriptor, {}, {}});
        auto decoded =
            decode_generator_replay_descriptor(*descriptor->bytes, limits);
        auto observed =
            decode_generator_replay_observation(*observation->bytes, limits);
        auto array = cache::decode_value(*value->bytes, limits);
        if (!decoded || !observed || !array)
            return std::unexpected(failure(!decoded ? decoded.error()
                                           : !observed
                                               ? observed.error()
                                               : invalid(array.error())));
        if (*decoded != current ||
            semantic(*value->bytes, *descriptor->bytes, *observation->bytes) !=
                candidate->semantic_result)
            return std::unexpected(
                failure({ReplayErrorCode::BindingMismatch, {}, {}}));
        auto valid = validate(admission, *observed, *array, limits);
        if (!valid)
            return std::unexpected(failure(valid.error()));
        return GeneratorReplayRecord{admission, *observed, std::move(*array),
                                     observation->object};
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure({ReplayErrorCode::OutOfMemory, {}, {}}));
    } catch (const std::length_error&) {
        return std::unexpected(
            failure({ReplayErrorCode::LimitExceeded, {}, {}}));
    }
}
} // namespace generator_detail
std::expected<GeneratorReplayRecord, ScalarReplayError> load_generator_replay(
    const cache::FileStore& store, const syntax::Module& module,
    const sema::TypeTable& types, const cache::QueryKey& specialization,
    cache::Limits limits) {
    auto admitted =
        admit_generator_replay(module, types, specialization, limits);
    if (!admitted)
        return std::unexpected(ScalarReplayError{
            ScalarReplayErrorCode::UnsupportedSource, admitted.error(), {}});
    return generator_detail::load(store, *admitted, limits);
}
} // namespace pagos::stage
