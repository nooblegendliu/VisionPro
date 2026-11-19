#include <visionpro/workflow/correlation_join.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
using Status = CorrelationJoinStatus;

template<class T> T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error->message); }
    return std::get<T>(std::move(result));
}
void check(bool success, const char* message) { if (!success) { throw std::runtime_error(message); } }
template<SchemaIdentity T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }
CorrelationId piece(std::string_view name, std::string_view domain = "line.one")
{ return take(RuntimeIdFactory::correlation(key<CorrelationDomainId>(domain), name)); }

struct Fixture {
    const FieldId width = key<FieldId>("width");
    const FieldId height = key<FieldId>("height");
    CorrelationJoinConfig config{key<SchemaId>("measure"),
        take(DataType::record(key<SchemaId>("dimension"), 1, {{width, DataType::integer()}, {height, DataType::integer()}})),
        2, 5ms, 6};
    CorrelationJoinService::Clock::time_point now{};
    CorrelationJoinService service{[this] { return now; }, 2};

    CorrelationJoinResult offer(const CorrelationId& item, const FieldId& field, std::int64_t dimension, std::uint64_t batch = 1)
    { return take(service.offer(config, batch, item, field, DataValue::integer(dimension))); }
};

void interleaved_and_bounded()
{
    Fixture f;
    const auto a = piece("A"), b = piece("B"), c = piece("C"), d = piece("D");
    check(f.offer(a, f.width, 10).status == Status::Pending && f.offer(b, f.width, 20).status == Status::Pending, "two open workpieces");
    check(f.offer(c, f.width, 30).status == Status::Capacity, "pending capacity");
    check(f.offer(b, f.width, -100).status == Status::Duplicate, "duplicate retains first contribution");
    const auto B = f.offer(b, f.height, 2);
    const auto A = f.offer(a, f.height, 1);
    check(B.stats.pending == 1 && B.stats.completed == 1 && A.stats.pending == 0 && A.stats.completed == 2,
        "each offer captures its own statistics before another can change them");
    check(B.status == Status::Complete && A.status == Status::Complete && B.record && A.record &&
        *B.record->field(f.width)->as_int64() == 20 && *B.record->field(f.height)->as_int64() == 2 &&
        *A.record->field(f.width)->as_int64() == 10 && *A.record->field(f.height)->as_int64() == 1,
        "B then A deliver complete paired Records exactly once");
    check(!f.offer(b, f.height, 3).record && f.offer(c, f.height, 3).status == Status::Pending, "closed replay yields no record; capacity is released");
    const auto c_other = piece("C", "line.two");
    check(f.offer(c_other, f.width, 301).status == Status::Pending && f.offer(d, f.width, 40).status == Status::Capacity,
        "same key in a different correlation domain occupies an independent slot");
    auto info = f.service.inspect_item(f.config.join, 1, c);
    check(info && info->state == CorrelationItemState::Pending && info->missing == std::vector{f.width}, "pending item exposes missing sources");
    f.now += 4ms;
    check(f.offer(c, f.height, 400).status == Status::Duplicate && f.service.inspect(f.config.join)->pending == 2, "duplicate does not change expiry");
    f.now += 1ms;
    info = f.service.inspect_item(f.config.join, 1, c);
    check(info && info->state == CorrelationItemState::Expired && info->missing == std::vector{f.width}, "timeout at exact deadline drops partial values");
    check(f.offer(c_other, f.height, 302).status == Status::Late && f.offer(c, f.width, 30).status == Status::Late, "expired workpieces cannot reopen");
    check(f.offer(d, f.width, 4).status == Status::Pending && f.offer(piece("E"), f.height, 5).status == Status::Pending, "timed-out slots are reusable");
    check(f.offer(piece("F"), f.height, 6).status == Status::Capacity, "bounded batch refuses new keys without evicting replay protection");
    check(f.offer(a, f.width, 400).status == Status::Duplicate, "delivered key stays closed at the batch key limit");
    auto stats = f.service.inspect(f.config.join).value();
    check(stats.pending == 2 && stats.retained_keys == 6 && stats.completed == 2 && stats.duplicates == 4 && stats.expired == 2 &&
        stats.late == 2 && stats.pending_capacity_rejections == 2 && stats.batch_capacity_rejections == 1, "observable join counters");
    check(f.offer(a, f.height, 11, 2).status == Status::Pending && f.offer(a, f.width, 100, 1).status == Status::Late, "stale batch cannot advance a new batch");
    const auto next = f.offer(a, f.width, 110, 2);
    check(next.record && *next.record->field(f.height)->as_int64() == 11 && *next.record->field(f.width)->as_int64() == 110, "next batch reuses the same workpiece safely");
    stats = f.service.inspect(f.config.join).value();
    check(stats.batch == 2 && stats.pending == 0 && stats.retained_keys == 1 && stats.abandoned_at_rollover == 2 && !f.service.inspect_item(f.config.join, 1, a),
        "batch rollover releases all previous keys and reports unfinished work");
}

void invalid_and_reconfigured()
{
    Fixture f;
    const auto a = piece("A");
    check(f.offer(a, f.width, 1).status == Status::Pending, "seed active batch");
    auto changed = f.config;
    ++changed.capacity;
    check(std::get<Diagnostic>(f.service.offer(changed, 1, a, f.height, DataValue::integer(8))).code == ErrorCode::InvalidState, "batch configuration is immutable");
    const auto bad = f.service.offer(changed, 2, a, f.height, DataValue::boolean(true));
    check(std::get<Diagnostic>(bad).location.path == std::vector<DiagnosticPathStep>{f.height}, "invalid new batch contribution identifies field");
    check(std::get<Diagnostic>(f.service.offer(changed, 2, a, key<FieldId>("depth"), DataValue::integer(1))).code == ErrorCode::UnknownField, "undeclared source rejected");
    check(f.offer(a, f.height, 9).record.has_value(), "invalid offers leave the old batch intact");
    changed.record = take(DataType::record(key<SchemaId>("quality"), 2, {{key<FieldId>("ok"), DataType::boolean()}}));
    const auto ok = key<FieldId>("ok");
    const auto delivered = take(f.service.offer(changed, 2, a, ok, DataValue::boolean(false)));
    check(delivered.record && delivered.record->type() == changed.record && !*delivered.record->field(ok)->as_bool(), "new batch may use a different typed required set");
    changed.join = key<SchemaId>("second");
    check(take(f.service.offer(changed, 1, a, ok, DataValue::boolean(true))).record.has_value(), "independent join namespace");
    changed.join = key<SchemaId>("third");
    const auto limit = take(f.service.offer(changed, 1, a, ok, DataValue::boolean(true)));
    check(limit.status == Status::Capacity && limit.stats.join_capacity_rejections == 1 && f.service.join_capacity_rejections() == 1,
        "session bounds the number of live joins");
    const auto all = f.service.inspect_all();
    check(all.size() == 2 && all[0].join == f.config.join && all[0].stats.completed == 2 && all[1].stats.retained_keys == 1,
        "read-only catalog includes both named joins without retaining payloads");
    changed.timeout = 0ms;
    check(std::get<Diagnostic>(f.service.offer(changed, 1, a, ok, DataValue::boolean(true))).code == ErrorCode::ConstraintViolation, "invalid timeout rejected");
    check(!f.service.inspect(changed.join), "failed offers create no join");
}

void concurrent_last_contribution()
{
    Fixture f;
    const auto a = piece("A");
    check(f.offer(a, f.height, 9).status == Status::Pending, "seed concurrent work");
    std::array<CorrelationJoinResult, 8> results{};
    std::barrier start(static_cast<std::ptrdiff_t>(results.size()));
    std::vector<std::jthread> workers;
    for (auto& result : results) {
        workers.emplace_back([&f, &a, &start, &result] { start.arrive_and_wait(); result = f.offer(a, f.width, 14); });
    }
    workers.clear();
    std::size_t delivered = 0, duplicates = 0;
    for (const auto& result : results) {
        if (result.status == Status::Complete && result.record) { ++delivered; }
        if (result.status == Status::Duplicate && !result.record) { ++duplicates; }
    }
    check(delivered == 1 && duplicates == 7 && f.service.inspect(f.config.join)->pending == 0, "concurrent offers deliver exactly once");
}
} // namespace

int main()
{
    try { interleaved_and_bounded(); invalid_and_reconfigured(); concurrent_last_contribution(); }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
    return 0;
}
