#pragma once

#include "pagos/cache/codec.h"

#include <limits>
#include <optional>
#include <utility>

namespace pagos::cache::wire {

// Sticky errors keep decoding bounded: no allocation is made from an unchecked
// file length, and every public decoder requires exact consumption.
class Writer {
  public:
    explicit Writer(Limits limits) : limits_(limits) {}
    void fail(Error error) {
        if (!error_)
            error_ = error;
    }
    void u8(std::uint8_t value) { raw(std::span(&value, 1)); }
    void u32(std::uint32_t value) { number(value, 4); }
    void u64(std::uint64_t value) { number(value, 8); }
    void boolean(bool value) { u8(value ? 1 : 0); }
    void raw(std::span<const std::uint8_t> value) {
        if (error_)
            return;
        if (value.size() > limits_.bytes - bytes_.size()) {
            fail(Error::LimitExceeded);
            return;
        }
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void count(std::size_t value) {
        if (value > limits_.elements || value > UINT32_MAX) {
            fail(Error::LimitExceeded);
            return;
        }
        u32(static_cast<std::uint32_t>(value));
    }
    void string(std::string_view value) {
        if (value.size() > limits_.string_bytes || value.size() > UINT32_MAX) {
            fail(Error::LimitExceeded);
            return;
        }
        u32(static_cast<std::uint32_t>(value.size()));
        raw({reinterpret_cast<const std::uint8_t*>(value.data()),
             value.size()});
    }
    [[nodiscard]] std::expected<Bytes, Error> finish() {
        if (error_)
            return std::unexpected(*error_);
        return std::move(bytes_);
    }
    [[nodiscard]] const Limits& limits() const { return limits_; }

  private:
    void number(std::uint64_t value, unsigned width) {
        for (unsigned index = 0; index < width; ++index)
            u8(static_cast<std::uint8_t>(value >> (index * 8)));
    }
    Limits limits_;
    Bytes bytes_;
    std::optional<Error> error_;
};

class Reader {
  public:
    Reader(std::span<const std::uint8_t> bytes, Limits limits)
        : bytes_(bytes), limits_(limits) {
        if (bytes.size() > limits.bytes)
            fail(Error::LimitExceeded);
    }
    void fail(Error error) {
        if (!error_)
            error_ = error;
    }
    std::uint8_t u8() {
        const auto data = raw(1);
        return data.empty() ? 0 : data[0];
    }
    std::uint32_t u32() { return static_cast<std::uint32_t>(number(4)); }
    std::uint64_t u64() { return number(8); }
    bool boolean() {
        const auto value = u8();
        if (value > 1)
            fail(Error::InvalidFormat);
        return value == 1;
    }
    std::span<const std::uint8_t> raw(std::size_t count) {
        if (error_)
            return {};
        if (count > remaining()) {
            fail(Error::InvalidFormat);
            return {};
        }
        const auto value = bytes_.subspan(offset_, count);
        offset_ += count;
        return value;
    }
    std::uint32_t count(std::size_t minimum_bytes = 1) {
        const auto count = u32();
        if (count > limits_.elements)
            fail(Error::LimitExceeded);
        if (minimum_bytes != 0 && count > remaining() / minimum_bytes)
            fail(Error::InvalidFormat);
        return good() ? count : 0;
    }
    std::string string() {
        const auto length = u32();
        if (length > limits_.string_bytes) {
            fail(Error::LimitExceeded);
            return {};
        }
        const auto value = raw(length);
        if (value.empty())
            return {};
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    [[nodiscard]] std::size_t remaining() const {
        return bytes_.size() - offset_;
    }
    [[nodiscard]] bool good() const { return !error_; }
    [[nodiscard]] const Limits& limits() const { return limits_; }
    [[nodiscard]] std::expected<void, Error> finish() {
        if (!error_ && remaining() != 0)
            fail(Error::InvalidFormat);
        if (error_)
            return std::unexpected(*error_);
        return {};
    }

  private:
    std::uint64_t number(unsigned width) {
        const auto value = raw(width);
        std::uint64_t result = 0;
        for (std::size_t index = 0; index < value.size(); ++index)
            result |= static_cast<std::uint64_t>(value[index]) << (index * 8);
        return result;
    }
    std::span<const std::uint8_t> bytes_;
    Limits limits_;
    std::size_t offset_{};
    std::optional<Error> error_;
};

// Only contiguous, explicitly bounded wire enums use this helper. Failed or
// out-of-range input sets a sticky error before any enum conversion; the
// fallback is a valid enumerator, never an unchecked value from disk.
template <class Enum> Enum enum_range(Reader& reader, Enum first, Enum last) {
    const auto value = reader.u8();
    if (!reader.good())
        return first;
    if (value < std::to_underlying(first) || value > std::to_underlying(last)) {
        reader.fail(Error::InvalidFormat);
        return first;
    }
    return static_cast<Enum>(value);
}
inline ObjectKind object_kind(Reader& reader) {
    return enum_range(reader, ObjectKind::Value,
                      ObjectKind::GeneratorReplayObservation);
}

void header(Writer& writer, ObjectKind kind);
void header(Reader& reader, ObjectKind kind);
void type(Writer& writer, const sema::Type& type);
sema::Type type(Reader& reader);
void value(Writer& writer, const TypedValue& value);
Constant value(Reader& reader, const sema::Type& type);

} // namespace pagos::cache::wire
