#include "pagos/codegen/llvm_codegen.h"
#include "llvm_types.h"

#include <llvm/ADT/APInt.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pagos::codegen {
namespace {

class Generator {
  public:
    explicit Generator(const TargetLayout& target)
        : module_(std::make_unique<llvm::Module>("pagos", context_)),
          builder_(context_), config_(target.config()) {
        module_->setTargetTriple(llvm::Triple(config_.triple));
        module_->setDataLayout(target.data_layout());
    }

    std::expected<std::string, std::string>
    emit(const mir::Module& mir_module) {
        if (auto valid = mir::verify(mir_module); !valid) {
            return std::unexpected("invalid MIR: " + valid.error());
        }
        for (const auto& function : mir_module.functions) {
            if (auto emitted = emit_function(function); !emitted) {
                return std::unexpected(emitted.error());
            }
        }

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
    std::expected<void, std::string>
    emit_function(const mir::Function& mir_function) {
        values_.clear();
        signed_values_.clear();
        blocks_.clear();
        exits_.clear();

        auto* function_type =
            llvm::FunctionType::get(llvm_type(mir_function.result_type), false);
        auto* function = llvm::Function::Create(function_type,
                                                llvm::Function::ExternalLinkage,
                                                mir_function.name, *module_);
        function->addFnAttr("target-cpu", config_.cpu);
        if (!config_.features.empty()) {
            function->addFnAttr("target-features", config_.features);
        }
        for (const auto& block : mir_function.blocks) {
            blocks_.emplace(block.id, llvm::BasicBlock::Create(
                                          context_, block.name, function));
        }

        for (const auto& block : mir_function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.type.kind == mir::Type::Integer &&
                    instruction.type.integer_type.is_signed) {
                    signed_values_.insert(instruction.result);
                }
                if (const auto* phi = std::get_if<mir::PhiOperation>(
                        &instruction.operation)) {
                    auto* llvm_phi = llvm::PHINode::Create(
                        llvm_type(instruction.type),
                        static_cast<unsigned>(phi->incoming.size()),
                        "if.result", blocks_.at(block.id));
                    values_.emplace(instruction.result, llvm_phi);
                }
            }
        }

        for (const auto* block : emission_order(mir_function)) {
            builder_.SetInsertPoint(blocks_.at(block->id));
            for (const auto& instruction : block->instructions) {
                if (std::holds_alternative<mir::PhiOperation>(
                        instruction.operation)) {
                    continue;
                }
                auto* value = emit_instruction(instruction);
                if (!value) {
                    return std::unexpected("could not emit MIR value %" +
                                           std::to_string(instruction.result));
                }
                values_.emplace(instruction.result, value);
            }
            auto* exit = builder_.GetInsertBlock();
            exits_.emplace(block->id, exit);
            emit_terminator(block->terminator);
        }

        for (const auto& block : mir_function.blocks) {
            for (const auto& instruction : block.instructions) {
                const auto* phi =
                    std::get_if<mir::PhiOperation>(&instruction.operation);
                if (!phi) {
                    continue;
                }
                auto* llvm_phi =
                    llvm::cast<llvm::PHINode>(values_.at(instruction.result));
                for (const auto& incoming : phi->incoming) {
                    llvm_phi->addIncoming(values_.at(incoming.value),
                                          exits_.at(incoming.block));
                }
            }
        }
        return {};
    }

    static std::vector<const mir::BasicBlock*>
    emission_order(const mir::Function& function) {
        std::unordered_map<mir::BlockId, const mir::BasicBlock*> blocks;
        for (const auto& block : function.blocks) {
            blocks.emplace(block.id, &block);
        }

        std::unordered_set<mir::BlockId> visited;
        std::vector<const mir::BasicBlock*> postorder;
        const std::function<void(mir::BlockId)> visit = [&](mir::BlockId id) {
            if (!visited.insert(id).second) {
                return;
            }
            const auto* block = blocks.at(id);
            if (const auto* branch =
                    std::get_if<mir::Branch>(&block->terminator)) {
                visit(branch->target);
            } else if (const auto* conditional_branch =
                           std::get_if<mir::ConditionalBranch>(
                               &block->terminator)) {
                visit(conditional_branch->then_target);
                visit(conditional_branch->else_target);
            }
            postorder.push_back(block);
        };
        visit(function.entry);
        return std::vector<const mir::BasicBlock*>(postorder.rbegin(),
                                                   postorder.rend());
    }

    llvm::Type* llvm_type(const mir::Type& type) {
        return detail::storage_type(type, context_);
    }

    llvm::Value* emit_instruction(const mir::Instruction& instruction) {
        if (const auto* constant =
                std::get_if<mir::ConstantOperation>(&instruction.operation)) {
            if (const auto* integer =
                    std::get_if<IntegerValue>(&constant->value)) {
                return llvm::ConstantInt::get(llvm_type(instruction.type),
                                              integer->bits());
            }
            if (std::holds_alternative<bool>(constant->value)) {
                return builder_.getInt1(std::get<bool>(constant->value));
            }
            if (const auto* record =
                    std::get_if<RecordConstant>(&constant->value)) {
                std::vector<llvm::Constant*> fields;
                for (const auto& field : record->fields) {
                    std::visit(
                        [&](auto value) {
                            if constexpr (std::is_same_v<decltype(value),
                                                         bool>) {
                                fields.push_back(builder_.getInt1(value));
                            } else {
                                fields.push_back(llvm::ConstantInt::get(
                                    llvm::IntegerType::get(context_,
                                                           value.type().width),
                                    value.bits()));
                            }
                        },
                        field);
                }
                return llvm::ConstantStruct::get(
                    llvm::cast<llvm::StructType>(llvm_type(instruction.type)),
                    fields);
            }
            std::vector<llvm::Constant*> elements;
            elements.reserve(instruction.type.length);
            std::visit(
                [&](const auto& values) {
                    if constexpr (requires { values.size(); }) {
                        for (auto value : values) {
                            if constexpr (std::is_same_v<decltype(value),
                                                         IntegerValue>) {
                                elements.push_back(llvm::ConstantInt::get(
                                    llvm_type(instruction.type.element_type()),
                                    value.bits()));
                            } else {
                                elements.push_back(llvm::ConstantInt::get(
                                    llvm_type(instruction.type.element_type()),
                                    value));
                            }
                        }
                    }
                },
                constant->value);
            return llvm::ConstantArray::get(
                llvm::cast<llvm::ArrayType>(llvm_type(instruction.type)),
                elements);
        }
        if (std::holds_alternative<mir::ExternalInputOperation>(
                instruction.operation)) {
            auto* input_type =
                llvm::FunctionType::get(builder_.getInt32Ty(), false);
            const auto input = module_->getOrInsertFunction(
                "pagos_external_input", input_type);
            return builder_.CreateCall(input, {}, "runtime.input");
        }
        if (const auto* unary =
                std::get_if<mir::UnaryOperation>(&instruction.operation)) {
            if (unary->operation == mir::UnaryOperator::Negate) {
                return builder_.CreateNeg(values_.at(unary->operand), "negate");
            }
            return builder_.CreateNot(values_.at(unary->operand), "not");
        }
        if (const auto* binary =
                std::get_if<mir::BinaryOperation>(&instruction.operation)) {
            return emit_binary(*binary);
        }
        if (const auto* cast = std::get_if<mir::IntegerCastOperation>(
                &instruction.operation)) {
            return builder_.CreateIntCast(
                values_.at(cast->operand), llvm_type(instruction.type),
                signed_values_.contains(cast->operand), "integer.cast");
        }
        if (const auto* cast =
                std::get_if<mir::BoolToU32Operation>(&instruction.operation)) {
            return builder_.CreateZExt(values_.at(cast->operand),
                                       builder_.getInt32Ty(), "bool.result");
        }
        if (const auto* array =
                std::get_if<mir::ArrayOperation>(&instruction.operation)) {
            llvm::Value* value =
                llvm::PoisonValue::get(llvm_type(instruction.type));
            for (std::size_t index = 0; index < array->elements.size();
                 ++index) {
                value = builder_.CreateInsertValue(
                    value, values_.at(array->elements[index]),
                    {static_cast<unsigned>(index)}, "array");
            }
            return value;
        }
        if (const auto* record =
                std::get_if<mir::RecordOperation>(&instruction.operation)) {
            llvm::Value* value =
                llvm::PoisonValue::get(llvm_type(instruction.type));
            for (std::size_t index = 0; index < record->fields.size();
                 ++index) {
                value = builder_.CreateInsertValue(
                    value, values_.at(record->fields[index]),
                    {static_cast<unsigned>(index)}, "record");
            }
            return value;
        }
        if (const auto* field =
                std::get_if<mir::FieldOperation>(&instruction.operation)) {
            return builder_.CreateExtractValue(values_.at(field->record),
                                               {field->index}, "field");
        }
        if (const auto* index =
                std::get_if<mir::IndexOperation>(&instruction.operation)) {
            return emit_index(*index);
        }
        return nullptr;
    }

    llvm::Value* emit_index(const mir::IndexOperation& operation) {
        auto* array = values_.at(operation.array);
        auto* index = values_.at(operation.index);
        auto* type = llvm::cast<llvm::ArrayType>(array->getType());
        const auto length = type->getNumElements();
        if (const auto* offset = llvm::dyn_cast<llvm::ConstantInt>(index);
            offset && offset->getZExtValue() < length) {
            return builder_.CreateExtractValue(
                array, {static_cast<unsigned>(offset->getZExtValue())},
                "array.element");
        }

        auto* function = builder_.GetInsertBlock()->getParent();
        auto* trap_block =
            llvm::BasicBlock::Create(context_, "index.oob", function);
        auto* continue_block =
            llvm::BasicBlock::Create(context_, "index.cont", function);
        auto* in_bounds = builder_.CreateICmpULT(
            index, builder_.getInt32(static_cast<std::uint32_t>(length)),
            "index.valid");
        builder_.CreateCondBr(in_bounds, continue_block, trap_block);
        builder_.SetInsertPoint(trap_block);
        auto* trap = llvm::Intrinsic::getOrInsertDeclaration(
            module_.get(), llvm::Intrinsic::trap);
        builder_.CreateCall(trap);
        builder_.CreateUnreachable();
        builder_.SetInsertPoint(continue_block);

        llvm::Value* storage{};
        if (auto* constant = llvm::dyn_cast<llvm::Constant>(array)) {
            auto [entry, inserted] = tables_.try_emplace(constant, nullptr);
            if (inserted) {
                entry->second = new llvm::GlobalVariable(
                    *module_, type, true, llvm::GlobalValue::PrivateLinkage,
                    constant, "pagos.table");
                entry->second->setUnnamedAddr(
                    llvm::GlobalValue::UnnamedAddr::Global);
            }
            storage = entry->second;
        } else {
            // Allocate once per access site, never once per loop iteration.
            llvm::IRBuilder<> allocations(context_);
            auto& entry = function->getEntryBlock();
            allocations.SetInsertPoint(&entry, entry.begin());
            storage = allocations.CreateAlloca(type, nullptr, "array.storage");
            builder_.CreateStore(array, storage);
        }
        auto* offset =
            builder_.CreateZExt(index, builder_.getInt64Ty(), "index.offset");
        auto* address = builder_.CreateInBoundsGEP(
            type, storage, {builder_.getInt64(0), offset}, "array.address");
        return builder_.CreateLoad(type->getElementType(), address,
                                   "array.element");
    }

    llvm::Value* emit_binary(const mir::BinaryOperation& operation) {
        auto* left = values_.at(operation.left);
        auto* right = values_.at(operation.right);
        const bool is_signed = signed_values_.contains(operation.left);
        using enum mir::BinaryOperator;
        switch (operation.operation) {
        case Add:
            return builder_.CreateAdd(left, right, "add");
        case Subtract:
            return builder_.CreateSub(left, right, "sub");
        case Multiply:
            return builder_.CreateMul(left, right, "mul");
        case BitAnd:
            return builder_.CreateAnd(left, right, "bit.and");
        case BitOr:
            return builder_.CreateOr(left, right, "bit.or");
        case BitXor:
            return builder_.CreateXor(left, right, "bit.xor");
        case ShiftLeftChecked:
            return emit_shift(left, right, false, is_signed);
        case ShiftRightChecked:
            return emit_shift(left, right, true, is_signed);
        case DivideChecked:
            return emit_division(left, right, false, is_signed);
        case RemainderChecked:
            return emit_division(left, right, true, is_signed);
        case Equal:
            return builder_.CreateICmpEQ(left, right, "eq");
        case NotEqual:
            return builder_.CreateICmpNE(left, right, "ne");
        case Less:
            return is_signed ? builder_.CreateICmpSLT(left, right, "lt")
                             : builder_.CreateICmpULT(left, right, "lt");
        case LessEqual:
            return is_signed ? builder_.CreateICmpSLE(left, right, "le")
                             : builder_.CreateICmpULE(left, right, "le");
        case Greater:
            return is_signed ? builder_.CreateICmpSGT(left, right, "gt")
                             : builder_.CreateICmpUGT(left, right, "gt");
        case GreaterEqual:
            return is_signed ? builder_.CreateICmpSGE(left, right, "ge")
                             : builder_.CreateICmpUGE(left, right, "ge");
        }
        return nullptr;
    }

    llvm::Value* emit_shift(llvm::Value* left, llvm::Value* right,
                            bool shift_right, bool is_signed) {
        // A known valid count needs no guard. Never emit an out-of-range
        // LLVM shift on an executed path: it would produce poison.
        const auto* count = llvm::dyn_cast<llvm::ConstantInt>(right);
        const auto width = left->getType()->getIntegerBitWidth();
        if (!count || count->getZExtValue() >= width) {
            auto* function = builder_.GetInsertBlock()->getParent();
            auto* trap_block =
                llvm::BasicBlock::Create(context_, "shift.oob", function);
            auto* continue_block =
                llvm::BasicBlock::Create(context_, "shift.cont", function);
            auto* valid = builder_.CreateICmpULT(
                right, llvm::ConstantInt::get(right->getType(), width),
                "shift.valid");
            builder_.CreateCondBr(valid, continue_block, trap_block);
            builder_.SetInsertPoint(trap_block);
            auto* trap = llvm::Intrinsic::getOrInsertDeclaration(
                module_.get(), llvm::Intrinsic::trap);
            builder_.CreateCall(trap);
            builder_.CreateUnreachable();
            builder_.SetInsertPoint(continue_block);
        }
        return shift_right
                   ? (is_signed ? builder_.CreateAShr(left, right, "ashr")
                                : builder_.CreateLShr(left, right, "lshr"))
                   : builder_.CreateShl(left, right, "shl");
    }

    llvm::Value* emit_division(llvm::Value* left, llvm::Value* right,
                               bool remainder, bool is_signed) {
        auto* function = builder_.GetInsertBlock()->getParent();
        auto* trap_block = llvm::BasicBlock::Create(
            context_, is_signed ? "div.invalid" : "div.zero", function);
        auto* continue_block =
            llvm::BasicBlock::Create(context_, "div.cont", function);
        auto* zero = llvm::ConstantInt::get(right->getType(), 0);
        auto* is_zero = builder_.CreateICmpEQ(right, zero, "divisor.zero");
        llvm::Value* invalid = is_zero;
        if (is_signed) {
            const auto width = left->getType()->getIntegerBitWidth();
            auto* minimum = llvm::ConstantInt::get(
                context_, llvm::APInt::getSignedMinValue(width));
            auto* minus_one = llvm::ConstantInt::get(
                context_, llvm::APInt::getAllOnes(width));
            auto* overflow = builder_.CreateAnd(
                builder_.CreateICmpEQ(left, minimum, "dividend.minimum"),
                builder_.CreateICmpEQ(right, minus_one, "divisor.minus.one"),
                "division.overflow");
            invalid = builder_.CreateOr(invalid, overflow, "division.invalid");
        }
        builder_.CreateCondBr(invalid, trap_block, continue_block);

        builder_.SetInsertPoint(trap_block);
        auto* trap = llvm::Intrinsic::getOrInsertDeclaration(
            module_.get(), llvm::Intrinsic::trap);
        builder_.CreateCall(trap);
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(continue_block);
        if (is_signed) {
            return remainder ? builder_.CreateSRem(left, right, "rem")
                             : builder_.CreateSDiv(left, right, "div");
        }
        return remainder ? builder_.CreateURem(left, right, "rem")
                         : builder_.CreateUDiv(left, right, "div");
    }

    void emit_terminator(const mir::Terminator& terminator) {
        if (const auto* branch = std::get_if<mir::Branch>(&terminator)) {
            builder_.CreateBr(blocks_.at(branch->target));
        } else if (const auto* conditional_branch =
                       std::get_if<mir::ConditionalBranch>(&terminator)) {
            builder_.CreateCondBr(values_.at(conditional_branch->condition),
                                  blocks_.at(conditional_branch->then_target),
                                  blocks_.at(conditional_branch->else_target));
        } else if (const auto* return_operation =
                       std::get_if<mir::Return>(&terminator)) {
            builder_.CreateRet(values_.at(return_operation->value));
        }
    }

    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    TargetConfig config_;
    std::unordered_map<mir::ValueId, llvm::Value*> values_;
    // LLVM integer types erase signedness; retain each MIR definition's fact.
    std::unordered_set<mir::ValueId> signed_values_;
    std::unordered_map<mir::BlockId, llvm::BasicBlock*> blocks_;
    std::unordered_map<mir::BlockId, llvm::BasicBlock*> exits_;
    std::unordered_map<llvm::Constant*, llvm::GlobalVariable*> tables_;
};

} // namespace

std::expected<std::string, std::string>
LLVMCodegen::emit(const mir::Module& mir_module, TargetConfig config) {
    const auto target = TargetLayout::create(std::move(config));
    if (!target) {
        return std::unexpected(target.error());
    }
    return Generator(*target).emit(mir_module);
}

} // namespace pagos::codegen
