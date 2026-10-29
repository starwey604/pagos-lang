#pragma once

#include "pagos/cache/codec.h"

#include <optional>

namespace pagos::cache {

struct ModuleIdentity {
    std::string workspace;
    std::string relative_path;
    bool operator==(const ModuleIdentity&) const = default;
};
enum class DefinitionKind : std::uint8_t {
    Function = 1,
    Record = 2,
    Global = 3,
    Module = 4
};
struct StableDefinitionId {
    ModuleIdentity module;
    DefinitionKind kind{DefinitionKind::Function};
    std::string name;
    bool operator==(const StableDefinitionId&) const = default;
};
[[nodiscard]] bool valid_module_identity(const ModuleIdentity& module);
[[nodiscard]] bool valid_definition_id(const StableDefinitionId& definition);

// Fully resolved by the caller, never inferred from host sizeof or empty
// triple.
struct TargetSemantics {
    std::string triple;
    std::string cpu;
    std::string features;
    std::string data_layout;
    unsigned pointer_bits{};
    bool little_endian{};
    bool operator==(const TargetSemantics&) const = default;
};
struct QueryContext {
    ModuleIdentity module;
    TargetSemantics target;
    std::string compiler_build;
    std::string semantic_options;
    std::uint32_t query_version{query_protocol_version};
    std::uint32_t budget_version{budget_protocol_version};
    bool operator==(const QueryContext&) const = default;
};
[[nodiscard]] bool valid_context(const QueryContext& context);

enum class ArgumentStage : std::uint8_t { Comptime = 1, Runtime = 2 };
enum class RuntimeConstraint : std::uint8_t { None = 0, ExplicitBoundary = 1 };
struct StageArgument {
    sema::Type type;
    ArgumentStage stage;
    std::optional<Constant> value;
    RuntimeConstraint constraint{RuntimeConstraint::None};
    bool operator==(const StageArgument&) const = default;
};
enum class QueryKind : std::uint8_t {
    EvaluatePureSpecialization = 1,
    SpecializeResidual = 2,
    FreezeEvidence = 3,
    FreezeEvidencePositions = 4,
    FreezeFunctionEvidence = 5,
    FreezeScalarReplay = 6,
    FreezeGeneratorReplay = 7
};
struct QueryKey {
    QueryKind kind{QueryKind::EvaluatePureSpecialization};
    QueryContext context;
    StableDefinitionId definition;
    Digest definition_fingerprint;
    std::vector<StageArgument> arguments;
    bool operator==(const QueryKey&) const = default;
};

[[nodiscard]] std::expected<Bytes, Error> encode_query_key(const QueryKey& key,
                                                           Limits limits = {});
[[nodiscard]] std::expected<QueryKey, Error>
decode_query_key(std::span<const std::uint8_t> bytes, Limits limits = {});
[[nodiscard]] std::expected<Digest, Error>
query_fingerprint(const QueryKey& key, Limits limits = {});

} // namespace pagos::cache
