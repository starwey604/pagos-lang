#include "../lib/cache/wire.h"
#include "../lib/stage/scalar_replay.h"

#include <charconv>
#include <fstream>
#include <iostream>

// Test-only corruption fixture: reseals a real CLI record with a false value,
// leaving its source recipe intact. It is never linked into pagosc.
int main(int argc, char** argv) {
    using namespace pagos;
    if (argc != 3)
        return 2;
    const std::filesystem::path path(argv[2]);
    const auto hex = path.filename().string();
    if (hex.size() != 64)
        return 2;
    cache::Digest digest;
    for (std::size_t i = 0; i < 32; ++i) {
        unsigned number{};
        const auto part = std::string_view(hex).substr(i * 2, 2);
        const auto parsed =
            std::from_chars(part.data(), part.data() + 2, number, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + 2)
            return 2;
        digest.bytes[i] = static_cast<std::uint8_t>(number);
    }
    std::ifstream stream(path, std::ios::binary);
    const cache::Bytes bytes((std::istreambuf_iterator<char>(stream)), {});
    if (bytes.size() < 17)
        return 1;
    cache::wire::Reader header{std::span(bytes).subspan(9, 8), {}};
    const auto size = header.u64();
    if (!header.finish())
        return 1;
    const cache::FileStore store(argv[1]);
    const auto payload =
        store.read_object({cache::ObjectKind::ReplayDescriptor, digest, size});
    if (!payload)
        return 1;
    auto descriptor = stage::decode_replay_descriptor(*payload);
    if (!descriptor)
        return 1;
    const auto key = stage::scalar_replay_key(descriptor->specialization);
    const auto candidate = store.lookup(key, descriptor->verified_inputs);
    if (!candidate)
        return 1;
    const cache::StoredArtifact* stored_value = nullptr;
    for (const auto& artifact : candidate->artifacts)
        if (artifact.name == "value" && artifact.bytes)
            stored_value = &artifact;
    if (!stored_value)
        return 1;
    auto value = cache::decode_value(*stored_value->bytes);
    if (!value || value->type.kind != sema::TypeKind::Integer ||
        descriptor->specialization.arguments.size() != 1)
        return 1;
    const auto integer = std::get<IntegerValue>(value->value);
    value->value = *IntegerValue::create(integer.type(), integer.bits() ^ 1U);
    descriptor->expected_result = *stage::replay_value_fingerprint(*value);
    const auto changed = stage::encode_replay_descriptor(*descriptor);
    const auto encoded_value = cache::encode_value(*value);
    const auto fingerprint = cache::query_fingerprint(key);
    if (!changed || !encoded_value || !fingerprint)
        return 1;
    const auto key_hex = fingerprint->hex();
    const auto manifest = store.root() / "queries" / key_hex.substr(0, 2) /
                          key_hex /
                          (descriptor->verified_inputs.hex() + ".manifest");
    if (!std::filesystem::remove(manifest))
        return 1;
    const std::array artifacts{
        cache::ArtifactInput{"value", cache::ObjectKind::Value, *encoded_value},
        cache::ArtifactInput{"replay", cache::ObjectKind::ReplayDescriptor,
                             *changed}};
    if (!store.publish(key, descriptor->verified_inputs,
                       stage::replay_detail::semantic(*encoded_value, *changed),
                       artifacts))
        return 1;
    std::cout << std::get<IntegerValue>(
                     *descriptor->specialization.arguments[0].value)
                     .decimal()
              << '\n';
}
