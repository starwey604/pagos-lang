#include "pagos/integer.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>

namespace pagos {

std::expected<IntegerValue, IntegerError>
IntegerValue::create(IntegerType type, std::uint64_t bits) {
    if (!type.valid()) {
        return std::unexpected(IntegerError::InvalidWidth);
    }
    return IntegerValue(type,
                        llvm::APInt(64, bits).trunc(type.width).getZExtValue());
}

std::string IntegerValue::decimal() const {
    llvm::SmallString<24> text;
    llvm::APInt(type_.width, bits_).toString(text, 10, type_.is_signed);
    return std::string(text);
}

IntegerValue IntegerValue::bit_not() const {
    return IntegerValue(type_,
                        (~llvm::APInt(type_.width, bits_)).getZExtValue());
}

std::expected<IntegerValue, IntegerError>
IntegerValue::apply(IntegerOperation operation,
                    const IntegerValue& right) const {
    if (type_ != right.type_) {
        return std::unexpected(IntegerError::TypeMismatch);
    }
    const llvm::APInt lhs(type_.width, bits_);
    const llvm::APInt rhs(type_.width, right.bits_);
    using enum IntegerOperation;
    if ((operation == Divide || operation == Remainder) && rhs.isZero()) {
        return std::unexpected(IntegerError::DivideByZero);
    }
    if ((operation == Divide || operation == Remainder) && type_.is_signed &&
        lhs.isMinSignedValue() && rhs.isAllOnes()) {
        return std::unexpected(IntegerError::SignedOverflow);
    }
    if ((operation == ShiftLeft || operation == ShiftRight) &&
        right.bits_ >= type_.width) {
        return std::unexpected(IntegerError::InvalidShift);
    }
    auto result = lhs;
    switch (operation) {
    case Add:
        result = lhs + rhs;
        break;
    case Subtract:
        result = lhs - rhs;
        break;
    case Multiply:
        result = lhs * rhs;
        break;
    case Divide:
        result = type_.is_signed ? lhs.sdiv(rhs) : lhs.udiv(rhs);
        break;
    case Remainder:
        result = type_.is_signed ? lhs.srem(rhs) : lhs.urem(rhs);
        break;
    case BitAnd:
        result = lhs & rhs;
        break;
    case BitOr:
        result = lhs | rhs;
        break;
    case BitXor:
        result = lhs ^ rhs;
        break;
    case ShiftLeft:
        result = lhs.shl(static_cast<unsigned>(right.bits_));
        break;
    case ShiftRight:
        result = type_.is_signed ? lhs.ashr(static_cast<unsigned>(right.bits_))
                                 : lhs.lshr(static_cast<unsigned>(right.bits_));
        break;
    }
    return IntegerValue(type_, result.getZExtValue());
}

std::expected<int, IntegerError>
IntegerValue::compare(const IntegerValue& right) const {
    if (type_ != right.type_) {
        return std::unexpected(IntegerError::TypeMismatch);
    }
    const llvm::APInt lhs(type_.width, bits_);
    const llvm::APInt rhs(type_.width, right.bits_);
    if (lhs == rhs) {
        return 0;
    }
    return (type_.is_signed ? lhs.slt(rhs) : lhs.ult(rhs)) ? -1 : 1;
}

} // namespace pagos
