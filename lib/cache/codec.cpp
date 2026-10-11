#include "pagos/cache/codec.h"
#include "wire.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>

namespace pagos::cache {
namespace {
bool integer_type(IntegerType type) {
    return type.valid() && (type.width == 8 || type.width == 16 ||
                            type.width == 32 || type.width == 64);
}
bool supported(const sema::Type& type) {
    using enum sema::TypeKind;
    if (type.kind == Bool)
        return type == sema::Type(Bool);
    if (type.kind == Integer) {
        auto canonical = sema::Type::integer(type.integer_type.width,
                                             type.integer_type.is_signed);
        canonical.integer_type.is_usize = type.integer_type.is_usize;
        return integer_type(type.integer_type) && type == canonical;
    }
    return type.kind == Array && type.length > 0 &&
           supported(type.element_type()) &&
           type == sema::Type::array(type.element_type(), type.length);
}
void integer(wire::Writer& writer, const IntegerValue& value,
             IntegerType type) {
    if (value.type() != type) {
        writer.fail(Error::InvalidValue);
        return;
    }
    writer.u64(value.bits());
}
IntegerValue integer(wire::Reader& reader, IntegerType type) {
    const auto bits = reader.u64();
    if (type.width < 64 && (bits >> type.width) != 0)
        reader.fail(Error::InvalidFormat);
    const auto value = IntegerValue::create(type, bits);
    if (!value) {
        reader.fail(Error::InvalidFormat);
        return {};
    }
    return *value;
}
} // namespace

std::string Digest::hex() const {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (const auto byte : bytes) {
        output += digits[byte >> 4];
        output += digits[byte & 15];
    }
    return output;
}
Digest fingerprint(std::string_view domain,
                   std::span<const std::uint8_t> bytes) {
    llvm::SHA256 hash;
    std::array<std::uint8_t, 8> size{};
    for (std::size_t index = 0; index < size.size(); ++index)
        size[index] = static_cast<std::uint8_t>(
            static_cast<std::uint64_t>(domain.size()) >> (index * 8));
    hash.update(llvm::ArrayRef(size));
    hash.update(llvm::ArrayRef(
        reinterpret_cast<const std::uint8_t*>(domain.data()), domain.size()));
    hash.update(llvm::ArrayRef(bytes.data(), bytes.size()));
    return {hash.final()};
}
namespace wire {
void header(Writer& writer, ObjectKind kind) {
    constexpr std::array<std::uint8_t, 4> magic{'P', 'G', 'C', '1'};
    writer.raw(magic);
    writer.u32(format_version);
    writer.u8(static_cast<std::uint8_t>(kind));
}
void header(Reader& reader, ObjectKind kind) {
    const auto magic = reader.raw(4);
    constexpr std::array<std::uint8_t, 4> expected{'P', 'G', 'C', '1'};
    if (!std::ranges::equal(magic, expected))
        reader.fail(Error::InvalidFormat);
    if (reader.u32() != format_version)
        reader.fail(Error::IncompatibleVersion);
    if (reader.u8() != static_cast<std::uint8_t>(kind))
        reader.fail(Error::InvalidFormat);
}
void type(Writer& writer, const sema::Type& type) {
    if (!supported(type)) {
        writer.fail(Error::UnsupportedType);
        return;
    }
    const auto scalar = type.is_array() ? type.element_type() : type;
    writer.u8(type.is_array()                       ? 3
              : scalar.kind == sema::TypeKind::Bool ? 1
                                                    : 2);
    if (type.is_array()) {
        writer.count(type.length);
        writer.u8(scalar.kind == sema::TypeKind::Bool ? 1 : 2);
    }
    if (scalar.kind == sema::TypeKind::Integer) {
        writer.u8(static_cast<std::uint8_t>(scalar.integer_type.width));
        writer.boolean(scalar.integer_type.is_signed);
        writer.boolean(scalar.integer_type.is_usize);
    }
}
sema::Type type(Reader& reader) {
    auto tag = reader.u8();
    const bool array = tag == 3;
    std::uint32_t length = 0;
    if (array) {
        // Type lengths do not imply that a value payload follows (Runtime
        // args).
        length = reader.count(0);
        if (length == 0)
            reader.fail(Error::InvalidFormat);
        tag = reader.u8();
    }
    sema::Type result{sema::TypeKind::Bool};
    if (tag == 2) {
        const auto width = reader.u8();
        const auto is_signed = reader.boolean();
        const auto is_usize = reader.boolean();
        result = sema::Type::integer(width, is_signed);
        result.integer_type.is_usize = is_usize;
        if (!integer_type(result.integer_type))
            reader.fail(Error::InvalidFormat);
    } else if (tag != 1)
        reader.fail(Error::InvalidFormat);
    return array ? sema::Type::array(result, length) : result;
}
void value(Writer& writer, const TypedValue& value) {
    if (!supported(value.type)) {
        writer.fail(Error::UnsupportedType);
        return;
    }
    if (value.type.kind == sema::TypeKind::Bool) {
        const auto* payload = std::get_if<bool>(&value.value);
        if (!payload) {
            writer.fail(Error::InvalidValue);
            return;
        }
        writer.boolean(*payload);
    } else if (value.type.kind == sema::TypeKind::Integer) {
        const auto* payload = std::get_if<IntegerValue>(&value.value);
        if (!payload) {
            writer.fail(Error::InvalidValue);
            return;
        }
        integer(writer, *payload, value.type.integer_type);
    } else if (value.type.element_kind == sema::TypeKind::Bool) {
        const auto* payload = std::get_if<std::vector<bool>>(&value.value);
        if (!payload || payload->size() != value.type.length) {
            writer.fail(Error::InvalidValue);
            return;
        }
        for (bool element : *payload)
            writer.boolean(element);
    } else {
        const auto* payload =
            std::get_if<std::vector<IntegerValue>>(&value.value);
        if (!payload || payload->size() != value.type.length) {
            writer.fail(Error::InvalidValue);
            return;
        }
        for (const auto& element : *payload)
            integer(writer, element, value.type.integer_type);
    }
}
Constant value(Reader& reader, const sema::Type& type) {
    if (!reader.good())
        return false;
    if (type.kind == sema::TypeKind::Bool)
        return reader.boolean();
    if (type.kind == sema::TypeKind::Integer)
        return integer(reader, type.integer_type);
    const bool boolean = type.element_kind == sema::TypeKind::Bool;
    const std::size_t width = boolean ? 1 : 8;
    if (type.length > reader.remaining() / width) {
        reader.fail(Error::InvalidFormat);
        return false;
    }
    if (boolean) {
        std::vector<bool> result;
        result.reserve(type.length);
        for (std::uint32_t index = 0; index < type.length && reader.good();
             ++index)
            result.push_back(reader.boolean());
        return result;
    }
    std::vector<IntegerValue> result;
    result.reserve(type.length);
    for (std::uint32_t index = 0; index < type.length && reader.good(); ++index)
        result.push_back(integer(reader, type.integer_type));
    return result;
}
} // namespace wire
std::expected<Bytes, Error> encode_value(const TypedValue& value,
                                         Limits limits) {
    wire::Writer writer(limits);
    wire::header(writer, ObjectKind::Value);
    wire::type(writer, value.type);
    wire::value(writer, value);
    return writer.finish();
}
std::expected<TypedValue, Error>
decode_value(std::span<const std::uint8_t> bytes, Limits limits) {
    wire::Reader reader(bytes, limits);
    wire::header(reader, ObjectKind::Value);
    auto type = wire::type(reader);
    auto value = wire::value(reader, type);
    if (const auto result = reader.finish(); !result)
        return std::unexpected(result.error());
    return TypedValue{std::move(type), std::move(value)};
}
} // namespace pagos::cache
