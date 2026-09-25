#pragma once

#include "pagos/source/span.h"
#include "pagos/syntax/ast.h"

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pagos::hir {

enum class Stage { Static, Runtime };
using Constant = std::variant<std::uint32_t, bool>;

struct RuntimeTrace {
    source::Span origin_span;
    std::string origin_name;
    std::vector<std::string> path;
};

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

struct Expr {
    enum class Kind {
        Constant,
        Reference, // Trace-only use of the same computation identity.
        RuntimeBoundary,
        ExternalInput,
        Unary,
        Binary,
        If,
        Sequence, // Ordered evaluations followed by the result value.
        LoopIndex,
        RangeLoop,
    };

    Kind kind{Kind::Constant};
    syntax::TypeKind type{syntax::TypeKind::Error};
    Stage stage{Stage::Static};
    source::Span span;
    std::optional<RuntimeTrace> trace;
    std::optional<Constant> constant;
    std::optional<syntax::UnaryOperator> unary_operation;
    std::optional<syntax::BinaryOperator> binary_operation;
    std::optional<std::string> variable_name;
    std::vector<ExprPtr> operands;
};

struct Binding {
    std::string name;
    source::Span name_span;
    syntax::TypeKind type{syntax::TypeKind::Error};
    Stage stage{Stage::Static};
    ExprPtr value;
    std::optional<RuntimeTrace> trace;
};

struct FunctionSummary {
    std::string name;
    std::vector<syntax::TypeKind> parameters;
    syntax::TypeKind result{syntax::TypeKind::Error};
};

struct Module {
    std::vector<FunctionSummary> functions;
    std::vector<Binding> bindings;
    ExprPtr result;
};

[[nodiscard]] std::string_view stage_name(Stage stage) noexcept;
[[nodiscard]] ExprPtr make_constant(Constant value, syntax::TypeKind type,
                                    source::Span span);
[[nodiscard]] ExprPtr with_trace_step(const ExprPtr& expression,
                                      std::string step);

void print(const Module& module, std::ostream& output);
void explain_stages(const Module& module, std::ostream& output);

} // namespace pagos::hir
