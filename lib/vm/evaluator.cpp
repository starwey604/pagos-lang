#include "pagos/vm/evaluator.h"

#include <cstdint>
#include <utility>

namespace pagos::vm {

std::expected<hir::Constant, EvaluationError>
Evaluator::unary(syntax::UnaryOperator operation,
                 const hir::Constant& operand) {
    switch (operation) {
    case syntax::UnaryOperator::Not:
        return hir::Constant{!std::get<bool>(operand)};
    case syntax::UnaryOperator::BitNot:
        return hir::Constant{
            static_cast<std::uint32_t>(~std::get<std::uint32_t>(operand))};
    }
    return std::unexpected(
        EvaluationError{.code = "E4002", .message = "unknown unary operation"});
}

std::expected<hir::Constant, EvaluationError>
Evaluator::binary(syntax::BinaryOperator operation, const hir::Constant& left,
                  const hir::Constant& right) {
    using enum syntax::BinaryOperator;
    if (operation == LogicalAnd || operation == LogicalOr) {
        const auto lhs = std::get<bool>(left);
        const auto rhs = std::get<bool>(right);
        return hir::Constant{operation == LogicalAnd ? lhs && rhs : lhs || rhs};
    }
    if (operation == Equal || operation == NotEqual) {
        const auto equal = left == right;
        return hir::Constant{operation == Equal ? equal : !equal};
    }

    const auto lhs = std::get<std::uint32_t>(left);
    const auto rhs = std::get<std::uint32_t>(right);
    switch (operation) {
    case Add:
        return hir::Constant{static_cast<std::uint32_t>(lhs + rhs)};
    case Subtract:
        return hir::Constant{static_cast<std::uint32_t>(lhs - rhs)};
    case Multiply:
        return hir::Constant{static_cast<std::uint32_t>(lhs * rhs)};
    case BitAnd:
        return hir::Constant{lhs & rhs};
    case BitOr:
        return hir::Constant{lhs | rhs};
    case BitXor:
        return hir::Constant{lhs ^ rhs};
    case ShiftLeft:
    case ShiftRight:
        if (rhs >= 32) {
            return std::unexpected(
                EvaluationError{.code = "E4009",
                                .message = "shift count must be less than 32"});
        }
        return hir::Constant{operation == ShiftLeft
                                 ? static_cast<std::uint32_t>(lhs << rhs)
                                 : static_cast<std::uint32_t>(lhs >> rhs)};
    case Divide:
        if (rhs == 0) {
            return std::unexpected(EvaluationError{
                .code = "E4001", .message = "division by zero"});
        }
        return hir::Constant{lhs / rhs};
    case Remainder:
        if (rhs == 0) {
            return std::unexpected(EvaluationError{
                .code = "E4001", .message = "remainder by zero"});
        }
        return hir::Constant{lhs % rhs};
    case Less:
        return hir::Constant{lhs < rhs};
    case LessEqual:
        return hir::Constant{lhs <= rhs};
    case Greater:
        return hir::Constant{lhs > rhs};
    case GreaterEqual:
        return hir::Constant{lhs >= rhs};
    case Equal:
    case NotEqual:
    case LogicalAnd:
    case LogicalOr:
        break;
    }
    return std::unexpected(EvaluationError{
        .code = "E4002", .message = "unknown binary operation"});
}

} // namespace pagos::vm
