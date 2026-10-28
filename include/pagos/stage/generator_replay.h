#pragma once

#include "pagos/stage/replay_descriptor.h"

namespace pagos::stage {
inline constexpr std::uint32_t generator_replay_descriptor_version = 1;
inline constexpr std::uint32_t closed_generator_semantics_version = 1;

// Describes a single tail generator in an internal, statement-free function.
// The key owns ALL parameters as typed Comptime snapshots, including unused
// captures. Begin/end recipes may read those slots; the element recipe may
// additionally read iterator_slot (u32, shadowing a same-named parameter).
// Ranges use Stage's [begin, end) semantics. This description does not assert
// that bounds evaluate successfully, are increasing, or match result length.
struct GeneratorReplayDescriptor {
    std::uint32_t version{generator_replay_descriptor_version};
    std::uint32_t semantics{closed_generator_semantics_version};
    std::uint32_t scalar_semantics{scalar_recipe_semantics_version};
    cache::QueryKey specialization;
    cache::Digest verified_inputs;
    SourceAnchor generator_site;
    SourceAnchor iterator_site;
    std::uint32_t iterator_slot{}; // Exactly specialization.arguments.size().
    sema::Type result_type;        // Declared, concrete scalar array signature.
    ScalarReplayRecipe begin;
    ScalarReplayRecipe end;
    ScalarReplayRecipe element;
    bool operator==(const GeneratorReplayDescriptor&) const = default;
};
// Codec success establishes structural admission only. In particular, it does
// not certify element independence or correspondence to checked source.
[[nodiscard]] std::expected<cache::Bytes, ReplayError>
encode_generator_replay_descriptor(const GeneratorReplayDescriptor& descriptor,
                                   cache::Limits limits = {});
[[nodiscard]] std::expected<GeneratorReplayDescriptor, ReplayError>
decode_generator_replay_descriptor(std::span<const std::uint8_t> bytes,
                                   cache::Limits limits = {});
[[nodiscard]] std::expected<cache::Digest, ReplayError>
generator_replay_descriptor_fingerprint(
    const GeneratorReplayDescriptor& descriptor, cache::Limits limits = {});

// Source-derived capability, deliberately not serializable or constructible
// from a descriptor alone. Owns no AST pointers. Certifies only that the body
// is a closed scalar expression with no cross-iteration state or effects.
// It is not a recorded computation, result verification, or replay engine.
class GeneratorReplayAdmission {
  public:
    [[nodiscard]] const GeneratorReplayDescriptor& descriptor() const {
        return descriptor_;
    }

  private:
    explicit GeneratorReplayAdmission(GeneratorReplayDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}
    GeneratorReplayDescriptor descriptor_;
    friend std::expected<GeneratorReplayAdmission, ReplayError>
    admit_generator_replay(const syntax::Module&, const sema::TypeTable&,
                           const cache::QueryKey&, cache::Limits);
};
// Requires a successfully checked AST and its matching type table. Rebuilds
// identity from source, checks target/signature/key and all captures, then
// admits only pure scalar bound/body trees. No Stage, arithmetic execution,
// filesystem access, or fallback synthesized from an existing array value.
[[nodiscard]] std::expected<GeneratorReplayAdmission, ReplayError>
admit_generator_replay(const syntax::Module& module,
                       const sema::TypeTable& types,
                       const cache::QueryKey& specialization,
                       cache::Limits limits = {});
// Missing or forged descriptions cannot become capabilities. Re-admits actual
// source and compares the ENTIRE description, including slots and recipes.
[[nodiscard]] std::expected<GeneratorReplayAdmission, ReplayError>
validate_generator_replay(const GeneratorReplayDescriptor* descriptor,
                          const syntax::Module& module,
                          const sema::TypeTable& types,
                          const cache::QueryKey& specialization,
                          cache::Limits limits = {});
} // namespace pagos::stage
