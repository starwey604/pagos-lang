#pragma once

#include "pagos/integer.h"

#include "pagos/source/span.h"
#include "pagos/value.h"

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pagos::mir {

using ValueId = std::uint32_t;
using BlockId = std::uint32_t;

struct Type {
    enum Kind { Invalid, Bool, Integer, Array, Record };
    // Convenience descriptor, not a separate integer kind.
    static const Type U32;
    Kind kind{Integer};
    IntegerType integer_type;
    Kind element_kind{Invalid};
    std::uint32_t length{};
    std::string record_name;
    std::vector<Type> fields;
    Type() = default;
    Type(Kind kind) : kind(kind) {}
    bool operator==(const Type&) const = default;
    [[nodiscard]] bool is_record() const noexcept { return kind == Record; }
    [[nodiscard]] bool is_array() const noexcept { return kind == Array; }
    [[nodiscard]] static Type integer(unsigned width, bool is_signed = false) {
        Type type{Integer};
        type.integer_type = {width, is_signed};
        return type;
    }
    [[nodiscard]] static Type array(const Type& element, std::uint32_t length) {
        if (element.kind != Bool && element.kind != Integer)
            return Invalid;
        Type type{Array};
        type.element_kind = element.kind;
        type.integer_type = element.integer_type;
        type.length = length;
        return type;
    }
    [[nodiscard]] Type element_type() const noexcept {
        if (!is_array())
            return Invalid;
        Type type{element_kind};
        type.integer_type = integer_type;
        return type;
    }
    [[nodiscard]] bool valid() const noexcept {
        if (is_record()) {
            if (record_name.empty() || fields.empty() || length != 0 ||
                element_kind != Invalid)
                return false;
            for (const auto& field : fields) {
                if ((field.kind != Bool && field.kind != Integer) ||
                    !field.valid())
                    return false;
            }
            return true;
        }
        if (!record_name.empty() || !fields.empty())
            return false;
        if (is_array())
            return length > 0 &&
                   (element_kind == Bool || element_kind == Integer) &&
                   element_type().valid();
        return element_kind == Invalid && length == 0 &&
               (kind == Bool || (kind == Integer && integer_type.valid()));
    }
};
inline const Type Type::U32 = Type::integer(32);
enum class UnaryOperator { Not, BitNot };
enum class BinaryOperator {
    Add,
    Subtract,
    Multiply,
    DivideChecked,
    RemainderChecked,
    BitAnd,
    BitOr,
    BitXor,
    // Counts >= 32 trap before the shift; right shift fills with zeros.
    ShiftLeftChecked,
    ShiftRightChecked,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

using Constant = pagos::Constant;

struct ConstantOperation {
    Constant value;
};

struct ExternalInputOperation {};

struct ArrayOperation {
    std::vector<ValueId> elements;
};

struct IndexOperation {
    // Unsigned bounds check before element access; an invalid index traps.
    ValueId array;
    ValueId index;
};

struct RecordOperation {
    std::vector<ValueId> fields;
};

struct FieldOperation {
    ValueId record;
    std::uint32_t index;
};

struct UnaryOperation {
    UnaryOperator operation;
    ValueId operand;
};

struct BinaryOperation {
    BinaryOperator operation;
    ValueId left;
    ValueId right;
};

struct BoolToU32Operation {
    ValueId operand;
};

struct PhiIncoming {
    BlockId block;
    ValueId value;
};

struct PhiOperation {
    std::vector<PhiIncoming> incoming;
};

using Operation = std::variant<ConstantOperation, ExternalInputOperation,
                               UnaryOperation, BinaryOperation,
                               BoolToU32Operation, PhiOperation, ArrayOperation,
                               IndexOperation, RecordOperation, FieldOperation>;

struct Instruction {
    ValueId result;
    Type type;
    Operation operation;
    source::Span span;
};

struct Branch {
    BlockId target;
};

struct ConditionalBranch {
    ValueId condition;
    BlockId then_target;
    BlockId else_target;
};

struct Return {
    ValueId value;
};

using Terminator =
    std::variant<std::monostate, Branch, ConditionalBranch, Return>;

struct BasicBlock {
    BlockId id;
    std::string name;
    std::vector<Instruction> instructions;
    Terminator terminator;
};

struct Function {
    std::string name;
    Type result_type;
    BlockId entry;
    std::vector<BasicBlock> blocks;
};

struct Module {
    std::vector<Function> functions;
};

[[nodiscard]] std::string type_name(const Type& type);
[[nodiscard]] std::string_view binary_name(BinaryOperator operation) noexcept;
[[nodiscard]] std::expected<void, std::string> verify(const Module& module);
void print(const Module& module, std::ostream& output);

} // namespace pagos::mir
