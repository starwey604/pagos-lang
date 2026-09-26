#pragma once

#include "pagos/mir/mir.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>

namespace pagos::codegen::detail {

inline llvm::Type* storage_type(const mir::Type& type,
                                llvm::LLVMContext& context) {
    if (type.is_pointer())
        return llvm::PointerType::get(context, 0);
    if (type.is_record()) {
        std::vector<llvm::Type*> fields;
        fields.reserve(type.fields.size());
        for (const auto& field : type.fields)
            fields.push_back(storage_type(field, context));
        return llvm::StructType::get(context, fields);
    }
    if (type.is_array()) {
        return llvm::ArrayType::get(storage_type(type.element_type(), context),
                                    type.length);
    }
    return llvm::IntegerType::get(
        context, type.kind == mir::Type::Bool ? 1 : type.integer_type.width);
}

} // namespace pagos::codegen::detail
