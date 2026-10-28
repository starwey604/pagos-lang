#include "build_identity.h"
#include "pagos/codegen/llvm_codegen.h"
#include "pagos/hir/hir.h"
#include "pagos/mir/lowering.h"
#include "pagos/mir/mir.h"
#include "pagos/sema/type_checker.h"
#include "pagos/source/diagnostic.h"
#include "pagos/source/source_manager.h"
#include "pagos/stage/function_evidence_view.h"
#include "pagos/stage/provenance_analysis.h"
#include "pagos/stage/provenance_snapshot.h"
#include "pagos/stage/stage_analyzer.h"
#include "pagos/syntax/lexer.h"
#include "pagos/syntax/parser.h"
#include "query_tasks.h"
#include "recursive_summaries.h"

#include <algorithm>
#include <charconv>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>

namespace {

enum class Command {
    Check,
    EmitHir,
    EmitMir,
    EmitLlvm,
    EmitObject,
    ExplainStage,
    ExplainProvenance
};

struct Options {
    Command command;
    std::string source_path;
    pagos::stage::AnalysisLimits limits;
    pagos::codegen::TargetConfig target;
    std::optional<std::string> output_path;
    pagos::stage::CacheConfig cache;
    bool cache_stats{};
    pagos::stage::ProvenanceQueryOptions provenance;
    std::optional<std::string> snapshot_write, snapshot_read;
    std::optional<std::string> evidence_write, evidence_read;
    std::string evidence_root, evidence_mode{"sources"};
    std::size_t evidence_cache_bytes{4 * 1024 * 1024};
    pagos::stage::provenance::EvidenceBuildConfig spill;
    bool spill_stats{};
    pagos::driver::QueryTaskOptions query;
    pagos::driver::RecursiveSummaryOptions recursive;
};

void print_usage() {
    std::println(
        stderr,
        "usage: pagosc [--max-fuel=N] "
        "[--max-recursion-depth=N] "
        "[--max-specializations=N] "
        "[--max-residual-specializations=N] "
        "[--max-array-elements=N] [--max-array-bytes=N] "
        "[--max-aggregate-members=N] [--max-aggregate-bytes=N] "
        "[--target=TRIPLE] [--cpu=CPU] [--features=FEATURES] "
        "[--stage-cache=off|read-write|read-only] "
        "[--cache-dir=PATH] [--cache-stats] "
        "[--provenance-query=legacy|resumable|compressed|topology] "
        "[--provenance-slice=N] [--provenance-query-stats] "
        "[--snapshot-write=DIR|--snapshot-read=DIR] "
        "[--evidence-write=DIR|--evidence-read=DIR] "
        "[--evidence-root=NAME] "
        "[--evidence-mode=sources|witness|topology|decisions|summaries|"
        "recursive-summary] "
        "[--evidence-summary=NAME|--evidence-summary-key=HEX] "
        "[--evidence-channel=value|effects|both] [--recursive-max-steps=N|all] "
        "[--evidence-cache-bytes=N] "
        "[--query-dir=DIR] [--query-id=HEX] "
        "[--query-action=start|resume|cancel] "
        "[--query-max-slices=N|all] [--query-ack=N] "
        "[--query-channel=value|effects] [--query-storage=memory|paged] "
        "[--query-read-only] [--query-disk-bytes=N] "
        "[--query-segment-bytes=N] [--query-resident-bytes=N] "
        "[--query-cache-bytes=N] [--query-max-records=N] "
        "[--evidence-spill=DIR] [--spill-block-bytes=N] "
        "[--spill-resident-bytes=N] [--spill-cache-bytes=N] "
        "[--spill-disk-bytes=N] [--spill-on-error=memory|unavailable] "
        "[--spill-stats] "
        "<check|emit-hir|emit-mir|emit-llvm|emit-obj|explain-stage|"
        "explain-provenance> "
        "<source.pgs> [-o output.o]\n"
        "       pagosc reclaim-evidence <temporary-parent>\n"
        "       pagosc reclaim-queries <query-parent> [--discard-resumable]");
}

std::optional<Command> parse_command(std::string_view name) {
    if (name == "check") {
        return Command::Check;
    }
    if (name == "emit-hir") {
        return Command::EmitHir;
    }
    if (name == "emit-mir") {
        return Command::EmitMir;
    }
    if (name == "emit-llvm") {
        return Command::EmitLlvm;
    }
    if (name == "emit-obj")
        return Command::EmitObject;
    if (name == "explain-stage") {
        return Command::ExplainStage;
    }
    if (name == "explain-provenance") {
        return Command::ExplainProvenance;
    }
    return std::nullopt;
}

std::expected<std::size_t, std::string> parse_limit(std::string_view option,
                                                    std::string_view prefix) {
    const auto spelling = option.substr(prefix.size());
    std::size_t value{};
    const auto result = std::from_chars(
        spelling.data(), spelling.data() + spelling.size(), value);
    if (spelling.empty() || result.ec != std::errc{} ||
        result.ptr != spelling.data() + spelling.size()) {
        return std::unexpected("invalid numeric option `" +
                               std::string(option) + "`");
    }
    return value;
}

std::expected<Options, std::string> parse_options(int argument_count,
                                                  char** arguments) {
    std::optional<Command> command;
    std::optional<std::string> source_path;
    pagos::stage::AnalysisLimits limits;
    pagos::codegen::TargetConfig target;
    std::optional<std::string> output_path;

    pagos::stage::CacheConfig cache;
    bool cache_stats = false;
    pagos::stage::ProvenanceQueryOptions provenance;
    provenance.mode = pagos::stage::ProvenanceQueryOptions::Mode::Resumable;
    pagos::driver::QueryTaskOptions query;
    pagos::driver::RecursiveSummaryOptions recursive;
    std::optional<std::string> snapshot_write, snapshot_read;
    std::optional<std::string> evidence_write, evidence_read;
    std::string evidence_root, evidence_mode{"sources"};
    std::size_t evidence_cache_bytes{4 * 1024 * 1024};
    pagos::stage::provenance::EvidenceBuildConfig spill;
    bool spill_stats = false, spill_options = false;
    bool provenance_options = false;

    for (int index = 1; index < argument_count; ++index) {
        const std::string_view argument{arguments[index]};
        constexpr std::string_view fuel_prefix = "--max-fuel=";
        constexpr std::string_view depth_prefix = "--max-recursion-depth=";
        constexpr std::string_view specialization_prefix =
            "--max-specializations=";
        constexpr std::string_view residual_specialization_prefix =
            "--max-residual-specializations=";
        constexpr std::string_view array_elements_prefix =
            "--max-array-elements=";
        constexpr std::string_view array_bytes_prefix = "--max-array-bytes=";
        constexpr std::string_view aggregate_members_prefix =
            "--max-aggregate-members=";
        constexpr std::string_view aggregate_bytes_prefix =
            "--max-aggregate-bytes=";
        if (argument.starts_with("--evidence-summary=") ||
            argument.starts_with("--evidence-summary-key=") ||
            argument.starts_with("--evidence-channel=") ||
            argument.starts_with("--recursive-max-steps=")) {
            const auto parsed = pagos::driver::parse_recursive_summary_option(
                argument, recursive);
            if (!parsed)
                return std::unexpected(parsed.error());
        } else if (argument.starts_with("--query-")) {
            const auto parsed =
                pagos::driver::parse_query_task_option(argument, query);
            if (!parsed)
                return std::unexpected(parsed.error());
        } else if (argument.starts_with("--evidence-spill=")) {
            const auto value = argument.substr(17);
            if (value.empty())
                return std::unexpected("spill directory must be non-empty");
            spill.mode = pagos::stage::provenance::EvidenceBuildMode::Spill;
            spill.directory = value;
        } else if (argument == "--spill-stats") {
            spill_options = spill_stats = true;
        } else if (argument.starts_with("--spill-on-error=")) {
            spill_options = true;
            const auto value = argument.substr(17);
            if (value == "memory")
                spill.failure_policy =
                    pagos::stage::provenance::SpillFailurePolicy::KeepMemory;
            else if (value == "unavailable")
                spill.failure_policy =
                    pagos::stage::provenance::SpillFailurePolicy::Unavailable;
            else
                return std::unexpected("invalid spill failure policy");
        } else if (argument.starts_with("--spill-block-bytes=") ||
                   argument.starts_with("--spill-resident-bytes=") ||
                   argument.starts_with("--spill-cache-bytes=") ||
                   argument.starts_with("--spill-disk-bytes=")) {
            spill_options = true;
            const auto split = argument.find('=') + 1;
            const auto value = parse_limit(argument, argument.substr(0, split));
            if (!value)
                return std::unexpected(value.error());
            if (argument.starts_with("--spill-block-")) {
                if (!*value)
                    return std::unexpected("spill block size must be positive");
                spill.block_bytes = *value;
            } else if (argument.starts_with("--spill-resident-"))
                spill.resident_bytes = *value;
            else if (argument.starts_with("--spill-cache-"))
                spill.cache_bytes = *value;
            else
                spill.disk_bytes = *value;
        } else if (argument.starts_with("--evidence-write=") ||
                   argument.starts_with("--evidence-read=")) {
            const auto value = argument.substr(argument.find('=') + 1);
            if (value.empty())
                return std::unexpected("evidence directory must be non-empty");
            auto& option = argument.starts_with("--evidence-write=")
                               ? evidence_write
                               : evidence_read;
            option = value;
        } else if (argument.starts_with("--evidence-root=")) {
            evidence_root = argument.substr(16);
            if (evidence_root.empty())
                return std::unexpected("evidence root must be non-empty");
        } else if (argument.starts_with("--evidence-mode=")) {
            evidence_mode = argument.substr(16);
            if (evidence_mode != "sources" && evidence_mode != "witness" &&
                evidence_mode != "topology" && evidence_mode != "decisions" &&
                !pagos::driver::recursive_evidence_mode(evidence_mode))
                return std::unexpected("invalid evidence query mode");
        } else if (argument.starts_with("--evidence-cache-bytes=")) {
            auto value = parse_limit(argument, "--evidence-cache-bytes=");
            if (!value)
                return std::unexpected(value.error());
            evidence_cache_bytes = *value;
        } else if (argument.starts_with("--snapshot-write=") ||
                   argument.starts_with("--snapshot-read=")) {
            const auto path = argument.substr(argument.find('=') + 1);
            if (path.empty())
                return std::unexpected("snapshot directory must be non-empty");
            auto& option = argument.starts_with("--snapshot-write=")
                               ? snapshot_write
                               : snapshot_read;
            option = path;
        } else if (argument.starts_with("--provenance-query=")) {
            provenance_options = true;
            const auto value = argument.substr(19);
            using Mode = pagos::stage::ProvenanceQueryOptions::Mode;
            if (value == "legacy")
                provenance.mode = Mode::Legacy;
            else if (value == "resumable")
                provenance.mode = Mode::Resumable;
            else if (value == "compressed")
                provenance.mode = Mode::Compressed;
            else if (value == "topology")
                provenance.mode = Mode::Topology;
            else
                return std::unexpected("invalid provenance query mode: " +
                                       std::string(value));
        } else if (argument.starts_with("--provenance-slice=")) {
            provenance_options = true;
            const auto value = parse_limit(argument, "--provenance-slice=");
            if (!value)
                return std::unexpected(value.error());
            if (!*value)
                return std::unexpected("provenance slice must be positive");
            provenance.slice = *value;
        } else if (argument == "--provenance-query-stats") {
            provenance_options = true;
            provenance.telemetry = true;
        } else if (argument.starts_with("--stage-cache=")) {
            const auto value = argument.substr(14);
            if (value == "off")
                cache.mode = pagos::stage::CacheMode::Off;
            else if (value == "read-write")
                cache.mode = pagos::stage::CacheMode::ReadWrite;
            else if (value == "read-only")
                cache.mode = pagos::stage::CacheMode::ReadOnly;
            else
                return std::unexpected("invalid Stage cache mode: " +
                                       std::string(value));
        } else if (argument.starts_with("--cache-dir=")) {
            const auto value = argument.substr(12);
            if (value.empty())
                return std::unexpected("--cache-dir requires a non-empty path");
            cache.directory = value;
        } else if (argument == "--cache-stats") {
            cache_stats = true;
        } else if (argument == "-o") {
            if (output_path || index + 1 >= argument_count)
                return std::unexpected("-o requires exactly one output path");
            output_path = arguments[++index];
            if (output_path->empty() || output_path->starts_with('-'))
                return std::unexpected("invalid object output path");
        } else if (argument.starts_with("--target=") ||
                   argument.starts_with("--cpu=") ||
                   argument.starts_with("--features=")) {
            const auto value = argument.substr(argument.find('=') + 1);
            if (value.empty())
                return std::unexpected("empty target option: " +
                                       std::string(argument));
            if (argument.starts_with("--target="))
                target.triple = value;
            else if (argument.starts_with("--cpu="))
                target.cpu = value;
            else
                target.features = value;
        } else if (argument.starts_with(fuel_prefix)) {
            auto value = parse_limit(argument, fuel_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.fuel = *value;
        } else if (argument.starts_with(depth_prefix)) {
            auto value = parse_limit(argument, depth_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.recursion_depth = *value;
        } else if (argument.starts_with(specialization_prefix)) {
            auto value = parse_limit(argument, specialization_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.specializations = *value;
        } else if (argument.starts_with(residual_specialization_prefix)) {
            auto value = parse_limit(argument, residual_specialization_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.residual_specializations = *value;
        } else if (argument.starts_with(array_elements_prefix)) {
            auto value = parse_limit(argument, array_elements_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.array_elements = *value;
        } else if (argument.starts_with(array_bytes_prefix)) {
            auto value = parse_limit(argument, array_bytes_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.array_bytes = *value;
        } else if (argument.starts_with(aggregate_members_prefix)) {
            auto value = parse_limit(argument, aggregate_members_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.aggregate_members = *value;
        } else if (argument.starts_with(aggregate_bytes_prefix)) {
            auto value = parse_limit(argument, aggregate_bytes_prefix);
            if (!value) {
                return std::unexpected(value.error());
            }
            limits.aggregate_bytes = *value;
        } else if (argument.starts_with("--")) {
            return std::unexpected("unknown option `" + std::string(argument) +
                                   "`");
        } else if (!command) {
            command = parse_command(argument);
            if (!command) {
                return std::unexpected("unknown command `" +
                                       std::string(argument) + "`");
            }
        } else if (!source_path) {
            source_path = std::string(argument);
        } else {
            return std::unexpected("unexpected argument `" +
                                   std::string(argument) + "`");
        }
    }
    if (!command || !source_path) {
        return std::unexpected("a command and source file are required");
    }
    if ((*command == Command::EmitObject) != output_path.has_value())
        return std::unexpected(
            "emit-obj requires -o; other commands do not accept -o");
    if (cache.mode != pagos::stage::CacheMode::Off && cache.directory.empty())
        return std::unexpected("enabled Stage cache requires --cache-dir=PATH");
    if (provenance_options && *command != Command::ExplainProvenance)
        return std::unexpected(
            "provenance query options require explain-provenance");
    if ((snapshot_write || snapshot_read) &&
        *command != Command::ExplainProvenance)
        return std::unexpected("snapshot options require explain-provenance");
    if (snapshot_write && snapshot_read)
        return std::unexpected(
            "snapshot read and write are mutually exclusive");
    if (snapshot_read &&
        (cache.mode != pagos::stage::CacheMode::Off || cache_stats))
        return std::unexpected(
            "snapshot read does not accept Stage cache options");
    if (spill_options &&
        spill.mode != pagos::stage::provenance::EvidenceBuildMode::Spill)
        return std::unexpected("spill options require --evidence-spill=DIR");
    if (spill.mode == pagos::stage::provenance::EvidenceBuildMode::Spill &&
        (snapshot_read || evidence_read))
        return std::unexpected("spill requires Stage analysis and excludes "
                               "evidence/snapshot read");
    const bool function_evidence = evidence_write || evidence_read;
    if (function_evidence && (*command != Command::ExplainProvenance ||
                              snapshot_read || snapshot_write))
        return std::unexpected("function evidence requires explain-provenance "
                               "and excludes snapshot options");
    if (evidence_read && evidence_write)
        return std::unexpected(
            "evidence read and write are mutually exclusive");
    if (evidence_read &&
        (cache.mode != pagos::stage::CacheMode::Off || cache_stats))
        return std::unexpected(
            "evidence read does not accept Stage cache options");
    if (!function_evidence &&
        (!evidence_root.empty() || evidence_mode != "sources" ||
         evidence_cache_bytes != 4 * 1024 * 1024))
        return std::unexpected(
            "evidence query options require evidence read or write");
    if (function_evidence &&
        (provenance.mode ==
             pagos::stage::ProvenanceQueryOptions::Mode::Compressed ||
         provenance.mode ==
             pagos::stage::ProvenanceQueryOptions::Mode::Topology))
        return std::unexpected(
            "function evidence uses --evidence-mode for topology and sources");
    const auto recursive_valid =
        pagos::driver::validate_recursive_summary_options(
            recursive, evidence_mode, evidence_root, query.specified);
    if (!recursive_valid)
        return std::unexpected(recursive_valid.error());
    if (pagos::driver::recursive_evidence_mode(evidence_mode) &&
        provenance.mode !=
            pagos::stage::ProvenanceQueryOptions::Mode::Resumable)
        return std::unexpected(
            "recursive summaries require resumable query mode");
    if (query.specified) {
        const auto valid = pagos::driver::validate_query_task_options(
            query, evidence_read.has_value(), evidence_write.has_value(),
            evidence_root, evidence_mode);
        if (!valid)
            return std::unexpected(valid.error());
        if (evidence_mode == "recursive-summary" && query.read_only &&
            recursive.steps_set)
            return std::unexpected(
                "read-only recursive tasks do not accept solver budgets");
        if (provenance.mode !=
            pagos::stage::ProvenanceQueryOptions::Mode::Resumable)
            return std::unexpected(
                "query tasks require the resumable provenance query mode");
    }
    return Options{.command = *command,
                   .source_path = std::move(*source_path),
                   .limits = limits,
                   .target = std::move(target),
                   .output_path = std::move(output_path),
                   .cache = std::move(cache),
                   .cache_stats = cache_stats,
                   .provenance = provenance,
                   .snapshot_write = snapshot_write,
                   .snapshot_read = snapshot_read,
                   .evidence_write = evidence_write,
                   .evidence_read = evidence_read,
                   .evidence_root = evidence_root,
                   .evidence_mode = evidence_mode,
                   .evidence_cache_bytes = evidence_cache_bytes,
                   .spill = std::move(spill),
                   .spill_stats = spill_stats,
                   .query = std::move(query),
                   .recursive = std::move(recursive)};
}

std::expected<void, std::string> write_object(const std::string& input,
                                              const std::string& output,
                                              const std::string& bytes) {
    std::error_code error;
    if (std::filesystem::equivalent(input, output, error))
        return std::unexpected(
            "object output must not replace the source file");
    // Exclusive sibling creation, then atomic replacement. Existing output
    // survives compilation/write failures; an existing .tmp is never touched.
    const auto temporary = output + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::out |
                                        std::ios::noreplace);
    if (!stream)
        return std::unexpected("cannot create object temporary file: " +
                               temporary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
        std::filesystem::remove(temporary, error);
        return std::unexpected("cannot write object file: " + output);
    }
    std::filesystem::rename(temporary, output, error);
    if (error) {
        const auto message = error.message();
        std::filesystem::remove(temporary, error);
        return std::unexpected("cannot replace object output: " + message);
    }
    return {};
}

} // namespace

int main(int argument_count, char** arguments) {
    if (argument_count >= 2 &&
        std::string_view(arguments[1]) == "reclaim-queries")
        return pagos::driver::reclaim_query_tasks(argument_count, arguments);
    if (argument_count == 3 &&
        std::string_view(arguments[1]) == "reclaim-evidence") {
        const auto result =
            pagos::stage::provenance::reclaim_evidence_sessions(arguments[2]);
        if (!result) {
            std::println(stderr, "error: evidence cleanup failed ({})",
                         static_cast<int>(result.error().code));
            return 1;
        }
        std::println("evidence-cleanup: examined={}, reclaimed={}, active={}, "
                     "skipped={}, errors={}",
                     result->examined, result->reclaimed, result->active,
                     result->skipped, result->errors);
        return result->errors ? 1 : 0;
    }
    const auto options = parse_options(argument_count, arguments);
    if (!options) {
        std::println(stderr, "error: {}", options.error());
        print_usage();
        return 2;
    }

    auto source = pagos::source::SourceFile::load(options->source_path);
    if (!source) {
        std::println(stderr, "error: {}", source.error());
        return 1;
    }
    const auto target = pagos::codegen::TargetLayout::create(options->target);
    if (!target) {
        std::println(stderr, "error: {}", target.error());
        return 1;
    }
    pagos::source::DiagnosticEngine diagnostics(*source);
    pagos::syntax::Lexer lexer(*source, diagnostics);
    auto tokens = lexer.tokenize();
    if (diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }

    pagos::syntax::Parser parser(tokens, diagnostics);
    auto syntax_module = parser.parse_module();
    if (diagnostics.has_error()) {
        diagnostics.render(std::cerr);
        return 1;
    }

    pagos::sema::TypeChecker type_checker(diagnostics, target->pointer_bits());
    if (!type_checker.check(*syntax_module)) {
        diagnostics.render(std::cerr);
        return 1;
    }
    if (std::ranges::any_of(
            syntax_module->functions,
            [](const auto& function) {
                return function->linkage !=
                       pagos::syntax::Function::Linkage::Internal;
            }) &&
        !target->supports_minimal_c_abi()) {
        std::println(stderr, "error: minimal C ABI requires x86-64 Linux LP64 "
                             "or generic RV32 ELF ILP32 (I/M/A/C)");
        return 1;
    }

    // Explicit module identity and resolved target for future Stage queries.
    // Outside the working directory, use the source's parent as its workspace.
    std::optional<pagos::cache::QueryContext> query_context;
    std::error_code path_error;
    const auto source_path =
        std::filesystem::absolute(options->source_path, path_error)
            .lexically_normal();
    if (!path_error) {
        auto workspace = std::filesystem::current_path(path_error);
        if (!path_error) {
            auto relative = source_path.lexically_relative(workspace);
            if (relative.empty() || *relative.begin() == "..") {
                workspace = source_path.parent_path();
                relative = source_path.filename();
            }
            const auto& config = target->config();
            query_context = pagos::cache::QueryContext{
                .module = {workspace.generic_string(),
                           relative.generic_string()},
                .target = {config.triple, config.cpu, config.features,
                           target->data_layout(), target->pointer_bits(),
                           target->little_endian()},
                .compiler_build = pagos_cache_build_id,
                .semantic_options = "pagos.staging.v1"};
        }
    }
    std::optional<pagos::stage::CacheIdentity> snapshot_identity;
    if (options->snapshot_read || options->snapshot_write ||
        options->evidence_read || options->evidence_write) {
        if (!query_context) {
            std::println(stderr, "error: snapshot requires module identity");
            return 1;
        }
        auto identity = pagos::stage::build_cache_identity(
            *syntax_module, type_checker.types(), *query_context);
        if (!identity) {
            std::println(stderr, "error: snapshot source identity unavailable");
            return 1;
        }
        snapshot_identity = std::move(*identity);
        if (options->evidence_read) {
            auto loaded = pagos::stage::FunctionEvidence::load(
                pagos::cache::FileStore(*options->evidence_read),
                *snapshot_identity, *query_context);
            if (!loaded) {
                std::println(stderr,
                             "error: function evidence load failed ({})",
                             static_cast<int>(loaded.error().code));
                return 1;
            }
            auto view = pagos::stage::FunctionEvidenceView::open(
                std::move(*loaded), options->evidence_cache_bytes);
            if (!view) {
                std::println(stderr,
                             "error: function evidence view failed ({})",
                             static_cast<int>(view.error().code));
                return 1;
            }
            if (pagos::driver::recursive_evidence_mode(options->evidence_mode))
                return pagos::driver::run_recursive_summaries(
                    *view, source->text(), options->evidence_mode,
                    options->provenance.slice, options->recursive, false,
                    options->provenance.telemetry, options->query);
            if (options->query.enabled())
                return pagos::driver::run_query_task(
                    *view, source->text(), options->evidence_root,
                    options->evidence_mode, options->provenance.slice,
                    options->query, false, options->provenance.telemetry);
            return pagos::stage::explain_function_evidence(
                       *view, std::cout, std::cerr, options->evidence_root,
                       options->evidence_mode, options->provenance.slice,
                       options->provenance.telemetry)
                       ? 0
                       : 1;
        }
        if (options->snapshot_read) {
            const pagos::cache::FileStore store(*options->snapshot_read);
            const auto loaded = pagos::stage::load_snapshot(
                store, *snapshot_identity, *query_context);
            if (!loaded) {
                std::println(stderr, "error: snapshot load failed ({})",
                             static_cast<int>(loaded.error().code));
                return 1;
            }
            pagos::stage::explain_provenance_experimental(
                **loaded, std::cout, std::cerr, options->provenance);
            return 0;
        }
    }
    auto spill_config = options->spill;
    spill_config.measure_memory = options->spill_stats;
    auto cache_config = options->cache;
    cache_config.measure_timings = options->cache_stats;
    pagos::stage::StageAnalyzer stage_analyzer(
        diagnostics, type_checker.types(), options->limits, query_context,
        std::move(cache_config), std::move(spill_config));
    auto hir_module = stage_analyzer.analyze(*syntax_module);
    struct SpillReport {
        bool enabled;
        std::shared_ptr<const pagos::stage::ProvenanceAnalysis> analysis;
        ~SpillReport() {
            if (!enabled)
                return;
            const auto& build = analysis->construction_stats();
            const auto read = analysis->evidence()->read_stats();
            const auto failure = analysis->failure();
            std::println(
                stderr,
                "evidence-spill: available={}, error={}, io={}, "
                "materialized={}, nodes={}, "
                "blocks={}, written={}, fallback={}, payload-written={}, "
                "disk-charged={}, active={}, resident={}, cache={}, live={}, "
                "peak-owned={}, peak-live={}, directory={}, summaries={}, "
                "released={}, build-reads={}, build-read-bytes={}, "
                "query-reads={}, query-read-bytes={}, query-cache={}, "
                "query-live={}",
                analysis->available() ? 1 : 0,
                failure ? static_cast<int>(failure->code) : -1,
                failure && failure->storage ? failure->storage->io.value() : 0,
                analysis->graph_materialized() ? 1 : 0, build.nodes,
                build.sealed_blocks, build.written_blocks,
                build.fallback_blocks, build.payload_bytes_written,
                build.charged_disk_bytes, build.active_bytes,
                build.resident_bytes, build.cached_bytes, build.live_bytes,
                build.peak_owned_bytes, build.peak_live_bytes,
                build.directory_bytes, build.summary_bytes,
                build.released_active_bytes, build.objects_read,
                build.payload_bytes_read, read.objects_read,
                read.payload_bytes_read, read.cached_bytes, read.live_bytes);
        }
    } spill_report{options->spill_stats, hir_module->provenance};
    if (options->spill_stats) {
        const auto memory = hir_module->provenance->memory_stats();
        const auto stage = stage_analyzer.memory_stats();
        std::println(stderr,
                     "evidence-memory: sites-bytes={}, roots-bytes={}, "
                     "decisions-bytes={}, scopes-bytes={}, "
                     "specialization-bytes={}, recorder-maps-bytes={}, "
                     "definition-bytes={}, expired-memos={}, pruned-memos={}, "
                     "analyzer-before-bytes={}, analyzer-after-bytes={}",
                     memory.sites_bytes, memory.roots_bytes,
                     memory.decisions_bytes, memory.scopes_bytes,
                     memory.specialization_bytes, memory.recorder_maps_bytes,
                     memory.definition_identity_bytes,
                     memory.expired_memo_entries, memory.pruned_memo_entries,
                     stage.before_release.tables_bytes +
                         stage.before_release.arguments_bytes +
                         stage.before_release.cache_bytes,
                     stage.after_release.tables_bytes +
                         stage.after_release.arguments_bytes +
                         stage.after_release.cache_bytes);
    }
    if (options->cache_stats) {
        const auto& stats = stage_analyzer.cache_telemetry();
        std::println(
            stderr,
            "stage-cache: lookups={}, hits={}, misses={}, writes={}, "
            "errors={}, rejected={}, budget-fallbacks={}, "
            "blocked-writes={}, pending-payload-peak={}, "
            "residual-lookups={}, residual-hits={}, residual-writes={}, "
            "body-evaluations={}, bodies-skipped={}, "
            "dependency-ns={}, query-ns={}, read-ns={}, decode-ns={}, "
            "encode-ns={}, publish-ns={}, resource-ns={}, "
            "restore-ns={}, evaluate-ns={}",
            stats.lookups, stats.hits, stats.misses, stats.writes, stats.errors,
            stats.rejected, stats.budget_fallbacks, stats.blocked_writes,
            stats.pending_payload_bytes_peak, stats.residual_lookups,
            stats.residual_hits, stats.residual_writes, stats.body_evaluations,
            stats.bodies_skipped, stats.dependency_ns, stats.query_ns,
            stats.read_ns, stats.decode_ns, stats.encode_ns, stats.publish_ns,
            stats.resource_ns, stats.restore_ns, stats.evaluate_ns);
    }
    if (diagnostics.has_error()) {
        if (options->command == Command::ExplainProvenance &&
            !options->query.enabled() &&
            !pagos::driver::recursive_evidence_mode(options->evidence_mode))
            pagos::stage::explain_provenance_experimental(
                *hir_module->provenance, std::cout, std::cerr,
                options->provenance);
        diagnostics.render(std::cerr);
        return 1;
    }

    if (options->evidence_write) {
        const pagos::cache::FileStore store(*options->evidence_write);
        auto published = pagos::stage::FunctionEvidence::publish(
            store, *hir_module->provenance, *snapshot_identity, *query_context);
        if (!published) {
            std::println(stderr, "error: function evidence publish failed ({})",
                         static_cast<int>(published.error().code));
            return 1;
        }
        auto loaded = pagos::stage::FunctionEvidence::load(
            store, *snapshot_identity, *query_context);
        if (!loaded)
            return 1;
        auto view = pagos::stage::FunctionEvidenceView::open(
            std::move(*loaded), options->evidence_cache_bytes);
        if (!view)
            return 1;
        if (pagos::driver::recursive_evidence_mode(options->evidence_mode))
            return pagos::driver::run_recursive_summaries(
                *view, source->text(), options->evidence_mode,
                options->provenance.slice, options->recursive, true,
                options->provenance.telemetry, options->query);
        if (options->query.enabled())
            return pagos::driver::run_query_task(
                *view, source->text(), options->evidence_root,
                options->evidence_mode, options->provenance.slice,
                options->query, true, options->provenance.telemetry);
        return pagos::stage::explain_function_evidence(
                   *view, std::cout, std::cerr, options->evidence_root,
                   options->evidence_mode, options->provenance.slice,
                   options->provenance.telemetry)
                   ? 0
                   : 1;
    }
    if (options->snapshot_write) {
        const auto encoded = pagos::stage::ProvenanceSnapshot::encode(
            *hir_module->provenance, *snapshot_identity, *query_context,
            *source);
        if (!encoded) {
            std::println(stderr, "error: snapshot encoding failed ({})",
                         static_cast<int>(encoded.error()));
            return 1;
        }
        const pagos::cache::FileStore store(*options->snapshot_write);
        const auto published = pagos::stage::publish_snapshot(store, *encoded);
        if (!published) {
            std::println(stderr, "error: snapshot publish failed ({})",
                         static_cast<int>(published.error().code));
            return 1;
        }
    }
    switch (options->command) {
    case Command::Check:
        return 0;
    case Command::EmitHir:
        pagos::hir::print(*hir_module, std::cout);
        return 0;
    case Command::ExplainProvenance:
        pagos::stage::explain_provenance_experimental(
            *hir_module->provenance, std::cout, std::cerr, options->provenance);
        return hir_module->provenance->available() ? 0 : 1;
    case Command::ExplainStage:
        pagos::hir::explain_stages(*hir_module, std::cout);
        std::println(std::cout,
                     "analysis: fuel={}, specializations={}, cache-hits={}, "
                     "max-depth={}, array-elements={}, array-bytes={}, "
                     "aggregate-constructions={}, aggregate-members={}, "
                     "aggregate-bytes={}, residual-specializations={}, "
                     "residual-cache-hits={}",
                     stage_analyzer.stats().fuel_consumed,
                     stage_analyzer.stats().specializations,
                     stage_analyzer.stats().cache_hits,
                     stage_analyzer.stats().maximum_recursion_depth,
                     stage_analyzer.stats().array_elements_reserved,
                     stage_analyzer.stats().array_bytes_reserved,
                     stage_analyzer.stats().aggregate_constructions,
                     stage_analyzer.stats().aggregate_members_reserved,
                     stage_analyzer.stats().aggregate_bytes_reserved,
                     stage_analyzer.stats().residual_specializations,
                     stage_analyzer.stats().residual_cache_hits);
        return 0;
    case Command::EmitMir:
    case Command::EmitLlvm:
    case Command::EmitObject:
        break;
    }

    auto mir_module = pagos::mir::lower(*hir_module);
    if (!mir_module) {
        std::println(stderr, "error: {}", mir_module.error());
        return 1;
    }
    if (options->command == Command::EmitMir) {
        pagos::mir::print(*mir_module, std::cout);
        return 0;
    }
    if (options->command == Command::EmitObject) {
        const auto bytes = pagos::codegen::LLVMCodegen::emit_object_for_target(
            *mir_module, *target);
        if (!bytes) {
            std::println(stderr, "error: {}", bytes.error());
            return 1;
        }
        if (auto written = write_object(options->source_path,
                                        *options->output_path, *bytes);
            !written) {
            std::println(stderr, "error: {}", written.error());
            return 1;
        }
        return 0;
    }

    auto llvm_ir =
        pagos::codegen::LLVMCodegen::emit_for_target(*mir_module, *target);
    if (!llvm_ir) {
        std::println(stderr, "error: {}", llvm_ir.error());
        return 1;
    }
    std::cout << *llvm_ir;
    return 0;
}
