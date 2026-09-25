#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace pagos {

// Target-independent value payloads shared by HIR and MIR. Record fields
// are scalar and ordered by declaration, never by initializer spelling.
using ScalarConstant = std::variant<std::uint32_t, bool>;
struct RecordConstant {
    std::string name;
    std::vector<ScalarConstant> fields;
    bool operator==(const RecordConstant&) const = default;
};
using Constant = std::variant<std::uint32_t, bool, std::vector<std::uint32_t>,
                              std::vector<bool>, RecordConstant>;

} // namespace pagos
