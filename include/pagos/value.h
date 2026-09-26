#pragma once

#include "pagos/integer.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <variant>
#include <vector>

namespace pagos {

// Target-independent value payloads shared by HIR and MIR. Record fields
// are scalar and ordered by declaration, never by initializer spelling.
using ScalarConstant = std::variant<IntegerValue, bool>;
struct RecordConstant {
    std::string name;
    std::vector<ScalarConstant> fields;
    bool operator==(const RecordConstant&) const = default;
};
using Constant = std::variant<IntegerValue, bool, std::vector<IntegerValue>,
                              std::vector<bool>, RecordConstant>;

[[nodiscard]] std::size_t constant_hash(const Constant& value) noexcept;
[[nodiscard]] bool constant_equal(const Constant& left, const Constant& right);
[[nodiscard]] Constant from_scalar(const ScalarConstant& value);
void print_constant(const Constant& value, std::ostream& output);

} // namespace pagos
