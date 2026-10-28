#pragma once

#include "query_tasks.h"

#include <random>
#include <stdexcept>

namespace pagos::driver::query_task_cli {
using namespace stage::provenance;
inline cache::Digest fresh_id() {
    std::random_device random;
    cache::Digest entropy;
    for (auto& byte : entropy.bytes)
        byte = static_cast<std::uint8_t>(random());
    return cache::fingerprint("pagos.cli-query-task.v1", entropy.bytes);
}
inline std::string error_text(const cache::StoreError& error) {
    switch (error.code) {
    case cache::StoreErrorCode::NotFound:
        return "query task not found";
    case cache::StoreErrorCode::MissingObject:
        return "query state object missing";
    case cache::StoreErrorCode::CorruptManifest:
    case cache::StoreErrorCode::CorruptObject:
        return "query state object corrupt";
    case cache::StoreErrorCode::LimitExceeded:
        return "query storage limit exceeded";
    case cache::StoreErrorCode::ReadOnly:
        return "query storage is read-only";
    case cache::StoreErrorCode::StaleGeneration:
        return "query head changed; resume the saved task again";
    case cache::StoreErrorCode::LockBusy:
        return "query task writer is busy";
    case cache::StoreErrorCode::IncompatibleVersion:
        return "query storage version is incompatible";
    default:
        return "query storage failure" +
               (error.io ? ": " + error.io.message() : "");
    }
}
inline std::string error_text(const CheckpointError& error) {
    if (error.storage)
        return error_text(*error.storage);
    switch (error.code) {
    case CheckpointErrorCode::BindingMismatch:
        return "query binding mismatch (source, evidence, root, channel or "
               "mode)";
    case CheckpointErrorCode::IncompatibleVersion:
        return "incompatible query checkpoint or storage mode";
    case CheckpointErrorCode::Corrupt:
        return "corrupt query checkpoint";
    case CheckpointErrorCode::LimitExceeded:
        return "query state or buffer limit exceeded";
    case CheckpointErrorCode::UnavailableEvidence:
        return "query evidence unavailable";
    case CheckpointErrorCode::OutOfMemory:
        return "query allocation failed";
    default:
        return "invalid query checkpoint state";
    }
}
template <class T, class E> T checked(std::expected<T, E> value) {
    if (!value)
        throw std::runtime_error(error_text(value.error()));
    return std::move(*value);
}
inline std::string_view status_name(cache::TaskStatus status) {
    switch (status) {
    case cache::TaskStatus::Paused:
        return "paused";
    case cache::TaskStatus::AwaitingDelivery:
        return "awaiting-delivery";
    case cache::TaskStatus::Cancelled:
        return "cancelled";
    case cache::TaskStatus::Completed:
        return "completed";
    }
    throw std::runtime_error("invalid query status");
}
} // namespace pagos::driver::query_task_cli
