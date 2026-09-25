#include "pagos/value.h"

#include <functional>
#include <ostream>
#include <type_traits>

namespace pagos {
namespace {

void print_scalar(const ScalarConstant& value, std::ostream& output) {
    std::visit(
        [&](auto scalar) {
            if constexpr (std::is_same_v<decltype(scalar), bool>) {
                output << (scalar ? "true" : "false");
            } else {
                output << scalar;
            }
        },
        value);
}

} // namespace

std::size_t constant_hash(const Constant& argument) noexcept {
    auto hash = argument.index();
    const auto mix = [&](std::size_t part) {
        hash ^= part + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
    };
    std::visit(
        [&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>,
                                         RecordConstant>) {
                mix(std::hash<std::string>{}(value.name));
                mix(value.fields.size());
                for (const auto& field : value.fields) {
                    mix(field.index());
                    std::visit(
                        [&](auto scalar) {
                            mix(static_cast<std::size_t>(scalar));
                        },
                        field);
                }
            } else if constexpr (requires { value.size(); }) {
                mix(value.size());
                for (auto element : value) {
                    mix(static_cast<std::size_t>(element));
                }
            } else {
                mix(static_cast<std::size_t>(value));
            }
        },
        argument);
    return hash;
}

bool constant_equal(const Constant& left, const Constant& right) {
    return left == right;
}

Constant from_scalar(const ScalarConstant& value) {
    return std::visit([](auto scalar) -> Constant { return scalar; }, value);
}

void print_constant(const Constant& value, std::ostream& output) {
    std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, RecordConstant>) {
                output << payload.name << '(';
                for (std::size_t index = 0; index < payload.fields.size();
                     ++index) {
                    if (index != 0)
                        output << ", ";
                    print_scalar(payload.fields[index], output);
                }
                output << ')';
            } else if constexpr (requires { payload.size(); }) {
                output << '[';
                for (std::size_t index = 0; index < payload.size(); ++index) {
                    if (index != 0)
                        output << ", ";
                    print_scalar(
                        static_cast<typename T::value_type>(payload[index]),
                        output);
                }
                output << ']';
            } else {
                print_scalar(payload, output);
            }
        },
        value);
}

} // namespace pagos
