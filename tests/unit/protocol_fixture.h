#pragma once

#include "pagos/cache/codec.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace protocol_test {
// Fixed baseline bytes are read independently of the current encoder. Tests
// must never regenerate them: a changed encoding needs a compatibility review.
inline pagos::cache::Bytes fixture(std::string_view name) {
    const auto path =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "Inputs/protocol-baseline" / (std::string(name) + ".hex");
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("missing protocol fixture: " + path.string());
    pagos::cache::Bytes bytes;
    unsigned byte{};
    while (input >> std::hex >> byte) {
        if (byte > 255)
            throw std::runtime_error("invalid protocol fixture");
        bytes.push_back(static_cast<std::uint8_t>(byte));
    }
    if (!input.eof() || bytes.empty())
        throw std::runtime_error("invalid protocol fixture");
    return bytes;
}
inline void freeze(std::string_view name,
                   std::span<const std::uint8_t> actual) {
    const auto expected = fixture(name);
    EXPECT_EQ(pagos::cache::Bytes(actual.begin(), actual.end()), expected)
        << name;
}
// Canonical test-only bundle: sorted relative path + NUL + u64 byte length
// (little endian) + file bytes. Lock markers are lifecycle state, not records.
inline void freeze_store(std::string_view name,
                         const std::filesystem::path& directory) {
    std::map<std::string, std::filesystem::path> files;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(directory))
        if (entry.is_regular_file() && entry.path().extension() != ".lock")
            files.emplace(
                entry.path().lexically_relative(directory).generic_string(),
                entry.path());
    pagos::cache::Bytes bundle;
    for (const auto& [path, file] : files) {
        std::ifstream input(file, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        bundle.insert(bundle.end(), path.begin(), path.end());
        bundle.push_back(0);
        for (unsigned i = 0; i < 8; ++i)
            bundle.push_back(static_cast<std::uint8_t>(
                static_cast<std::uint64_t>(bytes.size()) >> (8 * i)));
        bundle.insert(bundle.end(), bytes.begin(), bytes.end());
    }
    freeze(name, bundle);
}
} // namespace protocol_test
