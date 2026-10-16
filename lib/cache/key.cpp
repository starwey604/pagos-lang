#include "pagos/cache/key.h"
#include "wire.h"

#include <algorithm>
#include <filesystem>

namespace pagos::cache {
bool valid_module_identity(const ModuleIdentity& module) {
    if (module.workspace.empty() || module.relative_path.empty() ||
        module.workspace.find('\0') != std::string::npos ||
        module.relative_path.find('\0') != std::string::npos ||
        module.relative_path.find('\\') != std::string::npos ||
        module.relative_path.find(':') != std::string::npos)
        return false;
    const std::filesystem::path path(module.relative_path);
    if (path.is_absolute() ||
        path.lexically_normal().generic_string() != module.relative_path)
        return false;
    for (const auto& part : path)
        if (part == ".." || part == "." || part.empty())
            return false;
    return true;
}
bool valid_definition_id(const StableDefinitionId& definition) {
    return valid_module_identity(definition.module) &&
           !definition.name.empty() &&
           definition.name.find('\0') == std::string::npos &&
           (definition.kind == DefinitionKind::Function ||
            definition.kind == DefinitionKind::Record ||
            definition.kind == DefinitionKind::Global ||
            definition.kind == DefinitionKind::Module);
}
namespace {
void module(wire::Writer& writer, const ModuleIdentity& identity) {
    writer.string(identity.workspace);
    writer.string(identity.relative_path);
}
ModuleIdentity module(wire::Reader& reader) {
    auto workspace = reader.string();
    auto relative_path = reader.string();
    return {std::move(workspace), std::move(relative_path)};
}
void context(wire::Writer& writer, const QueryContext& value) {
    module(writer, value.module);
    writer.string(value.target.triple);
    writer.string(value.target.cpu);
    writer.string(value.target.features);
    writer.string(value.target.data_layout);
    writer.u32(value.target.pointer_bits);
    writer.boolean(value.target.little_endian);
    writer.string(value.compiler_build);
    writer.string(value.semantic_options);
    writer.u32(value.query_version);
    writer.u32(value.budget_version);
}
QueryContext context(wire::Reader& reader) {
    QueryContext value;
    value.module = module(reader);
    value.target.triple = reader.string();
    value.target.cpu = reader.string();
    value.target.features = reader.string();
    value.target.data_layout = reader.string();
    value.target.pointer_bits = reader.u32();
    value.target.little_endian = reader.boolean();
    value.compiler_build = reader.string();
    value.semantic_options = reader.string();
    value.query_version = reader.u32();
    value.budget_version = reader.u32();
    if (!valid_context(value))
        reader.fail(Error::InvalidContext);
    if (value.query_version != query_protocol_version ||
        value.budget_version != budget_protocol_version)
        reader.fail(Error::IncompatibleVersion);
    return value;
}
bool valid_argument(const StageArgument& argument) {
    if (argument.stage == ArgumentStage::Comptime)
        return argument.value && argument.constraint == RuntimeConstraint::None;
    return argument.stage == ArgumentStage::Runtime && !argument.value &&
           (argument.constraint == RuntimeConstraint::None ||
            argument.constraint == RuntimeConstraint::ExplicitBoundary);
}
} // namespace
bool valid_context(const QueryContext& context) {
    return valid_module_identity(context.module) &&
           !context.target.triple.empty() && !context.target.cpu.empty() &&
           !context.target.data_layout.empty() &&
           (context.target.pointer_bits == 32 ||
            context.target.pointer_bits == 64) &&
           !context.compiler_build.empty() &&
           !context.semantic_options.empty() && context.query_version != 0 &&
           context.budget_version != 0;
}
std::expected<Bytes, Error> encode_query_key(const QueryKey& key,
                                             Limits limits) {
    if (!valid_context(key.context) || !valid_definition_id(key.definition) ||
        key.definition.module != key.context.module ||
        (key.definition.kind != DefinitionKind::Function &&
         !((key.kind == QueryKind::FreezeEvidence ||
            key.kind == QueryKind::FreezeEvidencePositions) &&
           key.definition.kind == DefinitionKind::Module)))
        return std::unexpected(Error::InvalidContext);
    if (key.kind != QueryKind::EvaluatePureSpecialization &&
        key.kind != QueryKind::SpecializeResidual &&
        key.kind != QueryKind::FreezeEvidence &&
        key.kind != QueryKind::FreezeEvidencePositions &&
        key.kind != QueryKind::FreezeFunctionEvidence)
        return std::unexpected(Error::InvalidValue);
    if ((key.kind == QueryKind::FreezeEvidence ||
         key.kind == QueryKind::FreezeEvidencePositions) &&
        (!key.arguments.empty() ||
         key.definition.kind != DefinitionKind::Module))
        return std::unexpected(Error::InvalidValue);
    wire::Writer writer(limits);
    wire::header(writer, ObjectKind::QueryKey);
    writer.u8(static_cast<std::uint8_t>(key.kind));
    context(writer, key.context);
    writer.u8(static_cast<std::uint8_t>(key.definition.kind));
    writer.string(key.definition.name);
    writer.raw(key.definition_fingerprint.bytes);
    writer.count(key.arguments.size());
    for (const auto& argument : key.arguments) {
        const auto scalar = argument.type.is_array()
                                ? argument.type.element_type()
                                : argument.type;
        if (scalar.kind == sema::TypeKind::Integer &&
            scalar.integer_type.is_usize &&
            scalar.integer_type.width != key.context.target.pointer_bits)
            return std::unexpected(Error::InvalidValue);
        if (!valid_argument(argument) ||
            (key.kind == QueryKind::EvaluatePureSpecialization &&
             argument.stage != ArgumentStage::Comptime))
            return std::unexpected(Error::InvalidValue);
        wire::type(writer, argument.type);
        writer.u8(static_cast<std::uint8_t>(argument.stage));
        writer.u8(static_cast<std::uint8_t>(argument.constraint));
        if (argument.value)
            wire::value(writer, {argument.type, *argument.value});
    }
    return writer.finish();
}
std::expected<QueryKey, Error>
decode_query_key(std::span<const std::uint8_t> bytes, Limits limits) {
    wire::Reader reader(bytes, limits);
    wire::header(reader, ObjectKind::QueryKey);
    QueryKey key;
    key.kind = static_cast<QueryKind>(reader.u8());
    if (key.kind != QueryKind::EvaluatePureSpecialization &&
        key.kind != QueryKind::SpecializeResidual &&
        key.kind != QueryKind::FreezeEvidence &&
        key.kind != QueryKind::FreezeEvidencePositions &&
        key.kind != QueryKind::FreezeFunctionEvidence)
        reader.fail(Error::InvalidFormat);
    key.context = context(reader);
    key.definition.module = key.context.module;
    key.definition.kind = static_cast<DefinitionKind>(reader.u8());
    key.definition.name = reader.string();
    if (!valid_definition_id(key.definition) ||
        (key.definition.kind != DefinitionKind::Function &&
         !((key.kind == QueryKind::FreezeEvidence ||
            key.kind == QueryKind::FreezeEvidencePositions) &&
           key.definition.kind == DefinitionKind::Module)))
        reader.fail(Error::InvalidContext);
    const auto hash = reader.raw(32);
    if (reader.good())
        std::ranges::copy(hash, key.definition_fingerprint.bytes.begin());
    const auto count = reader.count(3);
    key.arguments.reserve(count);
    for (std::uint32_t index = 0; index < count && reader.good(); ++index) {
        auto type = wire::type(reader);
        const auto stage = static_cast<ArgumentStage>(reader.u8());
        const auto constraint = static_cast<RuntimeConstraint>(reader.u8());
        std::optional<Constant> value;
        if (stage == ArgumentStage::Comptime)
            value = wire::value(reader, type);
        StageArgument argument{std::move(type), stage, std::move(value),
                               constraint};
        const auto scalar = argument.type.is_array()
                                ? argument.type.element_type()
                                : argument.type;
        if (scalar.kind == sema::TypeKind::Integer &&
            scalar.integer_type.is_usize &&
            scalar.integer_type.width != key.context.target.pointer_bits)
            reader.fail(Error::InvalidFormat);
        if (!valid_argument(argument) ||
            (key.kind == QueryKind::EvaluatePureSpecialization &&
             stage != ArgumentStage::Comptime))
            reader.fail(Error::InvalidFormat);
        key.arguments.push_back(std::move(argument));
    }
    if ((key.kind == QueryKind::FreezeEvidence ||
         key.kind == QueryKind::FreezeEvidencePositions) &&
        (!key.arguments.empty() ||
         key.definition.kind != DefinitionKind::Module))
        reader.fail(Error::InvalidFormat);
    if (const auto result = reader.finish(); !result)
        return std::unexpected(result.error());
    return key;
}
std::expected<Digest, Error> query_fingerprint(const QueryKey& key,
                                               Limits limits) {
    const auto bytes = encode_query_key(key, limits);
    if (!bytes)
        return std::unexpected(bytes.error());
    return fingerprint("pagos.query-key.v1", *bytes);
}
} // namespace pagos::cache
