#pragma once

#include "pagos/source/span.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pagos::source {

struct Position {
    std::size_t line{};
    std::size_t column{};
};

class SourceFile {
  public:
    [[nodiscard]] static std::expected<SourceFile, std::string>
    load(const std::filesystem::path& path);

    [[nodiscard]] static SourceFile from_text(std::string path,
                                              std::string text);

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::string_view text() const noexcept { return text_; }
    [[nodiscard]] Position position(std::size_t offset) const noexcept;
    [[nodiscard]] std::string_view line(std::size_t one_based_line) const;

  private:
    SourceFile(std::string path, std::string text);
    void build_line_starts();

    std::string path_;
    std::string text_;
    std::vector<std::size_t> line_starts_;
};

} // namespace pagos::source
