#pragma once

#include "pagos/source/span.h"

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

enum class Type { Bool, U32 };
enum class UnaryOperator { Not };
enum class BinaryOperator {
    Add,
    Subtract,
    Multiply,
    DivideChecked,
    RemainderChecked,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

using Constant = std::variant<std::uint32_t, bool>;

struct ConstantOperation {
    Constant value;
};

struct ExternalInputOperation {};

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

using Operation =
    std::variant<ConstantOperation, ExternalInputOperation, UnaryOperation,
                 BinaryOperation, BoolToU32Operation, PhiOperation>;

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

[[nodiscard]] std::string_view type_name(Type type) noexcept;
[[nodiscard]] std::string_view binary_name(BinaryOperator operation) noexcept;
[[nodiscard]] std::expected<void, std::string> verify(const Module& module);
void print(const Module& module, std::ostream& output);

} // namespace pagos::mir
