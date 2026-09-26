#include "pagos/sema/type.h"

namespace pagos::sema {

Type::Type(const syntax::Type& annotation) {
    switch (annotation.kind) {
    case syntax::TypeKind::Unknown:
        kind = TypeKind::Unknown;
        break;
    case syntax::TypeKind::Void:
        kind = TypeKind::Void;
        break;
    case syntax::TypeKind::Bool:
        kind = TypeKind::Bool;
        break;
    case syntax::TypeKind::Integer:
        *this = integer(annotation.integer_width, annotation.integer_signed);
        break;
    case syntax::TypeKind::Never:
        kind = TypeKind::Never;
        break;
    case syntax::TypeKind::Error:
        kind = TypeKind::Error;
        break;
    case syntax::TypeKind::ArrayBool:
    case syntax::TypeKind::ArrayInteger:
        *this = array(Type(annotation.element_type()), annotation.length);
        break;
    case syntax::TypeKind::Record:
        *this = record(annotation.record_name);
        break;
    }
}

std::string type_name(const Type& type) {
    switch (type.kind) {
    case TypeKind::Unknown:
        return "<unknown>";
    case TypeKind::Void:
        return "void";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::Integer:
        return std::string(type.integer_type.is_signed ? "i" : "u") +
               std::to_string(type.integer_type.width);
    case TypeKind::Never:
        return "never";
    case TypeKind::Error:
        return "<error>";
    case TypeKind::Array:
        return "[" + type_name(type.element_type()) + "; " +
               (type.length == 0 ? "?" : std::to_string(type.length)) + "]";
    case TypeKind::Record:
        return type.record_name;
    }
    return "<error>";
}

} // namespace pagos::sema
