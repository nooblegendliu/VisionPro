#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace visionpro::core;

template <class Left, class Right>
concept Comparable = requires(const Left& left, const Right& right) { left == right; };

template <class Left, class... Right>
consteval bool isolated_from_all()
{
    return ((std::same_as<Left, Right>
        || (!std::is_constructible_v<Left, Right> && !std::is_convertible_v<Left, Right>
            && !std::is_assignable_v<Left&, Right> && !Comparable<Left, Right>)) && ...);
}

template <class... Id>
consteval bool isolated_set()
{
    return (isolated_from_all<Id, Id...>() && ...);
}

static_assert(isolated_set<SolutionId, WorkflowId, NodeId, ResourceId, VariableId, QueueId,
    ControlLinkId, RegionId, SolutionSessionId, SchemaId, ToolTypeId, InputId, OutputId,
    ParameterId, FieldId, ControlOutputId, EnumMemberId, CapabilityId, CorrelationDomainId, CoordinateSystemId,
    ConfigSnapshotId, WorkflowRunId, ExecutionScopeId, InvocationId, AttemptId, FrameId, CorrelationId>());
static_assert(PersistentId<ResourceId> && PersistentId<VariableId> && PersistentId<QueueId>
    && PersistentId<ControlLinkId> && PersistentId<RegionId>);
static_assert(UuidIdentity<SolutionSessionId> && !PersistentId<SolutionSessionId>);
static_assert(!UuidIdentity<WorkflowRunId> && !UuidIdentity<SchemaId>);

template <SchemaIdentity Id>
constexpr bool schema_interface = !std::is_default_constructible_v<Id>
    && !std::is_constructible_v<Id, std::string> && !std::is_constructible_v<Id, std::string_view>
    && !std::is_convertible_v<Id, std::string_view>;
static_assert(schema_interface<SchemaId> && schema_interface<ToolTypeId>
    && schema_interface<InputId> && schema_interface<OutputId> && schema_interface<ParameterId>
    && schema_interface<FieldId> && schema_interface<ControlOutputId> && schema_interface<EnumMemberId>
    && schema_interface<CapabilityId> && schema_interface<CorrelationDomainId>
    && schema_interface<CoordinateSystemId>);

template <RuntimeIdentity Id>
constexpr bool runtime_interface = !std::is_default_constructible_v<Id>
    && !std::is_constructible_v<Id, std::uint64_t> && !std::is_convertible_v<Id, std::uint64_t>
    && !std::is_copy_constructible_v<IdSequence<Id>> && !std::is_move_constructible_v<IdSequence<Id>>
    && !std::is_copy_assignable_v<IdSequence<Id>> && !std::is_move_assignable_v<IdSequence<Id>>;
static_assert(runtime_interface<ConfigSnapshotId> && runtime_interface<WorkflowRunId>
    && runtime_interface<ExecutionScopeId> && runtime_interface<InvocationId> && runtime_interface<AttemptId>);
static_assert(!std::is_default_constructible_v<CorrelationId> && !std::is_default_constructible_v<FrameId>);

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view description)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

template <class T>
T value(Result<T> result)
{
    if (const auto* item = std::get_if<T>(&result)) {
        return *item;
    }
    const auto& diagnostic = std::get<Diagnostic>(result);
    throw std::runtime_error("Expected value: " + diagnostic.message);
}

template <class T>
void error(const Result<T>& result, ErrorCode expected)
{
    const auto* diagnostic = std::get_if<Diagnostic>(&result);
    check(diagnostic != nullptr, "invalid identity produces a diagnostic alternative");
    if (diagnostic != nullptr) {
        check(diagnostic->code == expected, "identity error preserves its stable error code");
        check(diagnostic->severity == Severity::Error, "identity failure has error severity");
        check(!diagnostic->message.empty(), "identity failure has a readable explanation");
    }
}

constexpr std::string_view uuid_one = "00112233-4455-4677-8899-aabbccddeeff";
constexpr std::string_view uuid_two = "10112233-4455-4677-8899-aabbccddeeff";

template <UuidIdentity Id>
void check_uuid()
{
    const auto parsed = value(IdFactory::parse<Id>(uuid_one));
    check(parsed.to_string() == uuid_one, "new UUID identity preserves canonical representation");
    error(IdFactory::parse<Id>("invalid"), ErrorCode::InvalidIdFormat);
    std::set<Id> generated;
    for (int sample = 0; sample < 16; ++sample) {
        const auto id = value(IdFactory::generate<Id>());
        const auto text = id.to_string();
        check(text.size() == 36 && text[14] == '4'
            && std::string_view("89ab").find(text[19]) != std::string_view::npos,
            "new UUID identity generation retains version and variant bits");
        check(value(IdFactory::parse<Id>(text)) == id, "generated new UUID identity roundtrips");
        check(generated.insert(id).second, "new UUID sample does not repeat within this test");
    }
}

template <SchemaIdentity Id>
void check_schema()
{
    const auto id = value(SchemaIdFactory::parse<Id>("visionpro.measure-radius_2"));
    check(id.text() == "visionpro.measure-radius_2", "schema key is preserved without normalization");
    check(value(SchemaIdFactory::parse<Id>("A")) != value(SchemaIdFactory::parse<Id>("a")),
        "schema identities are case-sensitive");
    check(value(SchemaIdFactory::parse<Id>("_")).text() == "_", "underscore is a valid first character");
    check(value(SchemaIdFactory::parse<Id>(std::string(128, 'a'))).text().size() == 128,
        "maximum schema key length is accepted");
    auto original = std::string("source.output");
    const auto independent = value(SchemaIdFactory::parse<Id>(original));
    original.assign(13, 'x');
    check(independent.text() == "source.output", "schema key owns its source text");
    auto moved_source = id;
    auto moved = std::move(moved_source);
    check(moved == id && moved_source == id, "moving a schema key leaves both identities valid");
    moved = std::move(moved_source);
    check(moved == id && moved_source == id, "move assignment preserves validated identities");

    constexpr std::array invalid{
        std::string_view{}, std::string_view("1name"), std::string_view(".name"),
        std::string_view("-name"), std::string_view(" name"), std::string_view("name "),
        std::string_view("name/key"), std::string_view("name:key"), std::string_view("name\nkey"),
        std::string_view("\xc3\xa9")};
    for (const auto text : invalid) {
        error(SchemaIdFactory::parse<Id>(text), ErrorCode::InvalidSchemaId);
    }
    error(SchemaIdFactory::parse<Id>(std::string(129, 'a')), ErrorCode::InvalidSchemaId);
    error(SchemaIdFactory::parse<Id>(std::string("name\0key", 8)), ErrorCode::InvalidSchemaId);
    const std::string bounded = "prefixkey.trailing";
    check(value(SchemaIdFactory::parse<Id>(std::string_view(bounded).substr(6, 3))).text() == "key",
        "schema parser respects the provided view boundary");
}

template <RuntimeIdentity Id>
void check_runtime()
{
    constexpr auto minimum = minimum_runtime_id<Id>;
    const auto first = value(RuntimeIdFactory::from_value<Id>(minimum));
    check(first.value() == minimum, "runtime identity starts at its declared minimum");
    check(value(RuntimeIdFactory::next(first)).value() == minimum + 1, "checked next advances the same identity kind");
    if constexpr (minimum != 0) {
        error(RuntimeIdFactory::from_value<Id>(0), ErrorCode::InvalidRuntimeId);
    }
    const auto maximum = value(RuntimeIdFactory::from_value<Id>(std::numeric_limits<std::uint64_t>::max()));
    error(RuntimeIdFactory::next(maximum), ErrorCode::IdentityExhausted);
    IdSequence<Id> sequence;
    check(value(sequence.next()) == first, "sequence starts at the declared minimum");
    check(value(sequence.next()).value() == minimum + 1, "sequence advances instead of reusing identities");
    IdSequence<Id> last_sequence(maximum);
    check(value(last_sequence.next()) == maximum, "last representable identity is issued once");
    error(last_sequence.next(), ErrorCode::IdentityExhausted);
    error(last_sequence.next(), ErrorCode::IdentityExhausted);
    const auto penultimate = value(RuntimeIdFactory::from_value<Id>(maximum.value() - 1));
    IdSequence<Id> final_two(penultimate);
    check(value(final_two.next()) == penultimate, "sequence can resume at a typed next identity");
    check(value(final_two.next()) == maximum, "sequence does not skip the terminal identity");
    error(final_two.next(), ErrorCode::IdentityExhausted);
}

void compound_identities()
{
    const auto node = value(IdFactory::parse<NodeId>(uuid_one));
    const auto other_node = value(IdFactory::parse<NodeId>(uuid_two));
    const auto run = value(RuntimeIdFactory::from_value<WorkflowRunId>(1));
    const auto root = value(RuntimeIdFactory::from_value<ExecutionScopeId>(0));
    const auto call = value(RuntimeIdFactory::from_value<InvocationId>(1));
    const InvocationKey invocation{run, node, root, call};
    check(invocation != InvocationKey{value(RuntimeIdFactory::next(run)), node, root, call}, "run participates in invocation identity");
    check(invocation != InvocationKey{run, other_node, root, call}, "node participates in invocation identity");
    check(invocation != InvocationKey{run, node, value(RuntimeIdFactory::next(root)), call}, "scope participates in invocation identity");
    check(invocation != InvocationKey{run, node, root, value(RuntimeIdFactory::next(call))}, "logical call participates in invocation identity");
    const auto attempt = value(RuntimeIdFactory::from_value<AttemptId>(0));
    const auto retry = value(RuntimeIdFactory::next(attempt));
    check(retry != attempt && invocation == InvocationKey{run, node, root, call},
        "attempt identity changes independently of the logical invocation key");
    const auto session = value(IdFactory::parse<SolutionSessionId>(uuid_one));
    const auto other_session = value(IdFactory::parse<SolutionSessionId>(uuid_two));
    check(SessionInvocationKey{session, invocation} != SessionInvocationKey{other_session, invocation},
        "cross-session invocation identity includes the session");

    const auto source = value(IdFactory::parse<ResourceId>(uuid_one));
    const auto other_source = value(IdFactory::parse<ResourceId>(uuid_two));
    check(FrameId{source, 0} != FrameId{other_source, 0}, "equal frame counters from different sources do not match");
    check(FrameId{source, 0} != FrameId{source, 1}, "frame sequence participates in frame identity");
    check(FrameId{source, 0} == FrameId{source, 0}, "frame identity permits a source-local zero counter");
}

void correlation_identities()
{
    const auto domain = value(SchemaIdFactory::parse<CorrelationDomainId>("station.line1"));
    const auto other_domain = value(SchemaIdFactory::parse<CorrelationDomainId>("station.line2"));
    const auto original = value(RuntimeIdFactory::correlation(domain, "workpiece-42"));
    check(original != value(RuntimeIdFactory::correlation(other_domain, "workpiece-42")),
        "same business text from different declared domains does not match");
    check(original != value(RuntimeIdFactory::correlation(domain, "workpiece-43")), "business key participates in correlation identity");
    check(original == value(RuntimeIdFactory::correlation(domain, "workpiece-42")), "identical domain and key correlate");
    check(original.domain() == domain && original.key() == "workpiece-42", "correlation identity exposes its declared domain and key");
    check(value(RuntimeIdFactory::correlation(domain, "工件-42")).key() == "工件-42", "business keys support valid UTF-8");
    check(value(RuntimeIdFactory::correlation(domain, std::string(256, 'x'))).key().size() == 256, "maximum correlation key size is accepted");
    std::string source = "workpiece-42";
    const auto owned = value(RuntimeIdFactory::correlation(domain, source));
    source.clear();
    check(owned == original, "correlation owns its input text independently");
    auto moved_source = original;
    auto moved = std::move(moved_source);
    check(moved == original && moved_source == original, "moving correlations preserves valid identities");
    moved = std::move(moved_source);
    check(moved == original && moved_source == original, "correlation move assignment preserves valid identities");
    error(RuntimeIdFactory::correlation(domain, ""), ErrorCode::InvalidCorrelationId);
    error(RuntimeIdFactory::correlation(domain, std::string(257, 'x')), ErrorCode::InvalidCorrelationId);
    error(RuntimeIdFactory::correlation(domain, std::string("work\0piece", 10)), ErrorCode::InvalidCorrelationId);
    constexpr std::array invalid_utf8{std::string_view("\x80"), std::string_view("\xc0\xaf"),
        std::string_view("\xe2\x82"), std::string_view("\xed\xa0\x80"), std::string_view("\xf4\x90\x80\x80")};
    for (const auto text : invalid_utf8) {
        error(RuntimeIdFactory::correlation(domain, text), ErrorCode::InvalidCorrelationId);
    }
}

} // namespace

int main()
{
    try {
        check_uuid<ResourceId>();
        check_uuid<VariableId>();
        check_uuid<QueueId>();
        check_uuid<ControlLinkId>();
        check_uuid<RegionId>();
        check_uuid<SolutionSessionId>();
        check_schema<SchemaId>();
        check_schema<ToolTypeId>();
        check_schema<InputId>();
        check_schema<OutputId>();
        check_schema<ParameterId>();
        check_schema<FieldId>();
        check_schema<ControlOutputId>();
        check_schema<EnumMemberId>();
        check_schema<CapabilityId>();
        check_schema<CorrelationDomainId>();
        check_schema<CoordinateSystemId>();
        check_runtime<ConfigSnapshotId>();
        check_runtime<WorkflowRunId>();
        check_runtime<ExecutionScopeId>();
        check_runtime<InvocationId>();
        check_runtime<AttemptId>();
        compound_identities();
        correlation_identities();
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected exception: " << exception.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Unexpected non-standard exception\n";
        return 1;
    }
    std::cout << "identities: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
