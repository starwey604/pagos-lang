#include "pagos/stage/cache_identity.h"
#include "../cache/wire.h"

#include <algorithm>
#include <unordered_set>

namespace pagos::stage {
namespace {
using cache::DefinitionKind;
using cache::Error;
using cache::wire::Writer;

// Versioned AST schema: ordered children and explicit bindings are retained.
// Pointer-keyed side tables are looked up, never iterated or serialized.
class IdentityBuilder {
  public:
    IdentityBuilder(const syntax::Module& module, const sema::TypeTable& types,
                    const cache::QueryContext& context, cache::Limits limits)
        : module_(module), types_(types), context_(context), limits_(limits),
          writer_(limits) {}

    std::expected<CacheIdentity, Error> build() {
        output_.module = context_.module;
        Writer module_writer(limits_);
        module_writer.u32(cache::query_protocol_version);
        module_writer.u32(types_.pointer_bits);
        for (const auto& record : module_.records) {
            begin(DefinitionKind::Record, record.name);
            site(record.name_span);
            writer_.count(record.fields.size());
            for (const auto& field : record.fields)
                parameter(field);
            if (!finish(module_writer))
                return std::unexpected(error_);
        }
        for (const auto& global : module_.globals) {
            begin(DefinitionKind::Global, global.name);
            site(global.name_span);
            semantic_type(types_.resolve(global.type));
            writer_.boolean(global.writable);
            writer_.string(global.storage.section);
            writer_.u32(global.storage.alignment);
            expression(global.initializer.get(), 0);
            if (!finish(module_writer))
                return std::unexpected(error_);
        }
        for (const auto& function : module_.functions) {
            begin(DefinitionKind::Function, function->name);
            site(function->span);
            site(function->name_span);
            writer_.u8(static_cast<std::uint8_t>(function->linkage));
            writer_.count(function->parameters.size());
            for (const auto& value : function->parameters) {
                parameter(value);
                current_.parameters.push_back(types_.resolve(value.type));
            }
            current_.result = types_.resolve(function->result);
            semantic_type(current_.result);
            writer_.boolean(function->body != nullptr);
            if (function->body)
                block(*function->body, 0);
            if (!finish(module_writer))
                return std::unexpected(error_);
        }
        begin(DefinitionKind::Module, "$module");
        writer_.count(module_.statements.size());
        for (const auto& value : module_.statements)
            statement(*value, 0);
        if (!finish(module_writer))
            return std::unexpected(error_);
        const auto bytes = module_writer.finish();
        if (!bytes)
            return std::unexpected(bytes.error());
        output_.module_fingerprint =
            cache::fingerprint("pagos.module-semantics.v1", *bytes);
        return std::move(output_);
    }

  private:
    void begin(DefinitionKind kind, const std::string& name) {
        writer_ = Writer(limits_);
        current_ = {.id = {context_.module, kind, name}};
        sites_.clear();
        writer_.u32(cache::query_protocol_version);
        writer_.u8(static_cast<std::uint8_t>(kind));
        writer_.string(name);
        const auto identity =
            std::to_string(static_cast<unsigned>(kind)) + ":" + name;
        if (name.empty() || !definitions_.insert(identity).second)
            writer_.fail(Error::MissingDefinition);
    }
    bool finish(Writer& module_writer) {
        const auto bytes = writer_.finish();
        if (!bytes) {
            error_ = bytes.error();
            return false;
        }
        current_.semantic_fingerprint =
            cache::fingerprint("pagos.definition-semantics.v1", *bytes);
        module_writer.u8(static_cast<std::uint8_t>(current_.id.kind));
        module_writer.string(current_.id.name);
        module_writer.raw(current_.semantic_fingerprint.bytes);
        for (std::size_t index = 0; index < sites_.size(); ++index)
            output_.sites.push_back(
                {{current_.id, current_.semantic_fingerprint,
                  static_cast<std::uint32_t>(index)},
                 sites_[index]});
        output_.definitions.push_back(std::move(current_));
        return true;
    }
    void site(source::Span span) {
        if (output_.sites.size() + sites_.size() >= limits_.elements ||
            sites_.size() >= UINT32_MAX) {
            writer_.fail(Error::LimitExceeded);
            return;
        }
        if (span.begin > span.end) {
            writer_.fail(Error::InvalidAnchor);
            return;
        }
        sites_.push_back(span);
    }
    bool enter(std::size_t depth) {
        if (depth >= limits_.depth) {
            writer_.fail(Error::LimitExceeded);
            return false;
        }
        return true;
    }
    void semantic_type(const sema::Type& type) {
        using enum sema::TypeKind;
        if (type.kind == Unknown || type.kind == Error) {
            writer_.fail(cache::Error::MissingType);
            return;
        }
        writer_.u8(static_cast<std::uint8_t>(type.kind));
        writer_.u32(type.integer_type.width);
        writer_.boolean(type.integer_type.is_signed);
        writer_.boolean(type.integer_type.is_usize);
        writer_.u32(type.pointer_bits);
        writer_.boolean(type.pointer_mutable);
        writer_.u8(static_cast<std::uint8_t>(type.element_kind));
        writer_.u32(type.length); // Deferred lengths are valid semantic facts.
        writer_.string(type.record_name);
    }
    void parameter(const syntax::Parameter& value) {
        site(value.name_span);
        writer_.string(value.name);
        semantic_type(types_.resolve(value.type));
    }
    void block(const syntax::Block& value, std::size_t depth) {
        if (!enter(depth))
            return;
        site(value.span);
        writer_.count(value.statements.size());
        for (const auto& child : value.statements)
            statement(*child, depth + 1);
        writer_.boolean(value.tail != nullptr);
        if (value.tail)
            expression(value.tail.get(), depth + 1);
    }
    void statement(const syntax::Stmt& value, std::size_t depth) {
        if (!enter(depth))
            return;
        site(value.span);
        writer_.u8(static_cast<std::uint8_t>(value.kind));
        using enum syntax::Stmt::Kind;
        switch (value.kind) {
        case Binding: {
            const auto& binding =
                static_cast<const syntax::BindingStmt&>(value);
            writer_.u8(static_cast<std::uint8_t>(binding.binding_kind));
            writer_.string(binding.name);
            site(binding.name_span);
            writer_.boolean(binding.annotation.has_value());
            if (binding.annotation)
                semantic_type(types_.resolve(*binding.annotation));
            expression(binding.initializer.get(), depth + 1);
            break;
        }
        case Return:
            expression(
                static_cast<const syntax::ReturnStmt&>(value).value.get(),
                depth + 1);
            break;
        case Expression:
            expression(static_cast<const syntax::ExpressionStmt&>(value)
                           .expression.get(),
                       depth + 1);
            break;
        case Store: {
            const auto& store = static_cast<const syntax::StoreStmt&>(value);
            writer_.boolean(store.is_volatile);
            expression(store.pointer.get(), depth + 1);
            expression(store.value.get(), depth + 1);
            break;
        }
        case For: {
            const auto& loop = static_cast<const syntax::ForStmt&>(value);
            writer_.string(loop.variable);
            site(loop.variable_span);
            expression(loop.begin.get(), depth + 1);
            expression(loop.end.get(), depth + 1);
            block(*loop.body, depth + 1);
            break;
        }
        }
    }
    void expression(const syntax::Expr* value, std::size_t depth) {
        if (!value || !enter(depth)) {
            if (!value)
                writer_.fail(Error::MissingType);
            return;
        }
        site(value->span);
        writer_.u8(static_cast<std::uint8_t>(value->kind));
        const auto type = types_.expressions.find(value);
        if (type == types_.expressions.end()) {
            writer_.fail(Error::MissingType);
            return;
        }
        semantic_type(type->second);
        const auto global = types_.global_references.find(value);
        writer_.boolean(global != types_.global_references.end());
        if (global != types_.global_references.end()) {
            const auto found =
                std::ranges::find_if(module_.globals, [&](const auto& item) {
                    return &item == global->second;
                });
            if (found == module_.globals.end()) {
                writer_.fail(Error::MissingDefinition);
                return;
            }
            writer_.string(found->name);
        }
        const auto address = types_.field_addresses.find(value);
        writer_.boolean(address != types_.field_addresses.end());
        if (address != types_.field_addresses.end())
            semantic_type(address->second);
        using enum syntax::Expr::Kind;
        switch (value->kind) {
        case Integer: {
            const auto& literal =
                static_cast<const syntax::IntegerExpr&>(*value);
            auto spelling = literal.spelling;
            std::erase(spelling, '_');
            writer_.string(spelling);
            writer_.u32(literal.width);
            writer_.boolean(literal.is_signed);
            writer_.boolean(literal.is_usize);
            break;
        }
        case Boolean:
            writer_.boolean(
                static_cast<const syntax::BooleanExpr&>(*value).value);
            break;
        case Name:
            writer_.string(static_cast<const syntax::NameExpr&>(*value).name);
            break;
        case Unary: {
            const auto& unary = static_cast<const syntax::UnaryExpr&>(*value);
            writer_.u8(static_cast<std::uint8_t>(unary.operation));
            expression(unary.operand.get(), depth + 1);
            break;
        }
        case Cast: {
            const auto& cast = static_cast<const syntax::CastExpr&>(*value);
            semantic_type(types_.resolve(cast.destination));
            expression(cast.operand.get(), depth + 1);
            break;
        }
        case Binary: {
            const auto& binary = static_cast<const syntax::BinaryExpr&>(*value);
            writer_.u8(static_cast<std::uint8_t>(binary.operation));
            expression(binary.left.get(), depth + 1);
            expression(binary.right.get(), depth + 1);
            break;
        }
        case Call: {
            const auto& call = static_cast<const syntax::CallExpr&>(*value);
            const auto found = std::ranges::find_if(
                module_.functions, [&](const auto& function) {
                    return function->name == call.callee;
                });
            if (found == module_.functions.end() &&
                call.callee != "external_input")
                writer_.fail(Error::MissingDefinition);
            writer_.string(call.callee);
            writer_.boolean(found ==
                            module_.functions.end()); // Builtin namespace.
            site(call.callee_span);
            writer_.count(call.arguments.size());
            for (const auto& argument : call.arguments)
                expression(argument.get(), depth + 1);
            break;
        }
        case If: {
            const auto& select = static_cast<const syntax::IfExpr&>(*value);
            expression(select.condition.get(), depth + 1);
            block(*select.then_block, depth + 1);
            block(*select.else_block, depth + 1);
            break;
        }
        case Array: {
            const auto& array = static_cast<const syntax::ArrayExpr&>(*value);
            writer_.count(array.elements.size());
            for (const auto& element : array.elements)
                expression(element.get(), depth + 1);
            break;
        }
        case ArrayGenerator: {
            const auto& generator =
                static_cast<const syntax::ArrayGeneratorExpr&>(*value);
            writer_.string(generator.variable);
            site(generator.variable_span);
            expression(generator.begin.get(), depth + 1);
            expression(generator.end.get(), depth + 1);
            block(*generator.body, depth + 1);
            break;
        }
        case Index: {
            const auto& index = static_cast<const syntax::IndexExpr&>(*value);
            expression(index.array.get(), depth + 1);
            expression(index.index.get(), depth + 1);
            break;
        }
        case Record: {
            const auto& record = static_cast<const syntax::RecordExpr&>(*value);
            writer_.string(record.name);
            site(record.name_span);
            writer_.count(record.fields.size());
            for (const auto& field : record.fields) {
                writer_.string(field.name);
                site(field.name_span);
                expression(field.value.get(), depth + 1);
            }
            break;
        }
        case Field: {
            const auto& field = static_cast<const syntax::FieldExpr&>(*value);
            expression(field.record.get(), depth + 1);
            writer_.string(field.name);
            site(field.name_span);
            break;
        }
        }
    }
    const syntax::Module& module_;
    const sema::TypeTable& types_;
    const cache::QueryContext& context_;
    cache::Limits limits_;
    Writer writer_;
    DefinitionIdentity current_;
    std::vector<source::Span> sites_;
    std::unordered_set<std::string> definitions_;
    CacheIdentity output_;
    Error error_{Error::InvalidFormat};
};
} // namespace
std::expected<cache::Bytes, cache::Error>
encode_source_anchor(const SourceAnchor& anchor, cache::Limits limits) {
    if (!cache::valid_definition_id(anchor.definition))
        return std::unexpected(cache::Error::InvalidAnchor);
    cache::wire::Writer writer(limits);
    cache::wire::header(writer, cache::ObjectKind::SourceAnchor);
    writer.string(anchor.definition.module.workspace);
    writer.string(anchor.definition.module.relative_path);
    writer.u8(static_cast<std::uint8_t>(anchor.definition.kind));
    writer.string(anchor.definition.name);
    writer.raw(anchor.definition_fingerprint.bytes);
    writer.u32(anchor.position);
    return writer.finish();
}
std::expected<SourceAnchor, cache::Error>
decode_source_anchor(std::span<const std::uint8_t> bytes,
                     cache::Limits limits) {
    cache::wire::Reader reader(bytes, limits);
    cache::wire::header(reader, cache::ObjectKind::SourceAnchor);
    SourceAnchor anchor;
    anchor.definition.module.workspace = reader.string();
    anchor.definition.module.relative_path = reader.string();
    anchor.definition.kind = static_cast<cache::DefinitionKind>(reader.u8());
    anchor.definition.name = reader.string();
    const auto hash = reader.raw(32);
    if (reader.good())
        std::ranges::copy(hash, anchor.definition_fingerprint.bytes.begin());
    anchor.position = reader.u32();
    if (!cache::valid_definition_id(anchor.definition))
        reader.fail(cache::Error::InvalidAnchor);
    if (const auto result = reader.finish(); !result)
        return std::unexpected(result.error());
    return anchor;
}
const DefinitionIdentity* CacheIdentity::find(cache::DefinitionKind kind,
                                              std::string_view name) const {
    const auto found =
        std::ranges::find_if(definitions, [&](const auto& definition) {
            return definition.id.kind == kind && definition.id.name == name;
        });
    return found == definitions.end() ? nullptr : &*found;
}
std::expected<source::Span, cache::Error>
CacheIdentity::resolve(const SourceAnchor& anchor) const {
    const auto found = std::ranges::find_if(
        sites, [&](const auto& site) { return site.anchor == anchor; });
    if (found == sites.end())
        return std::unexpected(cache::Error::InvalidAnchor);
    return found->span;
}
std::expected<CacheIdentity, cache::Error>
build_cache_identity(const syntax::Module& module, const sema::TypeTable& types,
                     const cache::QueryContext& context, cache::Limits limits) {
    if (!cache::valid_context(context) ||
        types.pointer_bits != context.target.pointer_bits ||
        context.query_version != cache::query_protocol_version ||
        context.budget_version != cache::budget_protocol_version)
        return std::unexpected(cache::Error::InvalidContext);
    return IdentityBuilder(module, types, context, limits).build();
}
std::expected<cache::QueryKey, cache::Error>
make_query_key(const DefinitionIdentity& function,
               const cache::QueryContext& context,
               std::vector<cache::StageArgument> arguments,
               cache::QueryKind kind, cache::Limits limits) {
    if (function.id.kind != DefinitionKind::Function ||
        function.id.module != context.module ||
        arguments.size() != function.parameters.size())
        return std::unexpected(cache::Error::InvalidContext);
    // Validate result admission separately: unsupported definitions still have
    // fingerprints for conservative module dependencies, but no reusable keys.
    Writer result_writer(limits);
    cache::wire::type(result_writer, function.result);
    if (const auto result = result_writer.finish(); !result)
        return std::unexpected(result.error());
    const auto result_scalar = function.result.is_array()
                                   ? function.result.element_type()
                                   : function.result;
    if (result_scalar.kind == sema::TypeKind::Integer &&
        result_scalar.integer_type.is_usize &&
        result_scalar.integer_type.width != context.target.pointer_bits)
        return std::unexpected(cache::Error::InvalidValue);
    for (std::size_t index = 0; index < arguments.size(); ++index)
        if (arguments[index].type != function.parameters[index])
            return std::unexpected(cache::Error::InvalidValue);
    cache::QueryKey key{kind, context, function.id,
                        function.semantic_fingerprint, std::move(arguments)};
    if (const auto result = cache::encode_query_key(key, limits); !result)
        return std::unexpected(result.error());
    return key;
}
} // namespace pagos::stage
