#pragma once

#include "pagos/hir/hir.h"

#include <expected>
#include <string>

namespace pagos::vm {

struct EvaluationError {
    std::string code;
    std::string message;
};

class Evaluator {
  public:
    [[nodiscard]] static std::expected<hir::Constant, EvaluationError>
    unary(syntax::UnaryOperator operation, const hir::Constant& operand);

    [[nodiscard]] static std::expected<hir::Constant, EvaluationError>
    binary(syntax::BinaryOperator operation, const hir::Constant& left,
           const hir::Constant& right);
};

} // namespace pagos::vm
