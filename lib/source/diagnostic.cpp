#include "pagos/source/diagnostic.h"

#include <algorithm>
#include <ostream>
#include <string_view>

namespace pagos::source {
namespace {

std::string_view severity_name(Severity severity) {
    switch (severity) {
    case Severity::Error:
        return "error";
    case Severity::Warning:
        return "warning";
    case Severity::Note:
        return "note";
    }
    return "error";
}

} // namespace

void DiagnosticEngine::report(Diagnostic diagnostic) {
    diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticEngine::error(std::string code, std::string message, Span span,
                             std::string label) {
    report({.severity = Severity::Error,
            .code = std::move(code),
            .message = std::move(message),
            .primary = {.span = span, .message = std::move(label)}});
}

bool DiagnosticEngine::has_error() const noexcept {
    return std::ranges::any_of(diagnostics_, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Severity::Error;
    });
}

void DiagnosticEngine::render_label(std::ostream& output,
                                    const Label& label) const {
    const auto position = source_.position(label.span.begin);
    if (!label.heading.empty()) {
        output << '\n' << label.heading << ":\n";
    }
    output << "  --> " << source_.path() << ':' << position.line << ':'
           << position.column << '\n';

    const auto source_line = source_.line(position.line);
    const auto width = std::to_string(position.line).size();
    output << std::string(width + 1, ' ') << "|\n";
    output << position.line << " | " << source_line << '\n';
    output << std::string(width + 1, ' ') << "| "
           << std::string(position.column - 1, ' ');

    const auto available = source_line.size() >= position.column
                               ? source_line.size() - position.column + 1
                               : std::size_t{1};
    const auto requested =
        std::max<std::size_t>(1, label.span.end - label.span.begin);
    output << std::string(std::min(requested, available), '^');
    if (!label.message.empty()) {
        output << ' ' << label.message;
    }
    output << '\n';
}

void DiagnosticEngine::render(std::ostream& output) const {
    for (const auto& diagnostic : diagnostics_) {
        output << severity_name(diagnostic.severity);
        if (!diagnostic.code.empty()) {
            output << '[' << diagnostic.code << ']';
        }
        output << ": " << diagnostic.message << '\n';
        render_label(output, diagnostic.primary);
        for (const auto& related : diagnostic.related) {
            render_label(output, related);
        }
        if (!diagnostic.dependency_path.empty()) {
            output << "\ndependency path: ";
            for (std::size_t index = 0;
                 index < diagnostic.dependency_path.size(); ++index) {
                if (index != 0) {
                    output << " -> ";
                }
                output << diagnostic.dependency_path[index];
            }
            output << '\n';
        }
        if (!diagnostic.help.empty()) {
            output << "help: " << diagnostic.help << '\n';
        }
    }
}

} // namespace pagos::source
