#include "pagos/codegen/target.h"
#include "llvm_types.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/RISCVISAInfo.h>
#include <llvm/TargetParser/RISCVTargetParser.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

namespace pagos::codegen {

std::expected<TargetLayout, std::string>
TargetLayout::create(TargetConfig config) {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeRISCVTargetInfo();
        LLVMInitializeRISCVTarget();
        LLVMInitializeRISCVTargetMC();
        LLVMInitializeARMTargetInfo();
        LLVMInitializeARMTarget();
        LLVMInitializeARMTargetMC();
    });
    if (config.triple.empty())
        config.triple = llvm::sys::getDefaultTargetTriple();
    config.triple = llvm::Triple::normalize(config.triple);
    const llvm::Triple triple(config.triple);
    // Deliberately bounded preparation targets; do not silently accept an
    // untested backend merely because a monolithic LLVM happens to contain it.
    switch (triple.getArch()) {
    case llvm::Triple::x86:
    case llvm::Triple::x86_64:
    case llvm::Triple::riscv32:
    case llvm::Triple::riscv64:
    case llvm::Triple::arm:
    case llvm::Triple::thumb:
        break;
    default:
        return std::unexpected("unsupported target triple: " + config.triple);
    }
    std::string error;
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target)
        return std::unexpected(error);
    if (config.cpu.empty()) {
        config.cpu = triple.isRISCV32()   ? "generic-rv32"
                     : triple.isRISCV64() ? "generic-rv64"
                                          : "generic";
    }
    const std::unique_ptr<llvm::MCSubtargetInfo> metadata(
        target->createMCSubtargetInfo(triple, "", ""));
    if (!metadata || !metadata->isCPUStringValid(config.cpu) ||
        (triple.isRISCV() &&
         !llvm::RISCV::parseCPU(config.cpu, triple.isRISCV64()))) {
        return std::unexpected("unsupported CPU for " + config.triple + ": " +
                               config.cpu);
    }
    std::set<std::string> seen;
    std::istringstream features(config.features);
    std::string feature;
    while (std::getline(features, feature, ',')) {
        if (feature.size() < 2 ||
            (feature.front() != '+' && feature.front() != '-')) {
            return std::unexpected("invalid target feature: " + feature);
        }
        const auto name = feature.substr(1);
        if (!seen.insert(name).second)
            return std::unexpected("duplicate target feature: " + name);
        if (std::ranges::none_of(
                metadata->getAllProcessorFeatures(),
                [&](const auto& item) { return name == item.Key; }))
            return std::unexpected("unsupported target feature: " + name);
    }
    if (!config.features.empty() && config.features.back() == ',') {
        return std::unexpected("empty target feature");
    }
    if (triple.isRISCV()) {
        llvm::SmallVector<std::string> enabled;
        llvm::RISCV::getFeaturesForCPU(config.cpu, enabled, true);
        std::istringstream requested(config.features);
        while (std::getline(requested, feature, ','))
            enabled.push_back(feature);
        const std::vector<std::string> isa_features(enabled.begin(),
                                                    enabled.end());
        auto isa = llvm::RISCVISAInfo::parseFeatures(
            triple.isRISCV64() ? 64 : 32, isa_features);
        if (!isa)
            return std::unexpected("invalid RISC-V features: " +
                                   llvm::toString(isa.takeError()));
    }
    const std::unique_ptr<llvm::TargetMachine> machine(
        target->createTargetMachine(triple, config.cpu, config.features,
                                    llvm::TargetOptions{}, std::nullopt));
    if (!machine)
        return std::unexpected("cannot create target machine: " +
                               config.triple);
    return TargetLayout(std::move(config),
                        machine->createDataLayout().getStringRepresentation());
}

unsigned TargetLayout::pointer_bits() const {
    return llvm::DataLayout(layout_).getPointerSizeInBits();
}

bool TargetLayout::little_endian() const {
    return llvm::DataLayout(layout_).isLittleEndian();
}

std::expected<TypeLayout, std::string>
TargetLayout::layout_of(const mir::Type& type) const {
    if (!type.valid())
        return std::unexpected("cannot lay out invalid MIR type");
    llvm::LLVMContext context;
    const llvm::DataLayout layout(layout_);
    auto* storage = detail::storage_type(type, context);
    TypeLayout result{
        .size_bytes = layout.getTypeAllocSize(storage).getFixedValue(),
        .alignment_bytes = layout.getABITypeAlign(storage).value()};
    if (type.is_record()) {
        const auto* record =
            layout.getStructLayout(llvm::cast<llvm::StructType>(storage));
        for (unsigned index = 0; index < type.fields.size(); ++index) {
            result.field_offsets.push_back(record->getElementOffset(index));
        }
    }
    return result;
}

} // namespace pagos::codegen
