#include "scalar_replay.h"
#include "pagos/stage/generator_replay_execution.h"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <thread>

namespace pagos::driver {
namespace {
using namespace stage;
std::atomic_flag interrupted = ATOMIC_FLAG_INIT;
void interrupt(int) {
    (void)interrupted.test_and_set(std::memory_order_relaxed);
}
// Only a guaranteed lock-free atomic flag operation runs in the signal handler.
// The watcher bridges it to stop_token outside signal context; it owns no
// compiler state.
class Cancellation {
  public:
    Cancellation() {
        interrupted.clear(std::memory_order_relaxed);
        previous_ = std::signal(SIGINT, interrupt);
        if (previous_ == SIG_ERR)
            throw std::runtime_error("cannot install replay interrupt handler");
        try {
            watcher_ = std::jthread([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    if (interrupted.test(std::memory_order_relaxed)) {
                        source_.request_stop();
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });
        } catch (...) {
            std::signal(SIGINT, previous_);
            throw;
        }
    }
    ~Cancellation() {
        watcher_.request_stop();
        watcher_.join();
        std::signal(SIGINT, previous_);
    }
    std::stop_token token() const { return source_.get_token(); }
    bool requested() const {
        return interrupted.test(std::memory_order_relaxed) ||
               source_.stop_requested();
    }

  private:
    void (*previous_)(int){};
    std::stop_source source_;
    std::jthread watcher_;
};
struct RenderLimit {};
class Buffer : public std::streambuf {
  public:
    explicit Buffer(std::size_t limit) : limit_(limit) {}
    std::string bytes;

  private:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count < 0 ||
            static_cast<std::size_t>(count) > limit_ - bytes.size())
            throw RenderLimit{};
        bytes.append(data, static_cast<std::size_t>(count));
        return count;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        const auto byte = traits_type::to_char_type(value);
        (void)xsputn(&byte, 1);
        return value;
    }
    std::size_t limit_;
};
void quote(std::ostream& out, std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    out << '"';
    for (const auto byte : value) {
        const auto code = static_cast<unsigned char>(byte);
        if (byte == '"' || byte == '\\')
            out << '\\' << byte;
        else if (code < 32)
            out << "\\u00" << hex[code >> 4U] << hex[code & 15U];
        else
            out << byte;
    }
    out << '"';
}
void value(std::ostream& out, const sema::Type& type,
           const Constant& constant) {
    out << "{\"type\":";
    quote(out, sema::type_name(type));
    out << ",\"value\":";
    std::ostringstream text;
    print_constant(constant, text);
    quote(out, text.str()); // Decimal strings preserve signed/64-bit values.
    out << '}';
}
void site(std::ostream& out, const CacheIdentity& identity,
          const SourceAnchor& anchor, const source::SourceFile& source) {
    const auto span = identity.resolve(anchor);
    if (!span || span->end > source.text().size())
        throw std::runtime_error("replay source location unavailable");
    const auto start = source.position(span->begin);
    const auto finish = source.position(span->end);
    out << "{\"begin\":" << span->begin << ",\"end\":" << span->end
        << ",\"line\":" << start.line << ",\"column\":" << start.column
        << ",\"end_line\":" << finish.line
        << ",\"end_column\":" << finish.column << '}';
}
void work(std::ostream& out, const ScalarReplayExecutionStats& stats) {
    out << "{\"nodes\":" << stats.nodes_evaluated
        << ",\"fuel\":" << stats.fuel_consumed
        << ",\"detail_records\":" << stats.detail_records
        << ",\"value_bytes\":" << stats.value_bytes
        << ",\"output_bytes\":" << stats.output_bytes << '}';
}
std::string_view operation(ScalarReplayOp op) {
    constexpr std::array names{
        "invalid",     "input",        "constant",  "negate",   "bit-not",
        "not",         "add",          "subtract",  "multiply", "divide",
        "remainder",   "bit-and",      "bit-or",    "bit-xor",  "shift-left",
        "shift-right", "equal",        "not-equal", "less",     "less-equal",
        "greater",     "greater-equal"};
    const auto index = static_cast<std::size_t>(op);
    if (index >= names.size())
        throw std::runtime_error("invalid replay operation");
    return names[index];
}
std::string_view reason(ScalarReplayExecutionErrorCode code) {
    using enum ScalarReplayExecutionErrorCode;
    switch (code) {
    case InvalidDescriptor:
        return "invalid-descriptor";
    case InvalidRecordedValue:
        return "invalid-recorded-value";
    case InvalidSelection:
        return "invalid-selection";
    case FuelExhausted:
        return "fuel-exhausted";
    case ValueMemoryLimit:
        return "value-memory-limit";
    case OutputLimit:
        return "output-limit";
    case Cancelled:
        return "cancelled";
    case ArithmeticFailure:
        return "arithmetic-failure";
    case ResultMismatch:
        return "result-mismatch";
    case OutOfMemory:
        return "out-of-memory";
    }
    return "invalid-execution-state";
}
std::string_view reason(ScalarReplayErrorCode code) {
    switch (code) {
    case ScalarReplayErrorCode::MissingDescriptor:
        return "missing-descriptor";
    case ScalarReplayErrorCode::UnsupportedSource:
        return "unsupported-source";
    case ScalarReplayErrorCode::InvalidDescriptor:
        return "invalid-evidence";
    case ScalarReplayErrorCode::StorageFailure:
        return "storage-failure";
    }
    return "invalid-evidence-state";
}
std::string_view reason(IntegerError code) {
    switch (code) {
    case IntegerError::InvalidWidth:
        return "invalid-width";
    case IntegerError::TypeMismatch:
        return "type-mismatch";
    case IntegerError::DivideByZero:
        return "divide-by-zero";
    case IntegerError::InvalidShift:
        return "invalid-shift";
    case IntegerError::SignedOverflow:
        return "signed-overflow";
    case IntegerError::InvalidLiteral:
        return "invalid-literal";
    }
    return "invalid-integer-state";
}
std::string_view reason(const ScalarReplayError& error) {
    if (error.storage) {
        using enum cache::StoreErrorCode;
        switch (error.storage->code) {
        case MissingObject:
            return "missing-object";
        case CorruptManifest:
        case CorruptObject:
            return "corrupt-object";
        case IncompatibleVersion:
            return "incompatible-evidence-version";
        case KeyMismatch:
        case InputMismatch:
            return "binding-mismatch";
        case LimitExceeded:
            return "evidence-limit";
        case LockBusy:
            return "storage-busy";
        case OutOfMemory:
            return "out-of-memory";
        default:
            break;
        }
    }
    if (error.descriptor) {
        if (error.descriptor->code == ReplayErrorCode::IncompatibleVersion)
            return "incompatible-descriptor";
        if (error.descriptor->code == ReplayErrorCode::ResultMismatch)
            return "invalid-recorded-result";
    }
    return reason(error.code);
}
void envelope(std::ostream& out, std::string_view status,
              std::string_view type = "scalar-replay") {
    out << "{\"schema\":1,\"type\":";
    quote(out, type);
    out << ",\"status\":";
    quote(out, status);
    out << ",\"stage_runs\":0,\"continuation_available\":false";
}
int emit(Buffer& buffer, const Cancellation& cancellation, int code) {
    if (cancellation.requested() && code != 130) {
        std::cerr << "replay cancelled before output\n";
        return 130;
    }
    std::cout << buffer.bytes;
    std::cout.flush();
    if (!std::cout)
        throw std::runtime_error("replay output failed");
    return code;
}
int unavailable(std::string_view cause, const ScalarReplayOptions& options,
                const Cancellation& cancellation,
                ScalarReplayExecutionStats stats = {},
                std::optional<std::uint32_t> node = {},
                std::optional<IntegerError> integer = {}) {
    if (cancellation.requested())
        cause = "cancelled";
    Buffer buffer(options.render_bytes);
    std::ostream out(&buffer);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    envelope(out, "unavailable");
    out << ",\"reason\":";
    quote(out, cause);
    out << ",\"restart_required\":true,\"work\":";
    work(out, stats);
    out << ",\"node\":";
    if (node)
        out << *node;
    else
        out << "null";
    out << ",\"integer_error\":";
    if (integer)
        quote(out, reason(*integer));
    else
        out << "null";
    out << "}\n";
    return emit(buffer, cancellation, cause == "cancelled" ? 130 : 3);
}
void work(std::ostream& out, const GeneratorReplayExecutionStats& stats) {
    out << "{\"bound_nodes\":" << stats.bound_nodes_evaluated
        << ",\"element_nodes\":" << stats.element_nodes_evaluated
        << ",\"fuel\":" << stats.fuel_consumed
        << ",\"detail_records\":" << stats.detail_records
        << ",\"value_bytes\":" << stats.value_bytes
        << ",\"output_bytes\":" << stats.output_bytes << '}';
}
std::string_view reason(GeneratorReplayExecutionErrorCode code) {
    using enum GeneratorReplayExecutionErrorCode;
    switch (code) {
    case InvalidRecord:
        return "invalid-record";
    case InvalidSelection:
        return "invalid-selection";
    case FuelExhausted:
        return "fuel-exhausted";
    case ValueMemoryLimit:
        return "value-memory-limit";
    case OutputLimit:
        return "output-limit";
    case Cancelled:
        return "cancelled";
    case ArithmeticFailure:
        return "arithmetic-failure";
    case BoundsMismatch:
        return "bounds-mismatch";
    case ResultMismatch:
        return "result-mismatch";
    case OutOfMemory:
        return "out-of-memory";
    }
    return "invalid-execution-state";
}
std::string_view phase_name(GeneratorReplayPhase phase) {
    switch (phase) {
    case GeneratorReplayPhase::Admission:
        return "admission";
    case GeneratorReplayPhase::Begin:
        return "begin";
    case GeneratorReplayPhase::End:
        return "end";
    case GeneratorReplayPhase::Element:
        return "element";
    }
    return "invalid-phase";
}
int generator_unavailable(
    std::string_view cause, const ScalarReplayOptions& options,
    const Cancellation& cancellation, GeneratorReplayExecutionStats stats = {},
    GeneratorReplayPhase phase = GeneratorReplayPhase::Admission,
    std::optional<std::uint32_t> node = {},
    std::optional<IntegerError> integer = {}) {
    if (cancellation.requested())
        cause = "cancelled";
    Buffer buffer(options.render_bytes);
    std::ostream out(&buffer);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    envelope(out, "unavailable", "generator-replay");
    out << ",\"reason\":";
    quote(out, cause);
    out << ",\"restart_required\":true,\"element_index\":" << *options.element
        << ",\"phase\":";
    quote(out, phase_name(phase));
    out << ",\"work\":";
    work(out, stats);
    out << ",\"node\":";
    if (node)
        out << *node;
    else
        out << "null";
    out << ",\"integer_error\":";
    if (integer)
        quote(out, reason(*integer));
    else
        out << "null";
    out << "}\n";
    return emit(buffer, cancellation, cause == "cancelled" ? 130 : 3);
}
int run_generator(const syntax::Module& module, const sema::TypeTable& types,
                  const CacheIdentity& identity, const cache::QueryKey& key,
                  const source::SourceFile& source,
                  const ScalarReplayOptions& options,
                  const Cancellation& cancellation) {
    const auto record = load_generator_replay(
        cache::FileStore(options.directory), module, types, key);
    if (!record)
        return generator_unavailable(reason(record.error()), options,
                                     cancellation);
    const auto result =
        execute_generator_replay(*record, *options.element, options.limits,
                                 options.range, cancellation.token());
    if (!result)
        return generator_unavailable(reason(result.error().code), options,
                                     cancellation, result.error().stats,
                                     result.error().phase, result.error().node,
                                     result.error().integer);
    if (cancellation.requested())
        return generator_unavailable("cancelled", options, cancellation,
                                     result->stats,
                                     GeneratorReplayPhase::Element);
    const auto fingerprint = cache::query_fingerprint(key);
    if (!fingerprint)
        throw std::runtime_error(
            "replay specialization fingerprint unavailable");
    const auto& descriptor = record->admission.descriptor();
    Buffer buffer(options.render_bytes);
    std::ostream out(&buffer);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    envelope(out, "verified", "generator-replay");
    out << ",\"restart_required\":false,\"verification_scope\":\"selected-"
           "element\""
        << ",\"execution_version\":" << generator_replay_execution_version
        << ",\"evidence_version\":" << generator_replay_evidence_version
        << ",\"descriptor_version\":" << descriptor.version
        << ",\"generator_semantics\":" << descriptor.semantics
        << ",\"recipe_semantics\":" << descriptor.scalar_semantics
        << ",\"specialization\":{\"name\":";
    quote(out, options.function);
    out << ",\"key\":";
    quote(out, fingerprint->hex());
    out << ",\"arguments\":[";
    for (std::size_t i = 0; i < key.arguments.size(); ++i) {
        if (i)
            out << ',';
        value(out, key.arguments[i].type, *key.arguments[i].value);
    }
    out << "]},\"descriptor\":";
    quote(out, result->descriptor.hex());
    out << ",\"recorded_result\":";
    quote(out, result->recorded_result.hex());
    out << ",\"source\":{\"path\":";
    quote(out, source.path());
    out << ",\"revision\":";
    quote(out, cache::fingerprint(
                   "pagos.generator-replay.source.v1",
                   {reinterpret_cast<const std::uint8_t*>(source.text().data()),
                    source.text().size()})
                   .hex());
    out << "},\"element_index\":" << result->element_index
        << ",\"iteration_value\":" << result->iteration_value
        << ",\"iterator_slot\":" << descriptor.iterator_slot
        << ",\"bounds\":{\"first\":" << record->observation.first
        << ",\"last\":" << record->observation.last
        << ",\"length\":" << descriptor.result_type.length << "},\"result\":";
    value(out, result->value.type, result->value.value);
    out << ",\"site\":";
    site(out, identity, descriptor.generator_site, source);
    out << ",\"iterator_site\":";
    site(out, identity, descriptor.iterator_site, source);
    out << ",\"element_site\":";
    site(out, identity, descriptor.element.result_site, source);
    out << ",\"selection\":{\"first\":" << options.range.first
        << ",\"count\":" << result->details.size()
        << ",\"total\":" << descriptor.element.nodes.size() << ",\"complete\":"
        << (options.range.first == 0 &&
                    result->details.size() == descriptor.element.nodes.size()
                ? "true"
                : "false")
        << "},\"work\":";
    work(out, result->stats);
    out << ",\"details\":[";
    bool first = true;
    for (const auto& step : result->details) {
        if (cancellation.requested())
            return generator_unavailable("cancelled", options, cancellation,
                                         result->stats,
                                         GeneratorReplayPhase::Element);
        if (!first)
            out << ',';
        first = false;
        const auto& node = descriptor.element.nodes[step.node];
        out << "{\"node\":" << step.node << ",\"operation\":";
        quote(out, operation(node.operation));
        out << ",\"value\":";
        value(out, node.type, from_scalar(step.value));
        auto anchor = descriptor.element.result_site;
        anchor.position = node.position;
        out << ",\"site\":";
        site(out, identity, anchor, source);
        out << ",\"operands\":[";
        for (std::size_t i = 0; i < node.operands.size(); ++i) {
            if (i)
                out << ',';
            out << node.operands[i];
        }
        out << "],\"input_slot\":";
        if (node.input)
            out << *node.input;
        else
            out << "null";
        out << ",\"input_kind\":";
        if (node.input)
            quote(out, *node.input == descriptor.iterator_slot ? "iterator"
                                                               : "capture");
        else
            out << "null";
        out << '}';
    }
    out << "]}\n";
    if (options.statistics)
        std::cerr << "generator-replay: bound-nodes="
                  << result->stats.bound_nodes_evaluated
                  << " element-nodes=" << result->stats.element_nodes_evaluated
                  << " fuel=" << result->stats.fuel_consumed
                  << " rendered-bytes=" << buffer.bytes.size() << '\n';
    return emit(buffer, cancellation, 0);
}
} // namespace
std::expected<void, std::string>
parse_scalar_replay_option(std::string_view argument,
                           ScalarReplayOptions& options) {
    options.specified = true;
    if (argument == "--replay-stats") {
        options.statistics = true;
        return {};
    }
    const auto split = argument.find('=');
    if (split == std::string_view::npos || split + 1 == argument.size())
        return std::unexpected("replay option requires a non-empty value");
    const auto name = argument.substr(0, split),
               text = argument.substr(split + 1);
    if (name == "--replay-read")
        options.directory = text;
    else if (name == "--replay-function")
        options.function = text;
    else if (name == "--replay-arg")
        options.arguments.emplace_back(text);
    else {
        std::size_t number{};
        if (name == "--replay-count" && text == "all") {
            options.range.count.reset();
            return {};
        }
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            return std::unexpected("invalid replay numeric value");
        if (name == "--replay-fuel")
            options.limits.fuel = number;
        else if (name == "--replay-element") {
            if (number > UINT32_MAX)
                return std::unexpected("replay element index exceeds u32");
            options.element = static_cast<std::uint32_t>(number);
        } else if (name == "--replay-value-bytes")
            options.limits.value_bytes = number;
        else if (name == "--replay-output-records")
            options.limits.output_records = number;
        else if (name == "--replay-detail-bytes")
            options.limits.output_bytes = number;
        else if (name == "--replay-render-bytes")
            options.render_bytes = number;
        else if (name == "--replay-first")
            options.range.first = number;
        else if (name == "--replay-count")
            options.range.count = number;
        else
            return std::unexpected("unknown replay option");
    }
    return {};
}
int run_scalar_replay(const syntax::Module& module,
                      const sema::TypeTable& types,
                      const cache::QueryContext& context,
                      const source::SourceFile& source,
                      const ScalarReplayOptions& options) {
    try {
        Cancellation cancellation;
        if (options.statistics)
            std::cerr << (options.element ? "generator-replay"
                                          : "scalar-replay")
                      << ": stage-runs=0 ready\n";
        const auto report = [&](std::string_view cause) {
            return options.element
                       ? generator_unavailable(cause, options, cancellation)
                       : unavailable(cause, options, cancellation);
        };
        auto identity = build_cache_identity(module, types, context);
        if (!identity)
            return report("source-identity-unavailable");
        if (cancellation.requested())
            return report("cancelled");
        const auto* definition =
            identity->find(cache::DefinitionKind::Function, options.function);
        if (!definition)
            return report("function-not-found");
        if (definition->result.is_array() != options.element.has_value()) {
            std::cerr << "error: "
                      << (definition->result.is_array()
                              ? "array replay requires --replay-element"
                              : "--replay-element requires an array function")
                      << '\n';
            return 2;
        }
        if (definition->parameters.size() != options.arguments.size()) {
            std::cerr << "error: replay requires exactly "
                      << definition->parameters.size() << " scalar arguments\n";
            return 2;
        }
        std::vector<cache::StageArgument> arguments;
        for (std::size_t i = 0; i < definition->parameters.size(); ++i) {
            const auto& type = definition->parameters[i];
            const auto& text = options.arguments[i];
            if (type.kind != sema::TypeKind::Integer &&
                type.kind != sema::TypeKind::Bool)
                return report("unsupported-source");
            Constant constant;
            if (type.kind == sema::TypeKind::Bool) {
                if (text != "true" && text != "false") {
                    std::cerr << "error: replay bool arguments must be true or "
                                 "false\n";
                    return 2;
                }
                constant = text == "true";
            } else {
                const auto integer =
                    IntegerValue::parse_decimal(type.integer_type, text);
                if (!integer) {
                    std::cerr << "error: replay argument is not a valid "
                              << sema::type_name(type) << " decimal value\n";
                    return 2;
                }
                constant = *integer;
            }
            arguments.push_back(
                {type, cache::ArgumentStage::Comptime, std::move(constant)});
        }
        const auto key =
            make_query_key(*definition, context, std::move(arguments));
        if (!key)
            return report("unsupported-source");
        if (options.element)
            return run_generator(module, types, *identity, *key, source,
                                 options, cancellation);
        const auto record = load_scalar_replay(
            cache::FileStore(options.directory), module, types, *key);
        if (!record)
            return unavailable(reason(record.error()), options, cancellation);
        auto result = execute_scalar_replay(
            *record, options.limits, options.range, cancellation.token());
        if (!result)
            return unavailable(reason(result.error().code), options,
                               cancellation, result.error().stats,
                               result.error().node, result.error().integer);
        if (cancellation.requested())
            return unavailable("cancelled", options, cancellation,
                               result->stats);
        const auto fingerprint = cache::query_fingerprint(*key);
        if (!fingerprint)
            throw std::runtime_error(
                "replay specialization fingerprint unavailable");
        Buffer buffer(options.render_bytes);
        std::ostream out(&buffer);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        envelope(out, "verified");
        out << ",\"restart_required\":false,\"execution_version\":"
            << scalar_replay_execution_version
            << ",\"evidence_version\":" << scalar_replay_evidence_version
            << ",\"recipe_semantics\":" << scalar_recipe_semantics_version
            << ",\"specialization\":{\"name\":";
        quote(out, options.function);
        out << ",\"key\":";
        quote(out, fingerprint->hex());
        out << ",\"arguments\":[";
        for (std::size_t i = 0; i < key->arguments.size(); ++i) {
            if (i)
                out << ',';
            value(out, key->arguments[i].type, *key->arguments[i].value);
        }
        out << "]},\"descriptor\":";
        quote(out, result->descriptor.hex());
        out << ",\"source\":{\"path\":";
        quote(out, source.path());
        out << ",\"revision\":";
        quote(out, cache::fingerprint("pagos.scalar-replay.source.v1",
                                      {reinterpret_cast<const std::uint8_t*>(
                                           source.text().data()),
                                       source.text().size()})
                       .hex());
        out << '}';
        out << ",\"result\":";
        value(out, result->value.type, result->value.value);
        out << ",\"site\":";
        site(out, *identity, record->descriptor.result_site, source);
        out << ",\"selection\":{\"first\":" << options.range.first
            << ",\"count\":" << result->details.size()
            << ",\"total\":" << record->descriptor.nodes.size()
            << ",\"complete\":"
            << (options.range.first == 0 && result->details.size() ==
                                                record->descriptor.nodes.size()
                    ? "true"
                    : "false")
            << "},\"work\":";
        work(out, result->stats);
        out << ",\"details\":[";
        bool first = true;
        for (const auto& step : result->details) {
            if (cancellation.requested())
                return unavailable("cancelled", options, cancellation,
                                   result->stats);
            if (!first)
                out << ',';
            first = false;
            const auto& node = record->descriptor.nodes[step.node];
            out << "{\"node\":" << step.node << ",\"operation\":";
            quote(out, operation(node.operation));
            out << ",\"value\":";
            value(out, node.type, from_scalar(step.value));
            auto anchor = record->descriptor.result_site;
            anchor.position = node.position;
            out << ",\"site\":";
            site(out, *identity, anchor, source);
            out << ",\"operands\":[";
            for (std::size_t i = 0; i < node.operands.size(); ++i) {
                if (i)
                    out << ',';
                out << node.operands[i];
            }
            out << "],\"input_slot\":";
            if (node.input)
                out << *node.input;
            else
                out << "null";
            out << '}';
        }
        out << "]}\n";
        if (options.statistics)
            std::cerr << "scalar-replay: nodes="
                      << result->stats.nodes_evaluated
                      << " fuel=" << result->stats.fuel_consumed
                      << " rendered-bytes=" << buffer.bytes.size() << '\n';
        return emit(buffer, cancellation, 0);
    } catch (const RenderLimit&) {
        std::cerr << "error: " << (options.element ? "generator" : "scalar")
                  << " replay render byte limit exceeded\n";
        return interrupted.test(std::memory_order_relaxed) ? 130 : 3;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return interrupted.test(std::memory_order_relaxed) ? 130 : 1;
    }
}
} // namespace pagos::driver
