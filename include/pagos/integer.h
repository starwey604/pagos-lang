#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace pagos {

// A target-independent integer description. Source-language admission is a
// separate decision; the preparation layer deliberately bounds storage at 64.
struct IntegerType {
    unsigned width{32};
    bool is_signed{};
    bool is_usize{};
    bool operator==(const IntegerType&) const = default;
    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && width <= 64 &&
               (!is_usize || (!is_signed && (width == 32 || width == 64)));
    }
};

enum class IntegerError {
    InvalidWidth,
    TypeMismatch,
    DivideByZero,
    InvalidShift,
    SignedOverflow,
    InvalidLiteral
};
enum class IntegerOperation {
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    BitAnd,
    BitOr,
    BitXor,
    ShiftLeft,
    ShiftRight
};

// Inline bits: no heap allocation per scalar and no LLVM types in public APIs.
class IntegerValue {
  public:
    // Compatibility ingress for compiler-created u32 loop/index constants.
    IntegerValue(std::uint32_t bits = 0) noexcept : type_{}, bits_(bits) {}
    [[nodiscard]] static std::expected<IntegerValue, IntegerError>
    create(IntegerType type, std::uint64_t bits);
    [[nodiscard]] static std::expected<IntegerValue, IntegerError>
    parse_decimal(IntegerType type, std::string_view spelling);
    [[nodiscard]] std::expected<IntegerValue, IntegerError>
    convert(IntegerType destination) const;
    [[nodiscard]] static IntegerValue from_u32(std::uint32_t bits) noexcept {
        return IntegerValue({}, bits);
    }
    [[nodiscard]] IntegerType type() const noexcept { return type_; }
    [[nodiscard]] std::uint64_t bits() const noexcept { return bits_; }
    [[nodiscard]] std::string decimal() const;
    [[nodiscard]] IntegerValue bit_not() const;
    [[nodiscard]] IntegerValue negate() const;
    [[nodiscard]] std::expected<IntegerValue, IntegerError>
    apply(IntegerOperation operation, const IntegerValue& right) const;
    [[nodiscard]] std::expected<int, IntegerError>
    compare(const IntegerValue& right) const;
    bool operator==(const IntegerValue&) const = default;

  private:
    IntegerValue(IntegerType type, std::uint64_t bits)
        : type_(type), bits_(bits) {}
    IntegerType type_;
    std::uint64_t bits_;
};

} // namespace pagos
