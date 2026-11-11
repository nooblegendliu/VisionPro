#include <visionpro/core/id_factory.hpp>

#include <array>
#include <compare>
#include <concepts>
#include <cstdint>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace visionpro::core;

template <typename Left, typename Right>
concept EqualityComparable = requires(const Left& left, const Right& right) {
    { left == right } -> std::convertible_to<bool>;
};

template <typename Left, typename Right>
concept Ordered = requires(const Left& left, const Right& right) {
    left <=> right;
};

template <typename Left, typename Right>
constexpr bool isolated_ids = !std::is_constructible_v<Left, Right>
    && !std::is_convertible_v<Right, Left>
    && !std::is_assignable_v<Left&, Right>
    && !EqualityComparable<Left, Right> && !Ordered<Left, Right>;

static_assert(isolated_ids<SolutionId, WorkflowId>);
static_assert(isolated_ids<SolutionId, NodeId>);
static_assert(isolated_ids<WorkflowId, SolutionId>);
static_assert(isolated_ids<WorkflowId, NodeId>);
static_assert(isolated_ids<NodeId, SolutionId>);
static_assert(isolated_ids<NodeId, WorkflowId>);

template <typename Id>
constexpr bool valid_id_interface = !std::is_default_constructible_v<Id>
    && !std::is_constructible_v<Id, std::string_view>
    && !std::is_constructible_v<Id, std::array<std::uint8_t, 16>>
    && !std::is_convertible_v<Id, std::string>
    && std::is_copy_constructible_v<Id> && std::is_copy_assignable_v<Id>
    && std::is_move_constructible_v<Id> && std::is_move_assignable_v<Id>
    && EqualityComparable<Id, Id> && Ordered<Id, Id>;

static_assert(valid_id_interface<SolutionId>);
static_assert(valid_id_interface<WorkflowId>);
static_assert(valid_id_interface<NodeId>);

constexpr std::string_view first_id = "00112233-4455-4677-8899-aabbccddeeff";
constexpr std::string_view second_id = "10112233-4455-4677-8899-aabbccddeeff";

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

template <typename Id>
Id require_id(std::string_view text)
{
    const auto parsed = IdFactory::parse<Id>(text);
    const auto* id = std::get_if<Id>(&parsed);
    check(id != nullptr, "valid fixture must parse");
    if (id == nullptr) {
        throw std::runtime_error("Cannot continue with an invalid ID fixture");
    }
    return *id;
}

template <typename Id>
void check_valid_ids()
{
    const auto lower = require_id<Id>(first_id);
    check(lower.to_string() == first_id, "known UUID preserves byte and text order");
    const auto upper = require_id<Id>("00112233-4455-4677-8899-AABBCCDDEEFF");
    check(upper == lower, "uppercase hex parses to the same identity");
    check(upper.to_string() == first_id, "uppercase input normalizes to lowercase");
    const auto mixed = require_id<Id>("00112233-4455-4677-8899-aAbBcCdDeEfF");
    check(mixed == lower, "mixed-case hex parses to the same identity");
    const auto surrounding_text = std::string("prefix") + std::string(first_id) + "suffix";
    const std::string_view bounded_input(surrounding_text.data() + 6, first_id.size());
    check(require_id<Id>(bounded_input) == lower, "parsing respects a non-null-terminated string view");

    for (const char variant : std::string_view("89abAB")) {
        auto input = std::string(first_id);
        input[19] = variant;
        const auto id = require_id<Id>(input);
        if (variant == 'A' || variant == 'B') {
            input[19] = static_cast<char>(variant - 'A' + 'a');
        }
        check(id.to_string() == input, "all UUIDv4 variant encodings normalize correctly");
        check(require_id<Id>(id.to_string()) == id, "text roundtrip preserves identity");
    }

    check(require_id<Id>("00000000-0000-4000-8000-000000000000").to_string()
              == "00000000-0000-4000-8000-000000000000",
          "zero payload with version and variant bits remains valid");
    check(require_id<Id>("ffffffff-ffff-4fff-bfff-ffffffffffff").to_string()
              == "ffffffff-ffff-4fff-bfff-ffffffffffff",
          "maximum payload is valid");
}

template <typename Id>
void check_invalid_id(std::string_view input, ErrorCode expected)
{
    const auto result = IdFactory::parse<Id>(input);
    const auto* error = std::get_if<Diagnostic>(&result);
    check(error != nullptr, "invalid input returns a diagnostic, not a value");
    if (error != nullptr) {
        check(error->code == expected, "invalid input retains its specific error code");
        check(error->severity == Severity::Error, "parse failure has error severity");
        check(error->category() == ErrorCategory::Structure, "parse failure is structural");
        check(!error->message.empty(), "parse diagnostic includes a readable explanation");
    }
}

void invalid_for_all_ids(std::string_view input, ErrorCode expected)
{
    check_invalid_id<SolutionId>(input, expected);
    check_invalid_id<WorkflowId>(input, expected);
    check_invalid_id<NodeId>(input, expected);
}

void parse_invalid()
{
    constexpr std::array invalid_format_inputs{
        std::string_view{},
        std::string_view("00112233-4455-4677-8899-aabbccddeef"),
        std::string_view("00112233-4455-4677-8899-aabbccddeeff0"),
        std::string_view("00112233445546778899aabbccddeeff"),
        std::string_view("{00112233-4455-4677-8899-aabbccddeeff}"),
        std::string_view(" 00112233-4455-4677-8899-aabbccddeeff"),
        std::string_view("00112233-4455-4677-8899-aabbccddeeff "),
        std::string_view("00112233-4455-4677-8899-aabbccddeeff\n"),
        std::string_view("urn:uuid:00112233-4455-4677-8899-aabbccddeeff")};
    for (const auto input : invalid_format_inputs) {
        invalid_for_all_ids(input, ErrorCode::InvalidIdFormat);
    }

    // Exercise every character position, including the four required separators.
    for (std::size_t index = 0; index < first_id.size(); ++index) {
        auto input = std::string(first_id);
        input[index] = first_id[index] == '-' ? '0' : 'g';
        invalid_for_all_ids(input, ErrorCode::InvalidIdFormat);
    }
    auto embedded_nul = std::string(first_id);
    embedded_nul[30] = '\0';
    invalid_for_all_ids(embedded_nul, ErrorCode::InvalidIdFormat);
    auto trailing_nul = std::string(first_id);
    trailing_nul.push_back('\0');
    invalid_for_all_ids(trailing_nul, ErrorCode::InvalidIdFormat);
    auto non_ascii = std::string(first_id);
    non_ascii[0] = static_cast<char>(0xff);
    invalid_for_all_ids(non_ascii, ErrorCode::InvalidIdFormat);

    for (const char version : std::string_view("012356789abcdefABCDEF")) {
        auto input = std::string(first_id);
        input[14] = version;
        invalid_for_all_ids(input, ErrorCode::InvalidIdVersion);
    }
    invalid_for_all_ids("00000000-0000-0000-0000-000000000000", ErrorCode::InvalidIdVersion);
    for (const char variant : std::string_view("01234567cdefCDEF")) {
        auto input = std::string(first_id);
        input[19] = variant;
        invalid_for_all_ids(input, ErrorCode::InvalidIdVariant);
    }

    auto multiple_errors = std::string(first_id);
    multiple_errors[14] = '1';
    multiple_errors[19] = '0';
    multiple_errors[35] = 'g';
    invalid_for_all_ids(multiple_errors, ErrorCode::InvalidIdFormat);
    multiple_errors[35] = 'f';
    invalid_for_all_ids(multiple_errors, ErrorCode::InvalidIdVersion);
}

bool canonical_v4(std::string_view text)
{
    if (text.size() != 36 || text[14] != '4'
        || std::string_view("89ab").find(text[19]) == std::string_view::npos) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') {
                return false;
            }
        } else if (!((text[index] >= '0' && text[index] <= '9')
                     || (text[index] >= 'a' && text[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

template <typename Id>
void check_generated_ids(std::set<std::string>& samples)
{
    for (int sample = 0; sample < 256; ++sample) {
        const auto result = IdFactory::generate<Id>();
        const auto* id = std::get_if<Id>(&result);
        check(id != nullptr, "UUID generation succeeds in the verification environment");
        if (id == nullptr) {
            continue;
        }
        const auto text = id->to_string();
        check(canonical_v4(text), "generated UUID has canonical version and variant bits");
        check(require_id<Id>(text) == *id, "generated UUID survives text roundtrip");
        check(samples.insert(text).second, "generated sample is not repeated in this test run");
    }
}

template <typename Id>
void check_value_semantics()
{
    const auto first = require_id<Id>(first_id);
    const auto second = require_id<Id>(second_id);
    check(first != second, "different payloads have different identities");
    check(first < second, "same-type IDs have a stable byte order");
    check((first <=> first) == std::strong_ordering::equal, "equal IDs compare equally");
    auto copy = first;
    copy = second;
    check(first.to_string() == first_id, "assigning a copy preserves the original ID");
    check(copy == second, "copy assignment preserves the assigned identity");
    auto moved = std::move(copy);
    check(moved == second, "move construction preserves the destination identity");
    moved = first;
    moved = require_id<Id>(second_id);
    check(moved == second, "move assignment preserves the destination identity");
    const std::set<Id> unique_ids{second, first, first};
    check(unique_ids.size() == 2, "ordered containers distinguish identities and collapse equal IDs");
    check(*unique_ids.begin() == first, "ordered container uses identity order");

    auto input = std::string(first_id);
    const auto independent = require_id<Id>(input);
    input.assign(input.size(), '0');
    check(independent == first, "parsed identity owns its value independently of input text");
}

void diagnostics()
{
    const auto solution = require_id<SolutionId>(first_id);
    const auto workflow = require_id<WorkflowId>(first_id);
    const auto node = require_id<NodeId>(second_id);
    Diagnostic original{ErrorCode::InvalidIdFormat, Severity::Error,
                        {solution, workflow, node}, "invalid identity at a known object"};
    const Result<int> failed = original;
    original.message = "changed explanation";
    original.location.node.reset();
    original.severity = Severity::Warning;
    const auto* stored = std::get_if<Diagnostic>(&failed);
    check(stored != nullptr, "a failure result holds the diagnostic alternative");
    if (stored != nullptr) {
        check(stored->code == ErrorCode::InvalidIdFormat, "error classification is independent of message text");
        check(stored->severity == Severity::Error, "diagnostic copy owns severity");
        check(stored->location.solution == solution, "diagnostic identifies the solution");
        check(stored->location.workflow == workflow, "diagnostic identifies the workflow");
        check(stored->location.node == node, "diagnostic retains its copied node location");
        check(stored->message == "invalid identity at a known object", "diagnostic owns its message");
    }
    check(original.category() == ErrorCategory::Structure, "message and severity changes preserve error category");
    const Diagnostic without_location{ErrorCode::IdGenerationFailed, Severity::Error, {}, "entropy unavailable"};
    check(!without_location.location.solution && !without_location.location.workflow
              && !without_location.location.node,
          "diagnostic can omit location before an identity exists");
    check(without_location.category() == ErrorCategory::Resource, "generation failure is a resource error");

    const Result<int> zero = 0;
    check(std::holds_alternative<int>(zero) && std::get<int>(zero) == 0,
          "zero is a successful value and remains distinct from an error");
    check(std::get_if<Diagnostic>(&zero) == nullptr, "success does not expose a diagnostic");
    check(std::get_if<int>(&failed) == nullptr, "failure does not expose a success value");

    constexpr std::array codes{ErrorCode::InvalidIdFormat, ErrorCode::InvalidIdVersion,
                               ErrorCode::InvalidIdVariant, ErrorCode::IdGenerationFailed};
    constexpr std::array expected_names{std::string_view("InvalidIdFormat"),
                                        std::string_view("InvalidIdVersion"),
                                        std::string_view("InvalidIdVariant"),
                                        std::string_view("IdGenerationFailed")};
    std::set<std::string_view> names;
    for (std::size_t index = 0; index < codes.size(); ++index) {
        const auto code = codes[index];
        const auto name = error_code_name(code);
        check(name == expected_names[index], "each error code retains its stable name");
        check(names.insert(name).second, "error code names distinguish failure causes");
        const auto expected = code == ErrorCode::IdGenerationFailed ? ErrorCategory::Resource
                                                                   : ErrorCategory::Structure;
        check(error_category(code) == expected, "error code determines the category");
    }
    const auto unknown_code = static_cast<ErrorCode>(0xffffffffU);
    check(error_category(unknown_code) == ErrorCategory::Unknown, "unknown error code does not masquerade as a known category");
    check(error_code_name(unknown_code) == "UnknownErrorCode", "unknown error code has an explicit fallback name");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Expected one test group argument\n";
        return 2;
    }
    const std::string_view group = argv[1];
    try {
        if (group == "parse_valid") {
            check_valid_ids<SolutionId>();
            check_valid_ids<WorkflowId>();
            check_valid_ids<NodeId>();
        } else if (group == "parse_invalid") {
            parse_invalid();
        } else if (group == "generate") {
            std::set<std::string> samples;
            check_generated_ids<SolutionId>(samples);
            check_generated_ids<WorkflowId>(samples);
            check_generated_ids<NodeId>(samples);
            check(samples.size() == 768, "all generation samples are distinct within this run");
        } else if (group == "value_semantics") {
            check_value_semantics<SolutionId>();
            check_value_semantics<WorkflowId>();
            check_value_semantics<NodeId>();
        } else if (group == "diagnostics") {
            diagnostics();
        } else if (group == "--force-failure") {
            check(false, "intentional failure to verify the test process exits nonzero");
        } else {
            std::cerr << "Unknown test group: " << group << '\n';
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Unexpected non-standard exception\n";
        return 1;
    }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
