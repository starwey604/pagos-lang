#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

namespace pagos {

// Source-level storage requests, shared by AST, HIR and verified MIR.
// Zero alignment and an empty section mean no explicit request.
struct GlobalStorage {
    std::string section;
    std::uint32_t alignment{};

    // Keep every byte offset representable as a signed target pointer offset.
    [[nodiscard]] static bool fits_target(unsigned element_bits,
                                          std::uint32_t count,
                                          unsigned pointer_bits) {
        return (pointer_bits == 32 || pointer_bits == 64) &&
               static_cast<std::uint64_t>(count) * (element_bits / 8) <=
                   ((std::uint64_t{1} << (pointer_bits - 1)) - 1);
    }

    [[nodiscard]] static bool valid_alignment(std::uint32_t value) {
        return std::has_single_bit(value) && value <= 4096;
    }
    [[nodiscard]] bool requires_zero() const {
        return section.starts_with(".bss.");
    }
    [[nodiscard]] bool valid(bool writable) const {
        if (alignment != 0 && !valid_alignment(alignment))
            return false;
        if (section.empty())
            return true;
        // Conventional ELF linkers treat these as read-only after relocation,
        // not ordinary writable data/BSS subsections.
        if (section.starts_with(".data.rel.ro") ||
            section.starts_with(".bss.rel.ro"))
            return false;
        const std::string_view prefix = !writable         ? ".rodata."
                                        : requires_zero() ? ".bss."
                                                          : ".data.";
        if (!section.starts_with(prefix) || section.size() == prefix.size())
            return false;
        return std::ranges::all_of(section, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '.';
        });
    }
};

} // namespace pagos
