#include "pagos/cache/store.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace pagos;
using namespace pagos::cache;
namespace fs = std::filesystem;
QueryKey key(std::string name) {
    const QueryContext context{.module = {"store-probe", "input.pgs"},
                               .target = {"riscv32-unknown-elf", "generic-rv32",
                                          "", "e-p:32:32", 32, true},
                               .compiler_build = "store-probe-v1",
                               .semantic_options = "pagos.staging.v1"};
    return {QueryKind::EvaluatePureSpecialization,
            context,
            {context.module, DefinitionKind::Function, std::move(name)},
            fingerprint("test-definition", {}),
            {{sema::Type::integer(32), ArgumentStage::Comptime,
              Constant(IntegerValue(7))}}};
}
std::string reason(StoreErrorCode code) {
    switch (code) {
    case StoreErrorCode::NotFound:
        return "not-found";
    case StoreErrorCode::MissingObject:
        return "missing-object";
    case StoreErrorCode::CorruptManifest:
        return "corrupt-manifest";
    case StoreErrorCode::CorruptObject:
        return "corrupt-object";
    case StoreErrorCode::LockBusy:
        return "lock-busy";
    case StoreErrorCode::DeterminismConflict:
        return "conflict";
    default:
        return "error";
    }
}
bool wait_for(const fs::path& path) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!fs::exists(path)) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}
} // namespace

// Test-only storage exerciser. No pagosc options or evaluation hooks.
int main(int argc, char** argv) {
    if (argc < 5 || argc > 7)
        return 2;
    const std::string mode = argv[1];
    const fs::path root = argv[2];
    const std::string input = argv[3];
    const auto number = IntegerValue::parse_decimal({32, false}, argv[4]);
    if (!number)
        return 2;
    const std::string action = argc > 5 ? argv[5] : "";
    const auto query = key(argc > 6 ? argv[6] : "square");
    if (action == "gate" && !wait_for(root / "start"))
        return 2;
    StoreOptions options;
    if (action == "gate")
        options.lock_timeout = std::chrono::seconds(5);
    options.fault = [&](IoOperation operation, const fs::path& destination) {
        if ((action == "crash-temporary" &&
             operation == IoOperation::WriteTemporary) ||
            (action == "crash-object" &&
             operation == IoOperation::ObjectPublished) ||
            (action == "crash-manifest-before" &&
             operation == IoOperation::PublishManifest) ||
            (action == "crash-manifest-after" &&
             operation == IoOperation::ManifestPublished))
            std::_Exit(86);
        if (action == "hold" && operation == IoOperation::TemporaryCreated) {
            std::ofstream(root / "ready").put('1');
            if (!wait_for(root / "release"))
                return std::make_error_code(std::errc::timed_out);
        }
        if (action == "delete-tmp" && operation == IoOperation::PublishObject)
            fs::remove_all(root / "tmp");
        if (action == "delete-index" &&
            operation == IoOperation::PublishManifest)
            fs::remove_all(destination.parent_path());
        return std::error_code{};
    };
    FileStore store(root, options);
    const auto inputs = fingerprint(input, {});
    if (mode == "read") {
        const auto candidate = store.lookup(query, inputs);
        if (!candidate) {
            std::cout << reason(candidate.error().code) << '\n';
            return 1;
        }
        for (const auto& artifact : candidate->artifacts)
            if (artifact.name == "result" && artifact.bytes) {
                const auto decoded = decode_value(*artifact.bytes);
                if (!decoded)
                    return 2;
                std::cout << std::get<IntegerValue>(decoded->value).bits()
                          << '\n';
                return 0;
            }
        return 2;
    }
    if (mode != "write")
        return 2;
    const auto payload = encode_value({sema::Type::integer(32), *number});
    const auto resources =
        encode_value({sema::Type::integer(32), IntegerValue(3)});
    if (!payload || !resources)
        return 2;
    const std::array artifacts{
        ArtifactInput{"result", ObjectKind::Value, *payload},
        ArtifactInput{"resources", ObjectKind::Value, *resources}};
    const auto result = store.publish(
        query, inputs, fingerprint("test-result", *payload), artifacts);
    if (!result) {
        std::cout << reason(result.error().code) << '\n';
        return 1;
    }
    std::cout << (*result == PublishResult::Published ? "published" : "present")
              << '\n';
}
