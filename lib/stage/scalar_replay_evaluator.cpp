#include "scalar_replay_evaluator.h"

#include <stdexcept>

namespace pagos::stage::replay_detail {
namespace {
using Op = ScalarReplayOp;
IntegerOperation operation(Op op) {
    switch (op) {
    case Op::Add:
        return IntegerOperation::Add;
    case Op::Subtract:
        return IntegerOperation::Subtract;
    case Op::Multiply:
        return IntegerOperation::Multiply;
    case Op::Divide:
        return IntegerOperation::Divide;
    case Op::Remainder:
        return IntegerOperation::Remainder;
    case Op::BitAnd:
        return IntegerOperation::BitAnd;
    case Op::BitOr:
        return IntegerOperation::BitOr;
    case Op::BitXor:
        return IntegerOperation::BitXor;
    case Op::ShiftLeft:
        return IntegerOperation::ShiftLeft;
    case Op::ShiftRight:
        return IntegerOperation::ShiftRight;
    default:
        throw std::logic_error("unvalidated scalar replay operation");
    }
}
ScalarConstant scalar(const Constant& value) {
    if (const auto* integer = std::get_if<IntegerValue>(&value))
        return *integer;
    return std::get<bool>(value);
}
} // namespace
std::expected<ScalarConstant, IntegerError>
evaluate(const ScalarReplayNode& node,
         std::span<const cache::StageArgument> inputs,
         const ScalarConstant* values, std::optional<IntegerValue> iterator) {
    if (node.operation == Op::Input) {
        if (*node.input == inputs.size() && iterator)
            return *iterator;
        return scalar(*inputs[*node.input].value);
    }
    if (node.operation == Op::Constant)
        return scalar(*node.constant);
    const auto& left = values[node.operands[0]];
    if (node.operation == Op::Not)
        return !std::get<bool>(left);
    if (node.operation == Op::Negate)
        return std::get<IntegerValue>(left).negate();
    if (node.operation == Op::BitNot)
        return std::get<IntegerValue>(left).bit_not();
    const auto& right = values[node.operands[1]];
    if (node.operation == Op::Equal || node.operation == Op::NotEqual)
        return (left == right) == (node.operation == Op::Equal);
    const auto lhs = std::get<IntegerValue>(left);
    const auto rhs = std::get<IntegerValue>(right);
    if (node.operation >= Op::Less && node.operation <= Op::GreaterEqual) {
        const auto order = lhs.compare(rhs);
        if (!order)
            return std::unexpected(order.error());
        switch (node.operation) {
        case Op::Less:
            return *order < 0;
        case Op::LessEqual:
            return *order <= 0;
        case Op::Greater:
            return *order > 0;
        case Op::GreaterEqual:
            return *order >= 0;
        default:
            break;
        }
    }
    const auto value = lhs.apply(operation(node.operation), rhs);
    if (!value)
        return std::unexpected(value.error());
    return *value;
}
} // namespace pagos::stage::replay_detail
