#pragma once

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
    enum Kind { Bool, U32, ArrayBool, ArrayU32, Record };
    Kind kind{U32};
    std::uint32_t length{};
    std::string record_name;
    std::vector<Kind> fields;
    Type() = default;
    Type(Kind kind, std::uint32_t length = 0) : kind(kind), length(length) {}
    bool operator==(const Type&) const = default;
    [[nodiscard]] bool is_record() const noexcept { return kind == Record; }
    [[nodiscard]] bool is_array() const noexcept {
        return kind == ArrayBool || kind == ArrayU32;
    }
    [[nodiscard]] Type element_type() const noexcept {
        return kind == ArrayBool ? Bool : U32;
    }
    [[nodiscard]] bool valid() const noexcept {
        if (is_record()) {
            if (record_name.empty() || fields.empty() || length != 0) {
                return false;
            }
            for (const auto field : fields) {
                if (field != Bool && field != U32) {
                    return false;
                }
            }
            return true;
        }
        if (!record_name.empty() || !fields.empty()) {
            return false;
        }
        return is_array() ? length > 0
                          : (kind == Bool || kind == U32) && length == 0;
    }
};
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
