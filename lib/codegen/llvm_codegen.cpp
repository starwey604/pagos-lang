#include "pagos/codegen/llvm_codegen.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace pagos::codegen {
namespace {

class Generator {
  public:
    Generator()
        : module_(std::make_unique<llvm::Module>("pagos", context_)),
          builder_(context_) {
        module_->setTargetTriple(
            llvm::Triple(llvm::sys::getDefaultTargetTriple()));
    }

    std::expected<std::string, std::string>
    emit(const hir::Module& hir_module) {
        auto* function_type =
            llvm::FunctionType::get(builder_.getInt32Ty(), false);
        auto* function = llvm::Function::Create(function_type,
                                                llvm::Function::ExternalLinkage,
                                                "pagos_main", *module_);
        auto* entry = llvm::BasicBlock::Create(context_, "entry", function);
        builder_.SetInsertPoint(entry);

        llvm::Value* result = builder_.getInt32(0);
        if (hir_module.result) {
            result = emit_expression(hir_module.result);
            if (!result) {
                return std::unexpected("could not lower HIR expression");
            }
            if (hir_module.result->type == syntax::TypeKind::Bool) {
                result = builder_.CreateZExt(result, builder_.getInt32Ty(),
                                             "bool.result");
            }
        }
        builder_.CreateRet(result);

        std::string verification_error;
        llvm::raw_string_ostream verification_stream(verification_error);
        if (llvm::verifyModule(*module_, &verification_stream)) {
            verification_stream.flush();
            return std::unexpected("invalid generated LLVM IR: " +
                                   verification_error);
        }

        std::string output;
        llvm::raw_string_ostream stream(output);
        module_->print(stream, nullptr);
        stream.flush();
        return output;
    }

  private:
    llvm::Type* llvm_type(syntax::TypeKind type) {
        return type == syntax::TypeKind::Bool ? builder_.getInt1Ty()
                                              : builder_.getInt32Ty();
    }

    llvm::Value* emit_expression(const hir::ExprPtr& expression) {
        switch (expression->kind) {
        case hir::Expr::Kind::Constant: {
            const auto& constant = expression->constant.value();
            if (std::holds_alternative<std::uint32_t>(constant)) {
                return builder_.getInt32(std::get<std::uint32_t>(constant));
            }
            return builder_.getInt1(std::get<bool>(constant));
        }
        case hir::Expr::Kind::RuntimeBoundary:
            return emit_expression(expression->operands.at(0));
        case hir::Expr::Kind::ExternalInput: {
            auto* input_type =
                llvm::FunctionType::get(builder_.getInt32Ty(), false);
            const auto input = module_->getOrInsertFunction(
                "pagos_external_input", input_type);
            return builder_.CreateCall(input, {}, "runtime.input");
        }
        case hir::Expr::Kind::Unary: {
            auto* operand = emit_expression(expression->operands.at(0));
            return builder_.CreateNot(operand, "not");
        }
        case hir::Expr::Kind::Binary:
            return emit_binary(expression);
        case hir::Expr::Kind::If:
            return emit_if(expression);
        }
        return nullptr;
    }

    llvm::Value* emit_binary(const hir::ExprPtr& expression) {
        auto* left = emit_expression(expression->operands.at(0));
        auto* right = emit_expression(expression->operands.at(1));
        using enum syntax::BinaryOperator;
        switch (expression->binary_operation.value()) {
        case Add:
            return builder_.CreateAdd(left, right, "add");
        case Subtract:
            return builder_.CreateSub(left, right, "sub");
        case Multiply:
            return builder_.CreateMul(left, right, "mul");
        case Divide:
            return emit_division(left, right, false);
        case Remainder:
            return emit_division(left, right, true);
        case Equal:
            return builder_.CreateICmpEQ(left, right, "eq");
        case NotEqual:
            return builder_.CreateICmpNE(left, right, "ne");
        case Less:
            return builder_.CreateICmpULT(left, right, "lt");
        case LessEqual:
            return builder_.CreateICmpULE(left, right, "le");
        case Greater:
            return builder_.CreateICmpUGT(left, right, "gt");
        case GreaterEqual:
            return builder_.CreateICmpUGE(left, right, "ge");
        case LogicalAnd:
        case LogicalOr:
            return nullptr;
        }
        return nullptr;
    }

    llvm::Value* emit_division(llvm::Value* left, llvm::Value* right,
                               bool remainder) {
        auto* function = builder_.GetInsertBlock()->getParent();
        auto* trap_block =
            llvm::BasicBlock::Create(context_, "div.zero", function);
        auto* continue_block =
            llvm::BasicBlock::Create(context_, "div.cont", function);
        auto* zero = llvm::ConstantInt::get(right->getType(), 0);
        auto* is_zero = builder_.CreateICmpEQ(right, zero, "divisor.zero");
        builder_.CreateCondBr(is_zero, trap_block, continue_block);

        builder_.SetInsertPoint(trap_block);
        auto* trap = llvm::Intrinsic::getOrInsertDeclaration(
            module_.get(), llvm::Intrinsic::trap);
        builder_.CreateCall(trap);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(continue_block);
        return remainder ? builder_.CreateURem(left, right, "rem")
                         : builder_.CreateUDiv(left, right, "div");
    }

    llvm::Value* emit_if(const hir::ExprPtr& expression) {
        auto* condition = emit_expression(expression->operands.at(0));
        auto* function = builder_.GetInsertBlock()->getParent();
        auto* then_block =
            llvm::BasicBlock::Create(context_, "if.then", function);
        auto* else_block =
            llvm::BasicBlock::Create(context_, "if.else", function);
        auto* merge_block =
            llvm::BasicBlock::Create(context_, "if.merge", function);
        builder_.CreateCondBr(condition, then_block, else_block);

        builder_.SetInsertPoint(then_block);
        auto* then_value = emit_expression(expression->operands.at(1));
        then_block = builder_.GetInsertBlock();
        builder_.CreateBr(merge_block);

        builder_.SetInsertPoint(else_block);
        auto* else_value = emit_expression(expression->operands.at(2));
        else_block = builder_.GetInsertBlock();
        builder_.CreateBr(merge_block);

        builder_.SetInsertPoint(merge_block);
        auto* phi =
            builder_.CreatePHI(llvm_type(expression->type), 2, "if.result");
        phi->addIncoming(then_value, then_block);
        phi->addIncoming(else_value, else_block);
        return phi;
    }

    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
};

} // namespace

std::expected<std::string, std::string>
LLVMCodegen::emit(const hir::Module& hir_module) {
    return Generator().emit(hir_module);
}

} // namespace pagos::codegen
