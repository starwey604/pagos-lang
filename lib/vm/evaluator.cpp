#include "pagos/vm/evaluator.h"
#include "pagos/integer.h"

#include <cstdint>
#include <optional>

namespace pagos::vm {
namespace {
std::optional<IntegerOperation>
integer_operation(syntax::BinaryOperator operation) {
    using enum syntax::BinaryOperator;
    switch (operation) {
    case Add:
        return IntegerOperation::Add;
    case Subtract:
        return IntegerOperation::Subtract;
    case Multiply:
        return IntegerOperation::Multiply;
    case Divide:
        return IntegerOperation::Divide;
    case Remainder:
        return IntegerOperation::Remainder;
    case BitAnd:
        return IntegerOperation::BitAnd;
    case BitOr:
        return IntegerOperation::BitOr;
    case BitXor:
        return IntegerOperation::BitXor;
    case ShiftLeft:
        return IntegerOperation::ShiftLeft;
    case ShiftRight:
        return IntegerOperation::ShiftRight;
    default:
        return std::nullopt;
    }
}
} // namespace

std::expected<hir::Constant, EvaluationError>
Evaluator::unary(syntax::UnaryOperator operation,
                 const hir::Constant& operand) {
    switch (operation) {
    case syntax::UnaryOperator::Not:
        return hir::Constant{!std::get<bool>(operand)};
    case syntax::UnaryOperator::BitNot:
        return hir::Constant{std::get<IntegerValue>(operand).bit_not()};
    case syntax::UnaryOperator::Negate:
        return hir::Constant{std::get<IntegerValue>(operand).negate()};
    case syntax::UnaryOperator::Dereference:
        return std::unexpected(EvaluationError{
            .code = "E4013",
            .message = "target memory cannot be read by the Static evaluator"});
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
        const auto equal = constant_equal(left, right);
        return hir::Constant{operation == Equal ? equal : !equal};
    }
    const auto lhs = std::get<IntegerValue>(left);
    const auto rhs = std::get<IntegerValue>(right);
    if (const auto integer_op = integer_operation(operation)) {
        const auto result = lhs.apply(*integer_op, rhs);
        if (!result) {
            if (result.error() == IntegerError::InvalidShift) {
                return std::unexpected(EvaluationError{
                    .code = "E4009",
                    .message =
                        std::string(lhs.type().is_signed
                                        ? "shift count must be non-negative "
                                          "and less than "
                                        : "shift count must be less than ") +
                        std::to_string(lhs.type().width)});
            }
            if (result.error() == IntegerError::DivideByZero) {
                return std::unexpected(EvaluationError{
                    .code = "E4001",
                    .message = operation == Divide ? "division by zero"
                                                   : "remainder by zero"});
            }
            if (result.error() == IntegerError::SignedOverflow) {
                return std::unexpected(EvaluationError{
                    .code = "E4011",
                    .message = operation == Divide
                                   ? "signed division overflow"
                                   : "signed remainder overflow"});
            }
            return std::unexpected(EvaluationError{
                .code = "E4002", .message = "invalid integer operation"});
        }
        return hir::Constant{*result};
    }
    const auto order = lhs.compare(rhs).value();
    switch (operation) {
    case Less:
        return hir::Constant{order < 0};
    case LessEqual:
        return hir::Constant{order <= 0};
    case Greater:
        return hir::Constant{order > 0};
    case GreaterEqual:
        return hir::Constant{order >= 0};
    default:
        break;
    }
    return std::unexpected(EvaluationError{
        .code = "E4002", .message = "unknown binary operation"});
}

} // namespace pagos::vm
