#include "pagos/source/source_manager.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace pagos::source {

SourceFile::SourceFile(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
    build_line_starts();
}

std::expected<SourceFile, std::string>
SourceFile::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected("could not open source file `" + path.string() +
                               "`");
    }

    std::string text{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
    if (!input.good() && !input.eof()) {
        return std::unexpected("could not read source file `" + path.string() +
                               "`");
    }
    return SourceFile(path.string(), std::move(text));
}

SourceFile SourceFile::from_text(std::string path, std::string text) {
    return SourceFile(std::move(path), std::move(text));
}

void SourceFile::build_line_starts() {
    line_starts_.clear();
    line_starts_.push_back(0);
    for (std::size_t index = 0; index < text_.size(); ++index) {
        if (text_[index] == '\n') {
            line_starts_.push_back(index + 1);
        }
    }
}

Position SourceFile::position(std::size_t offset) const noexcept {
    offset = std::min(offset, text_.size());
    const auto iterator =
        std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    const auto line_index = static_cast<std::size_t>(
        std::distance(line_starts_.begin(), iterator) - 1);
    return {.line = line_index + 1,
            .column = offset - line_starts_[line_index] + 1};
}

std::string_view SourceFile::line(std::size_t one_based_line) const {
    if (one_based_line == 0 || one_based_line > line_starts_.size()) {
        return {};
    }
    const auto begin = line_starts_[one_based_line - 1];
    auto end = one_based_line < line_starts_.size()
                   ? line_starts_[one_based_line] - 1
                   : text_.size();
    if (end > begin && text_[end - 1] == '\r') {
        --end;
    }
    return std::string_view(text_).substr(begin, end - begin);
}

} // namespace pagos::source
