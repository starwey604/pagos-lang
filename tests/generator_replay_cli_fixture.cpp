#include "../lib/cache/wire.h"
#include "../lib/stage/generator_replay_record.h"

#include <charconv>
#include <fstream>
#include <iostream>

// Test-only: reseal one real CLI capability with false value/bounds/source or
// a future observation version. Never linked into pagosc.
int main(int argc, char** argv) {
    using namespace pagos;
    if (argc != 4)
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
    const cache::Bytes bytes{std::istreambuf_iterator<char>(stream), {}};
    if (bytes.size() < 17)
        return 1;
    cache::wire::Reader header{std::span(bytes).subspan(9, 8), {}};
    const auto size = header.u64();
    if (!header.finish())
        return 1;
    const cache::FileStore store(argv[1]);
    const auto payload = store.read_object(
        {cache::ObjectKind::GeneratorReplayDescriptor, digest, size});
    if (!payload)
        return 1;
    auto descriptor = stage::decode_generator_replay_descriptor(*payload);
    if (!descriptor)
        return 1;
    const auto key = stage::generator_replay_key(descriptor->specialization);
    const auto candidate = store.lookup(key, descriptor->verified_inputs);
    if (!candidate)
        return 1;
    std::optional<cache::TypedValue> value;
    std::optional<stage::GeneratorReplayObservation> observation;
    for (const auto& artifact : candidate->artifacts) {
        if (!artifact.bytes)
            return 1;
        if (artifact.name == "value") {
            auto decoded = cache::decode_value(*artifact.bytes);
            if (decoded)
                value = std::move(*decoded);
        }
        if (artifact.name == "observation") {
            auto decoded =
                stage::decode_generator_replay_observation(*artifact.bytes);
            if (decoded)
                observation = *decoded;
        }
    }
    if (!value || !observation ||
        descriptor->specialization.arguments.size() != 1 ||
        value->type.element_type() != sema::Type::integer(32) ||
        value->type.length < 2)
        return 1;
    const std::string_view mode(argv[3]);
    if (mode == "value") {
        auto& element = std::get<std::vector<IntegerValue>>(value->value)[1];
        element = IntegerValue(static_cast<std::uint32_t>(element.bits() ^ 1U));
        observation->expected_result =
            *stage::generator_replay_value_fingerprint(*value);
    } else if (mode == "bounds") {
        ++observation->first;
        ++observation->last;
    } else if (mode == "source") {
        descriptor->element.nodes.back().operation =
            stage::ScalarReplayOp::Subtract;
        observation->descriptor =
            *stage::generator_replay_descriptor_fingerprint(*descriptor);
    } else if (mode != "version")
        return 2;
    auto encoded_descriptor =
        stage::encode_generator_replay_descriptor(*descriptor);
    auto encoded_observation =
        stage::encode_generator_replay_observation(*observation);
    auto encoded_value = cache::encode_value(*value);
    const auto fingerprint = cache::query_fingerprint(key);
    if (!encoded_descriptor || !encoded_observation || !encoded_value ||
        !fingerprint)
        return 1;
    if (mode == "version")
        (*encoded_observation)[9] = 2;
    const auto key_hex = fingerprint->hex();
    const auto manifest = store.root() / "queries" / key_hex.substr(0, 2) /
                          key_hex /
                          (descriptor->verified_inputs.hex() + ".manifest");
    if (!std::filesystem::remove(manifest))
        return 1;
    const std::array artifacts{
        cache::ArtifactInput{"value", cache::ObjectKind::Value, *encoded_value},
        cache::ArtifactInput{"generator",
                             cache::ObjectKind::GeneratorReplayDescriptor,
                             *encoded_descriptor},
        cache::ArtifactInput{"observation",
                             cache::ObjectKind::GeneratorReplayObservation,
                             *encoded_observation}};
    if (!store.publish(key, descriptor->verified_inputs,
                       stage::generator_detail::semantic(*encoded_value,
                                                         *encoded_descriptor,
                                                         *encoded_observation),
                       artifacts))
        return 1;
    std::cout << std::get<IntegerValue>(
                     *descriptor->specialization.arguments[0].value)
                     .decimal()
              << '\n';
}
