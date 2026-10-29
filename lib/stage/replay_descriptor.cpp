#include "replay_recipe.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace pagos::stage {
namespace {
using cache::wire::Reader;
using cache::wire::Writer;
using Op = ScalarReplayOp;
[[noreturn]] void fail(ReplayErrorCode code,
                       std::optional<std::uint32_t> node = {}) {
    throw ReplayError{code, {}, node};
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
void finish(Reader& reader) {
    if (auto result = reader.finish(); !result)
        throw codec_error(result.error());
}
cache::Digest digest(Reader& reader) {
    cache::Digest value;
    const auto data = reader.raw(32);
    if (reader.good())
        std::ranges::copy(data, value.bytes.begin());
    return value;
}
void blob(Writer& writer, std::span<const std::uint8_t> bytes) {
    writer.u64(bytes.size());
    writer.raw(bytes);
}
std::span<const std::uint8_t> blob(Reader& reader) {
    const auto length = reader.u64();
    if (length > reader.limits().bytes)
        fail(ReplayErrorCode::LimitExceeded);
    return reader.raw(static_cast<std::size_t>(length));
}
bool scalar(const sema::Type& type) {
    if (type.kind == sema::TypeKind::Bool)
        return type == sema::Type(sema::TypeKind::Bool);
    if (type.kind != sema::TypeKind::Integer || !type.integer_type.valid())
        return false;
    const auto width = type.integer_type.width;
    if (width != 8 && width != 16 && width != 32 && width != 64)
        return false;
    auto canonical = sema::Type::integer(width, type.integer_type.is_signed);
    canonical.integer_type.is_usize = type.integer_type.is_usize;
    return type == canonical;
}
void scalar_type(const sema::Type& type, unsigned pointer_bits,
                 std::optional<std::uint32_t> node = {}) {
    if (!scalar(type))
        fail(ReplayErrorCode::UnsupportedType, node);
    if (type.kind == sema::TypeKind::Integer && type.integer_type.is_usize &&
        type.integer_type.width != pointer_bits)
        fail(ReplayErrorCode::InvalidRecipe, node);
}
unsigned arity(Op operation) {
    switch (operation) {
    case Op::Input:
    case Op::Constant:
        return 0;
    case Op::Negate:
    case Op::BitNot:
    case Op::Not:
        return 1;
    case Op::Add:
    case Op::Subtract:
    case Op::Multiply:
    case Op::Divide:
    case Op::Remainder:
    case Op::BitAnd:
    case Op::BitOr:
    case Op::BitXor:
    case Op::ShiftLeft:
    case Op::ShiftRight:
    case Op::Equal:
    case Op::NotEqual:
    case Op::Less:
    case Op::LessEqual:
    case Op::Greater:
    case Op::GreaterEqual:
        return 2;
    }
    fail(ReplayErrorCode::UnsupportedOperation);
}
// Deliberately does not evaluate even constant-only operations. Divide/shift
// failures and actual result equality belong to the separately budgeted engine.
void shape(const ReplayDescriptor& value, cache::Limits limits) {
    if (value.version != replay_descriptor_version ||
        value.semantics != scalar_recipe_semantics_version ||
        value.specialization.context.query_version !=
            cache::query_protocol_version ||
        value.specialization.context.budget_version !=
            cache::budget_protocol_version)
        fail(ReplayErrorCode::IncompatibleVersion);
    if (value.nodes.size() > limits.elements || value.nodes.size() > UINT32_MAX)
        fail(ReplayErrorCode::LimitExceeded);
    const auto& key = value.specialization;
    if (key.arguments.size() > limits.elements)
        fail(ReplayErrorCode::LimitExceeded);
    if (key.kind != cache::QueryKind::EvaluatePureSpecialization)
        fail(ReplayErrorCode::InvalidRecipe);
    if (value.result_site.definition != key.definition ||
        value.result_site.definition_fingerprint != key.definition_fingerprint)
        fail(ReplayErrorCode::InvalidAnchor);
    const auto bits = key.context.target.pointer_bits;
    scalar_type(value.result_type, bits);
    for (const auto& argument : key.arguments)
        scalar_type(argument.type, bits);
    (void)checked(cache::encode_query_key(key, limits));
    std::vector<sema::Type> inputs;
    inputs.reserve(key.arguments.size());
    for (const auto& argument : key.arguments)
        inputs.push_back(argument.type);
    replay_detail::check_nodes(value.nodes, value.result_type,
                               value.result_site.position, inputs, bits,
                               limits);
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
} // namespace
namespace replay_detail {
void check_scalar_type(const sema::Type& type, unsigned bits,
                       std::optional<std::uint32_t> node) {
    scalar_type(type, bits, node);
}
void check_nodes(std::span<const ScalarReplayNode> nodes,
                 const sema::Type& result_type, std::uint32_t result_position,
                 std::span<const sema::Type> inputs, unsigned bits,
                 cache::Limits limits) {
    if (nodes.size() > limits.elements || nodes.size() > UINT32_MAX)
        fail(ReplayErrorCode::LimitExceeded);
    scalar_type(result_type, bits);
    if (nodes.empty())
        fail(ReplayErrorCode::InvalidRecipe);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        const auto& node = nodes[i];
        scalar_type(node.type, bits, index);
        const auto count = arity(node.operation);
        if (node.operands.size() != count ||
            node.input.has_value() != (node.operation == Op::Input) ||
            node.constant.has_value() != (node.operation == Op::Constant))
            fail(ReplayErrorCode::InvalidRecipe, index);
        for (const auto operand : node.operands)
            if (operand >= i)
                fail(ReplayErrorCode::InvalidReference, index);
        if (node.operation == Op::Input) {
            if (*node.input >= inputs.size())
                fail(ReplayErrorCode::InvalidReference, index);
            if (node.type != inputs[*node.input])
                fail(ReplayErrorCode::InvalidRecipe, index);
        } else if (node.operation == Op::Constant) {
            (void)checked(
                cache::encode_value({node.type, *node.constant}, limits));
        } else {
            const auto& left = nodes[node.operands[0]].type;
            const bool boolean = node.operation == Op::Not;
            const bool equality =
                node.operation == Op::Equal || node.operation == Op::NotEqual;
            const bool comparison = node.operation >= Op::Less &&
                                    node.operation <= Op::GreaterEqual;
            if ((!boolean && !equality &&
                 left.kind != sema::TypeKind::Integer) ||
                (boolean && left.kind != sema::TypeKind::Bool) ||
                (count == 2 && left != nodes[node.operands[1]].type) ||
                node.type != (equality || comparison
                                  ? sema::Type(sema::TypeKind::Bool)
                                  : left))
                fail(ReplayErrorCode::InvalidRecipe, index);
        }
    }
    if (nodes.back().type != result_type ||
        nodes.back().position != result_position)
        fail(ReplayErrorCode::InvalidRecipe);
    std::vector<bool> needed(nodes.size());
    needed.back() = true;
    for (std::size_t i = nodes.size(); i-- > 0;) {
        if (!needed[i])
            fail(ReplayErrorCode::InvalidRecipe, static_cast<std::uint32_t>(i));
        for (const auto operand : nodes[i].operands)
            needed[operand] = true;
    }
}
void write_nodes(Writer& writer, std::span<const ScalarReplayNode> nodes) {
    writer.count(nodes.size());
    for (const auto& node : nodes) {
        writer.u8(static_cast<std::uint8_t>(node.operation));
        cache::wire::type(writer, node.type);
        writer.u32(node.position);
        if (node.input)
            writer.u32(*node.input);
        if (node.constant)
            cache::wire::value(writer, {node.type, *node.constant});
        for (const auto operand : node.operands)
            writer.u32(operand);
    }
}
std::vector<ScalarReplayNode> read_nodes(Reader& reader, unsigned bits,
                                         std::size_t maximum_nodes) {
    const auto count = reader.count(6);
    if (!reader.good())
        finish(reader);
    if (count > maximum_nodes)
        fail(ReplayErrorCode::LimitExceeded);
    std::vector<ScalarReplayNode> nodes;
    nodes.reserve(count);
    for (std::uint32_t i = 0; i < count && reader.good(); ++i) {
        const auto operation =
            cache::wire::enum_range(reader, Op::Input, Op::GreaterEqual);
        const auto operands = arity(operation);
        auto node_type = cache::wire::type(reader);
        if (!reader.good())
            finish(reader);
        scalar_type(node_type, bits, i);
        ScalarReplayNode node{.operation = operation,
                              .type = node_type,
                              .position = reader.u32()};
        if (operation == Op::Input)
            node.input = reader.u32();
        if (operation == Op::Constant)
            node.constant = cache::wire::value(reader, node_type);
        for (unsigned j = 0; j < operands; ++j)
            node.operands.push_back(reader.u32());
        nodes.push_back(std::move(node));
    }
    if (!reader.good())
        finish(reader);
    return nodes;
}
} // namespace replay_detail
std::expected<cache::Digest, ReplayError>
replay_value_fingerprint(const cache::TypedValue& value, cache::Limits limits) {
    return guarded([&] {
        if (!scalar(value.type))
            fail(ReplayErrorCode::UnsupportedType);
        return cache::fingerprint("pagos.replay-value.v1",
                                  checked(cache::encode_value(value, limits)));
    });
}
std::expected<cache::Bytes, ReplayError>
encode_replay_descriptor(const ReplayDescriptor& value, cache::Limits limits) {
    return guarded([&] {
        shape(value, limits);
        Writer writer(limits);
        cache::wire::header(writer, cache::ObjectKind::ReplayDescriptor);
        writer.u32(value.version);
        writer.u32(value.semantics);
        blob(writer,
             checked(cache::encode_query_key(value.specialization, limits)));
        blob(writer, checked(encode_source_anchor(value.result_site, limits)));
        writer.raw(value.verified_inputs.bytes);
        cache::wire::type(writer, value.result_type);
        writer.raw(value.expected_result.bytes);
        replay_detail::write_nodes(writer, value.nodes);
        return checked(writer.finish());
    });
}
std::expected<ReplayDescriptor, ReplayError>
decode_replay_descriptor(std::span<const std::uint8_t> bytes,
                         cache::Limits limits) {
    return guarded([&] {
        Reader reader(bytes, limits);
        cache::wire::header(reader, cache::ObjectKind::ReplayDescriptor);
        ReplayDescriptor value;
        value.version = reader.u32();
        value.semantics = reader.u32();
        if (!reader.good())
            finish(reader);
        if (value.version != replay_descriptor_version ||
            value.semantics != scalar_recipe_semantics_version)
            fail(ReplayErrorCode::IncompatibleVersion);
        value.specialization =
            checked(cache::decode_query_key(blob(reader), limits));
        value.result_site = checked(decode_source_anchor(blob(reader), limits));
        value.verified_inputs = digest(reader);
        value.result_type = cache::wire::type(reader);
        if (!reader.good())
            finish(reader);
        scalar_type(value.result_type,
                    value.specialization.context.target.pointer_bits);
        value.expected_result = digest(reader);
        value.nodes = replay_detail::read_nodes(
            reader, value.specialization.context.target.pointer_bits);
        finish(reader);
        shape(value, limits);
        return value;
    });
}
std::expected<cache::Digest, ReplayError>
replay_descriptor_fingerprint(const ReplayDescriptor& value,
                              cache::Limits limits) {
    auto bytes = encode_replay_descriptor(value, limits);
    if (!bytes)
        return std::unexpected(bytes.error());
    return cache::fingerprint("pagos.replay-descriptor.v1", *bytes);
}
std::expected<void, ReplayError> validate_replay_descriptor(
    const ReplayDescriptor* value, const CacheIdentity& identity,
    const cache::QueryKey& current, const cache::Digest& inputs,
    const cache::TypedValue& expected, cache::Limits limits) {
    auto result = guarded([&] {
        if (!value)
            fail(ReplayErrorCode::MissingDescriptor);
        // Encoding also enforces the complete object byte/string limits.
        auto encoded = encode_replay_descriptor(*value, limits);
        if (!encoded)
            throw encoded.error();
        if (value->specialization != current ||
            value->verified_inputs != inputs ||
            identity.module != current.context.module)
            fail(ReplayErrorCode::BindingMismatch);
        const auto* definition = identity.find(cache::DefinitionKind::Function,
                                               current.definition.name);
        if (!definition || definition->id != current.definition ||
            definition->semantic_fingerprint != current.definition_fingerprint)
            fail(ReplayErrorCode::BindingMismatch);
        if (definition->parameters.size() != current.arguments.size() ||
            definition->result != value->result_type)
            fail(ReplayErrorCode::InvalidRecipe);
        for (std::size_t i = 0; i < current.arguments.size(); ++i)
            if (definition->parameters[i] != current.arguments[i].type)
                fail(ReplayErrorCode::InvalidRecipe);
        const auto fingerprint = replay_value_fingerprint(expected, limits);
        if (!fingerprint)
            throw fingerprint.error();
        if (expected.type != value->result_type ||
            *fingerprint != value->expected_result)
            fail(ReplayErrorCode::ResultMismatch);
        std::unordered_set<std::uint32_t> unresolved;
        for (const auto& node : value->nodes)
            unresolved.insert(node.position);
        for (const auto& site : identity.sites)
            if (site.anchor.definition == current.definition &&
                site.anchor.definition_fingerprint ==
                    current.definition_fingerprint &&
                site.span.begin <= site.span.end)
                unresolved.erase(site.anchor.position);
        if (!unresolved.empty())
            fail(ReplayErrorCode::InvalidAnchor);
        return true;
    });
    if (!result)
        return std::unexpected(result.error());
    return {};
}
} // namespace pagos::stage
