#pragma once

#include "pagos/integer.h"
#include "pagos/syntax/ast.h"

#include <cstdint>
#include <string>

namespace pagos::sema {

enum class TypeKind {
    Unknown,
    Void,
    Bool,
    Integer,
    Never,
    Error,
    Array,
    Record
};

// Semantic facts are distinct from source annotations and concrete MIR types.
// Arrays currently have scalar elements; adding a width adds no array kind.
struct Type {
    TypeKind kind{TypeKind::Error};
    IntegerType integer_type;
    TypeKind element_kind{TypeKind::Error};
    std::uint32_t length{}; // Zero is deferred in sema/HIR, never concrete MIR.
    std::string record_name;

    Type() = default;
    Type(TypeKind kind) : kind(kind) {}
    explicit Type(const syntax::Type& annotation, unsigned pointer_bits = 0);
    Type(syntax::TypeKind kind) : Type(syntax::Type(kind)) {}
    bool operator==(const Type&) const = default;
    [[nodiscard]] bool is_record() const noexcept {
        return kind == TypeKind::Record;
    }
    [[nodiscard]] bool is_array() const noexcept {
        return kind == TypeKind::Array;
    }
    [[nodiscard]] static Type integer(unsigned width, bool is_signed = false) {
        Type type{TypeKind::Integer};
        type.integer_type = {width, is_signed};
        return type;
    }
    [[nodiscard]] static Type record(std::string name) {
        Type type{TypeKind::Record};
        type.record_name = std::move(name);
        return type;
    }
    [[nodiscard]] static Type usize(unsigned pointer_bits) {
        if (pointer_bits != 32 && pointer_bits != 64)
            return TypeKind::Error;
        auto type = integer(pointer_bits);
        type.integer_type.is_usize = true;
        return type;
    }
    [[nodiscard]] static Type array(const Type& element, std::uint32_t length) {
        if (element.kind != TypeKind::Bool &&
            element.kind != TypeKind::Integer) {
            return TypeKind::Error;
        }
        Type type{TypeKind::Array};
        type.element_kind = element.kind;
        type.integer_type = element.integer_type;
        type.length = length;
        return type;
    }
    [[nodiscard]] Type element_type() const noexcept {
        if (!is_array()) {
            return TypeKind::Error;
        }
        Type type{element_kind};
        type.integer_type = integer_type;
        return type;
    }
};

[[nodiscard]] std::string type_name(const Type& type);

} // namespace pagos::sema
