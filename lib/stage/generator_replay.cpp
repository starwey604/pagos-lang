#include "pagos/stage/generator_replay.h"
#include "replay_recipe.h"

#include <algorithm>
#include <stdexcept>

namespace pagos::stage {
namespace {
using cache::wire::Reader;
using cache::wire::Writer;
[[noreturn]] void fail(ReplayErrorCode code) {
    throw ReplayError{code, {}, {}};
}
ReplayError codec_error(cache::Error error) {
    return {error == cache::Error::IncompatibleVersion
                ? ReplayErrorCode::IncompatibleVersion
            : error == cache::Error::LimitExceeded
                ? ReplayErrorCode::LimitExceeded
            : error == cache::Error::UnsupportedType
                ? ReplayErrorCode::UnsupportedType
                : ReplayErrorCode::InvalidFormat,
            error,
            {}};
}
template <class T> T checked(std::expected<T, cache::Error> value) {
    if (!value)
        throw codec_error(value.error());
    return std::move(*value);
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
void finish(Reader& reader) {
    if (auto result = reader.finish(); !result)
        throw codec_error(result.error());
}
void blob(Writer& writer, std::span<const std::uint8_t> bytes) {
    writer.u64(bytes.size());
    writer.raw(bytes);
}
std::span<const std::uint8_t> blob(Reader& reader) {
    const auto size = reader.u64();
    if (size > reader.limits().bytes)
        fail(ReplayErrorCode::LimitExceeded);
    return reader.raw(static_cast<std::size_t>(size));
}
void versions(const GeneratorReplayDescriptor& value) {
    if (value.version != generator_replay_descriptor_version ||
        value.semantics != closed_generator_semantics_version ||
        value.scalar_semantics != scalar_recipe_semantics_version ||
        value.specialization.context.query_version !=
            cache::query_protocol_version ||
        value.specialization.context.budget_version !=
            cache::budget_protocol_version)
        fail(ReplayErrorCode::IncompatibleVersion);
}
void anchor(const SourceAnchor& site, const cache::QueryKey& key) {
    if (site.definition != key.definition ||
        site.definition_fingerprint != key.definition_fingerprint)
        fail(ReplayErrorCode::InvalidAnchor);
}
void shape(const GeneratorReplayDescriptor& value, cache::Limits limits) {
    versions(value);
    const auto& key = value.specialization;
    if (key.kind != cache::QueryKind::EvaluatePureSpecialization)
        fail(ReplayErrorCode::InvalidRecipe);
    (void)checked(cache::encode_query_key(key, limits));
    if (key.arguments.size() >= UINT32_MAX ||
        key.arguments.size() > limits.elements)
        fail(ReplayErrorCode::LimitExceeded);
    if (value.iterator_slot != key.arguments.size())
        fail(ReplayErrorCode::InvalidReference);
    const auto bits = key.context.target.pointer_bits;
    if (!value.result_type.is_array() || value.result_type.length == 0 ||
        value.result_type != sema::Type::array(value.element.result_type,
                                               value.result_type.length))
        fail(ReplayErrorCode::UnsupportedType);
    if (value.begin.result_type != sema::Type::integer(32) ||
        value.end.result_type != sema::Type::integer(32))
        fail(ReplayErrorCode::UnsupportedType);
    std::vector<sema::Type> inputs;
    for (const auto& argument : key.arguments) {
        replay_detail::check_scalar_type(argument.type, bits);
        inputs.push_back(argument.type);
    }
    anchor(value.generator_site, key);
    anchor(value.iterator_site, key);
    auto remaining = limits.elements;
    for (const auto* recipe : {&value.begin, &value.end, &value.element}) {
        if (recipe->nodes.size() > remaining)
            fail(ReplayErrorCode::LimitExceeded);
        remaining -= recipe->nodes.size();
        anchor(recipe->result_site, key);
        if (recipe == &value.element)
            inputs.push_back(sema::Type::integer(32));
        replay_detail::check_nodes(recipe->nodes, recipe->result_type,
                                   recipe->result_site.position, inputs, bits,
                                   limits);
    }
}
void write_recipe(Writer& writer, const ScalarReplayRecipe& recipe,
                  cache::Limits limits) {
    blob(writer, checked(encode_source_anchor(recipe.result_site, limits)));
    cache::wire::type(writer, recipe.result_type);
    replay_detail::write_nodes(writer, recipe.nodes);
}
ScalarReplayRecipe read_recipe(Reader& reader, unsigned bits,
                               std::size_t& remaining) {
    ScalarReplayRecipe recipe;
    recipe.result_site =
        checked(decode_source_anchor(blob(reader), reader.limits()));
    recipe.result_type = cache::wire::type(reader);
    if (!reader.good())
        finish(reader);
    replay_detail::check_scalar_type(recipe.result_type, bits);
    // Check the aggregate count before allocating this recipe's nodes.
    recipe.nodes = replay_detail::read_nodes(reader, bits, remaining);
    if (recipe.nodes.size() > remaining)
        fail(ReplayErrorCode::LimitExceeded);
    remaining -= recipe.nodes.size();
    return recipe;
}
GeneratorReplayDescriptor describe(const syntax::Module& module,
                                   const sema::TypeTable& types,
                                   const cache::QueryKey& key,
                                   cache::Limits limits) {
    if (types.pointer_bits != key.context.target.pointer_bits)
        fail(ReplayErrorCode::BindingMismatch);
    auto identity =
        checked(build_cache_identity(module, types, key.context, limits));
    const auto found =
        std::ranges::find(module.functions, key.definition.name,
                          [](const auto& fn) { return fn->name; });
    if (found == module.functions.end() ||
        key.kind != cache::QueryKind::EvaluatePureSpecialization)
        fail(ReplayErrorCode::UnsupportedOperation);
    const auto& fn = **found;
    if (fn.linkage != syntax::Function::Linkage::Internal || !fn.body ||
        !fn.body->statements.empty() || !fn.body->tail ||
        fn.body->tail->kind != syntax::Expr::Kind::ArrayGenerator)
        fail(ReplayErrorCode::UnsupportedOperation);
    const auto* definition =
        identity.find(cache::DefinitionKind::Function, fn.name);
    if (!definition ||
        checked(make_query_key(*definition, key.context, key.arguments,
                               key.kind, limits)) != key)
        fail(ReplayErrorCode::BindingMismatch);
    const auto& generator =
        static_cast<const syntax::ArrayGeneratorExpr&>(*fn.body->tail);
    if (!generator.body || !generator.body->statements.empty() ||
        !generator.body->tail)
        fail(ReplayErrorCode::UnsupportedOperation);
    GeneratorReplayDescriptor result{
        .specialization = key,
        .verified_inputs = identity.module_fingerprint,
        .generator_site =
            replay_detail::source_anchor(identity, key, generator.span),
        .iterator_site = replay_detail::source_anchor(identity, key,
                                                      generator.variable_span),
        .iterator_slot = static_cast<std::uint32_t>(key.arguments.size()),
        .result_type = definition->result};
    auto budget = limits;
    const auto build = [&](const syntax::Expr& expression,
                           std::optional<std::string_view> iterator) {
        auto recipe = replay_detail::build_recipe(
            expression, fn, types, identity, key, iterator, budget);
        budget.elements -= recipe.nodes.size();
        return recipe;
    };
    result.begin = build(*generator.begin, {});
    result.end = build(*generator.end, {});
    result.element = build(*generator.body->tail, generator.variable);
    // Checks complete bytes, captures, scalar types, iterator scope and arity.
    const auto encoded = encode_generator_replay_descriptor(result, limits);
    if (!encoded)
        throw encoded.error();
    return result;
}
} // namespace
std::expected<cache::Bytes, ReplayError>
encode_generator_replay_descriptor(const GeneratorReplayDescriptor& value,
                                   cache::Limits limits) {
    return guarded([&] {
        shape(value, limits);
        Writer writer(limits);
        cache::wire::header(writer,
                            cache::ObjectKind::GeneratorReplayDescriptor);
        writer.u32(value.version);
        writer.u32(value.semantics);
        writer.u32(value.scalar_semantics);
        blob(writer,
             checked(cache::encode_query_key(value.specialization, limits)));
        writer.raw(value.verified_inputs.bytes);
        blob(writer,
             checked(encode_source_anchor(value.generator_site, limits)));
        blob(writer,
             checked(encode_source_anchor(value.iterator_site, limits)));
        writer.u32(value.iterator_slot);
        cache::wire::type(writer, value.result_type);
        for (const auto* recipe : {&value.begin, &value.end, &value.element})
            write_recipe(writer, *recipe, limits);
        return checked(writer.finish());
    });
}
std::expected<GeneratorReplayDescriptor, ReplayError>
decode_generator_replay_descriptor(std::span<const std::uint8_t> bytes,
                                   cache::Limits limits) {
    return guarded([&] {
        Reader reader(bytes, limits);
        cache::wire::header(reader,
                            cache::ObjectKind::GeneratorReplayDescriptor);
        GeneratorReplayDescriptor value;
        value.version = reader.u32();
        value.semantics = reader.u32();
        value.scalar_semantics = reader.u32();
        if (!reader.good())
            finish(reader);
        versions(value);
        value.specialization =
            checked(cache::decode_query_key(blob(reader), limits));
        const auto digest = reader.raw(32);
        if (reader.good())
            std::ranges::copy(digest, value.verified_inputs.bytes.begin());
        value.generator_site =
            checked(decode_source_anchor(blob(reader), limits));
        value.iterator_site =
            checked(decode_source_anchor(blob(reader), limits));
        value.iterator_slot = reader.u32();
        value.result_type = cache::wire::type(reader);
        if (!reader.good())
            finish(reader);
        auto remaining = limits.elements;
        const auto bits = value.specialization.context.target.pointer_bits;
        value.begin = read_recipe(reader, bits, remaining);
        value.end = read_recipe(reader, bits, remaining);
        value.element = read_recipe(reader, bits, remaining);
        finish(reader);
        shape(value, limits);
        return value;
    });
}
std::expected<cache::Digest, ReplayError>
generator_replay_descriptor_fingerprint(const GeneratorReplayDescriptor& value,
                                        cache::Limits limits) {
    auto bytes = encode_generator_replay_descriptor(value, limits);
    if (!bytes)
        return std::unexpected(bytes.error());
    return cache::fingerprint("pagos.generator-replay-descriptor.v1", *bytes);
}
std::expected<GeneratorReplayAdmission, ReplayError>
admit_generator_replay(const syntax::Module& module,
                       const sema::TypeTable& types, const cache::QueryKey& key,
                       cache::Limits limits) {
    return guarded([&] {
        return GeneratorReplayAdmission(describe(module, types, key, limits));
    });
}
std::expected<GeneratorReplayAdmission, ReplayError>
validate_generator_replay(const GeneratorReplayDescriptor* descriptor,
                          const syntax::Module& module,
                          const sema::TypeTable& types,
                          const cache::QueryKey& key, cache::Limits limits) {
    if (!descriptor)
        return std::unexpected(
            ReplayError{ReplayErrorCode::MissingDescriptor, {}, {}});
    const auto bytes = encode_generator_replay_descriptor(*descriptor, limits);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto actual = admit_generator_replay(module, types, key, limits);
    if (!actual)
        return actual;
    if (actual->descriptor() != *descriptor)
        return std::unexpected(
            ReplayError{ReplayErrorCode::BindingMismatch, {}, {}});
    return actual;
}
} // namespace pagos::stage
