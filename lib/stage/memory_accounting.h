#pragma once

#include "pagos/stage/cache_identity.h"

#include <type_traits>

namespace pagos::stage::memory {
inline std::size_t heap(const std::string& text) {
    const auto data = reinterpret_cast<std::uintptr_t>(text.data());
    const auto object = reinterpret_cast<std::uintptr_t>(&text);
    return data < object || data >= object + sizeof(text) ? text.capacity() + 1
                                                          : 0;
}
template <class T> std::size_t vector(const std::vector<T>& values) {
    return values.capacity() * sizeof(T);
}
template <class T> std::size_t map(const T& values) {
    return values.size() * sizeof(typename T::value_type) +
           (values.bucket_count() > 1 ? values.bucket_count() * sizeof(void*)
                                      : 0);
}
inline std::size_t heap(const sema::Type& type) {
    return heap(type.record_name);
}
inline std::size_t heap(const Constant& constant) {
    return std::visit(
        [](const auto& value) -> std::size_t {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RecordConstant>)
                return heap(value.name) + vector(value.fields);
            else if constexpr (std::is_same_v<T, std::vector<bool>>)
                return (value.capacity() + 7) / 8;
            else if constexpr (std::is_same_v<T, std::vector<IntegerValue>>)
                return vector(value);
            else
                return 0;
        },
        constant);
}
inline std::size_t heap(const cache::ModuleIdentity& id) {
    return heap(id.workspace) + heap(id.relative_path);
}
inline std::size_t heap(const cache::StableDefinitionId& id) {
    return heap(id.module) + heap(id.name);
}
inline std::size_t heap(const cache::QueryContext& context) {
    return heap(context.module) + heap(context.compiler_build) +
           heap(context.semantic_options) + heap(context.target.triple) +
           heap(context.target.cpu) + heap(context.target.features) +
           heap(context.target.data_layout);
}
inline std::size_t heap(const DefinitionIdentity& id) {
    auto bytes = heap(id.id) + vector(id.parameters) + heap(id.result);
    for (const auto& type : id.parameters)
        bytes += heap(type);
    return bytes;
}
inline std::size_t heap(const CacheIdentity& identity) {
    auto bytes = heap(identity.module) + vector(identity.definitions) +
                 vector(identity.sites);
    for (const auto& id : identity.definitions)
        bytes += heap(id);
    for (const auto& site : identity.sites)
        bytes += heap(site.anchor.definition);
    return bytes;
}
inline std::size_t heap(const cache::QueryKey& key) {
    auto bytes =
        heap(key.context) + heap(key.definition) + vector(key.arguments);
    for (const auto& argument : key.arguments)
        bytes +=
            heap(argument.type) + (argument.value ? heap(*argument.value) : 0);
    return bytes;
}
} // namespace pagos::stage::memory
