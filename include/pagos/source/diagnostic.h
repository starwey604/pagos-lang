#pragma once

#include "pagos/source/source_manager.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace pagos::source {

enum class Severity { Error, Warning, Note };

struct Label {
    Span span;
    std::string message;
    std::string heading;
};

struct Diagnostic {
    Severity severity{Severity::Error};
    std::string code;
    std::string message;
    Label primary;
    std::vector<Label> related;
    std::string help;
    std::vector<std::string> dependency_path;
};

class DiagnosticEngine {
  public:
    explicit DiagnosticEngine(const SourceFile& source) : source_(source) {}

    void report(Diagnostic diagnostic);
    void error(std::string code, std::string message, Span span,
               std::string label = {});

    [[nodiscard]] bool has_error() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return diagnostics_.size();
    }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

    void render(std::ostream& output) const;

  private:
    void render_label(std::ostream& output, const Label& label) const;

    const SourceFile& source_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace pagos::source
