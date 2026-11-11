#include <visionpro/core/value.hpp>
#include <visionpro/core/text.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace visionpro::core {
namespace detail {

template <class Variant, class... Extra> struct ExtendVariant;
template <class... Types, class... Extra>
struct ExtendVariant<std::variant<Types...>, Extra...> { using type = std::variant<Types..., Extra...>; };

struct TypeNode {
    TypeKind kind;
    std::optional<SchemaId> schema;
    std::uint32_t version{0};
    std::vector<EnumMember> members;
    std::vector<FieldSchema> fields;
    std::optional<DataType> element;
};

} // namespace detail
namespace {

Diagnostic failure(ErrorCode code, std::string message)
{
    return {code, Severity::Error, {}, std::move(message)};
}

Diagnostic schema_failure(ErrorCode code, const SchemaId& id, std::string message)
{
    auto error = failure(code, std::move(message));
    error.location.schema = id;
    return error;
}

} // namespace

DataType DataType::scalar(TypeKind kind)
{
    return DataType{std::make_shared<const detail::TypeNode>(detail::TypeNode{kind, {}, 0, {}, {}, {}})};
}

DataType DataType::boolean() { return scalar(TypeKind::Bool); }
DataType DataType::integer() { return scalar(TypeKind::Int64); }
DataType DataType::real() { return scalar(TypeKind::Float64); }
DataType DataType::string() { return scalar(TypeKind::String); }
DataType DataType::binary() { return scalar(TypeKind::Binary); }
DataType DataType::image() { return scalar(TypeKind::Image); }
DataType DataType::raster_mask() { return scalar(TypeKind::RasterMask); }
DataType DataType::point2d() { return scalar(TypeKind::Point2D); }
DataType DataType::line2d() { return scalar(TypeKind::Line2D); }
DataType DataType::circle2d() { return scalar(TypeKind::Circle2D); }
DataType DataType::pose2d() { return scalar(TypeKind::Pose2D); }
DataType DataType::transform2d() { return scalar(TypeKind::Transform2D); }
DataType DataType::region2d() { return scalar(TypeKind::Region2D); }
DataType DataType::resource_reference() { return scalar(TypeKind::ResourceReference); }

Result<DataType> DataType::enumeration(SchemaId id, std::uint32_t version,
    const std::vector<EnumMember>& source)
{
    auto members = source;
    if (version == 0 || members.empty()) {
        return schema_failure(ErrorCode::InvalidSchema, id, "Enum requires a nonzero version and at least one member.");
    }
    std::ranges::sort(members, {}, &EnumMember::id);
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (!is_valid_utf8(members[index].display_name)) {
            return schema_failure(ErrorCode::InvalidUtf8, id, "Enum display names must be valid UTF-8.");
        }
        if (index != 0 && members[index - 1].id == members[index].id) {
            return schema_failure(ErrorCode::DuplicateSchemaMember, id, "Enum member IDs must be unique.");
        }
    }
    return DataType{std::make_shared<const detail::TypeNode>(
        detail::TypeNode{TypeKind::Enum, std::move(id), version, std::move(members), {}, {}})};
}

Result<DataType> DataType::record(SchemaId id, std::uint32_t version,
    const std::vector<FieldSchema>& source)
{
    auto fields = source;
    if (version == 0) {
        return schema_failure(ErrorCode::InvalidSchema, id, "Record requires a nonzero Schema version.");
    }
    std::ranges::sort(fields, {}, &FieldSchema::id);
    for (std::size_t index = 1; index < fields.size(); ++index) {
        if (fields[index - 1].id == fields[index].id) {
            auto error = schema_failure(ErrorCode::DuplicateSchemaMember, id, "Record field IDs must be unique.");
            error.location.path.emplace_back(fields[index].id);
            return error;
        }
    }
    return DataType{std::make_shared<const detail::TypeNode>(
        detail::TypeNode{TypeKind::Record, std::move(id), version, {}, std::move(fields), {}})};
}

DataType DataType::list(DataType element)
{
    return DataType{std::make_shared<const detail::TypeNode>(
        detail::TypeNode{TypeKind::List, {}, 0, {}, {}, std::move(element)})};
}

DataType DataType::optional(DataType element)
{
    return DataType{std::make_shared<const detail::TypeNode>(
        detail::TypeNode{TypeKind::Optional, {}, 0, {}, {}, std::move(element)})};
}

TypeKind DataType::kind() const noexcept { return node_->kind; }
const DataType* DataType::element_type() const noexcept { return node_->element ? &*node_->element : nullptr; }
const std::optional<SchemaId>& DataType::schema_id() const noexcept { return node_->schema; }
std::uint32_t DataType::schema_version() const noexcept { return node_->version; }
const std::vector<EnumMember>& DataType::enum_members() const noexcept { return node_->members; }
const std::vector<FieldSchema>& DataType::fields() const noexcept { return node_->fields; }

bool DataType::operator==(const DataType& other) const
{
    if (node_ == other.node_) { return true; }
    if (kind() != other.kind() || schema_id() != other.schema_id()
        || schema_version() != other.schema_version()) { return false; }
    if (node_->element != other.node_->element) { return false; }
    if (node_->members.size() != other.node_->members.size() || node_->fields.size() != other.node_->fields.size()) {
        return false;
    }
    for (std::size_t index = 0; index < node_->members.size(); ++index) {
        if (node_->members[index].id != other.node_->members[index].id) { return false; }
    }
    for (std::size_t index = 0; index < node_->fields.size(); ++index) {
        if (node_->fields[index].id != other.node_->fields[index].id
            || !node_->fields[index].type.operator==(other.node_->fields[index].type)) { return false; }
    }
    return true;
}

template <ValueDomain Domain>
struct BasicValue<Domain>::Node {
    using ConfigPayload = std::variant<bool, std::int64_t, double, std::string,
        std::vector<std::uint8_t>, EnumMemberId, std::vector<BasicValue>,
        std::vector<BasicFieldValue<Domain>>, std::optional<BasicValue>,
        Point2D, Line2D, Circle2D, Pose2D, Transform2D, RegionDefinition, ResourceReference>;
    using Payload = std::conditional_t<Domain == ValueDomain::Data,
        typename detail::ExtendVariant<ConfigPayload, ImageFrame, RasterMask>::type, ConfigPayload>;
    DataType type;
    Payload payload;
};

template <ValueDomain Domain>
BasicValue<Domain> BasicValue<Domain>::boolean(bool value)
{
    return BasicValue{std::make_shared<const Node>(Node{DataType::boolean(), value})};
}

template <ValueDomain Domain>
BasicValue<Domain> BasicValue<Domain>::integer(std::int64_t value)
{
    return BasicValue{std::make_shared<const Node>(Node{DataType::integer(), value})};
}

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::real(double value)
{
    if (!std::isfinite(value)) {
        return failure(ErrorCode::InvalidValue, "Float64 values must be finite; NaN and infinity are not valid data.");
    }
    return BasicValue{std::make_shared<const Node>(Node{DataType::real(), value})};
}

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::string(std::string_view value)
{
    if (!is_valid_utf8(value)) {
        return failure(ErrorCode::InvalidUtf8, "String values must contain valid UTF-8 Unicode scalar values.");
    }
    return BasicValue{std::make_shared<const Node>(Node{DataType::string(), std::string(value)})};
}

template <ValueDomain Domain>
BasicValue<Domain> BasicValue<Domain>::binary(const std::vector<std::uint8_t>& value)
{
    return BasicValue{std::make_shared<const Node>(Node{DataType::binary(), value})};
}

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::enumeration(DataType type, EnumMemberId value)
{
    if (type.kind() != TypeKind::Enum) {
        return failure(ErrorCode::TypeMismatch, "An Enum value requires an Enum Schema.");
    }
    if (std::ranges::none_of(type.enum_members(), [&](const auto& member) { return member.id == value; })) {
        auto error = failure(ErrorCode::InvalidEnumValue, "Enum member is not declared by its Schema.");
        error.location.schema = type.schema_id();
        return error;
    }
    return BasicValue{std::make_shared<const Node>(Node{std::move(type), std::move(value)})};
}

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::list(DataType element, const std::vector<BasicValue>& values)
{
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index].type() != element) {
            auto error = failure(ErrorCode::TypeMismatch, "List element type differs from the declared element type.");
            error.location.path.emplace_back(DiagnosticElementIndex{index});
            return error;
        }
    }
    return BasicValue{std::make_shared<const Node>(Node{DataType::list(std::move(element)), values})};
}

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::record(DataType type,
    const std::vector<BasicFieldValue<Domain>>& source)
{
    auto fields = source;
    if (type.kind() != TypeKind::Record) {
        return failure(ErrorCode::TypeMismatch, "A Record value requires a Record Schema.");
    }
    const auto invalid_field = [&](ErrorCode code, const FieldId& id, std::string message) {
        auto error = failure(code, std::move(message));
        error.location.schema = type.schema_id();
        error.location.path.emplace_back(id);
        return error;
    };
    std::ranges::sort(fields, {}, &BasicFieldValue<Domain>::id);
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0 && fields[index - 1].id == fields[index].id) {
            return invalid_field(ErrorCode::DuplicateSchemaMember, fields[index].id, "Record repeats a field.");
        }
        const auto found = std::ranges::find(type.fields(), fields[index].id, &FieldSchema::id);
        if (found == type.fields().end()) {
            return invalid_field(ErrorCode::UnknownField, fields[index].id, "Record contains an undeclared field.");
        }
        if (found->type != fields[index].value.type()) {
            return invalid_field(ErrorCode::TypeMismatch, fields[index].id, "Record field value has the wrong type.");
        }
    }
    for (const auto& declared : type.fields()) {
        if (std::ranges::find(fields, declared.id, &BasicFieldValue<Domain>::id) == fields.end()) {
            return invalid_field(ErrorCode::MissingField, declared.id,
                "Record is missing a declared field; Optional fields require an explicit None value.");
        }
    }
    return BasicValue{std::make_shared<const Node>(Node{std::move(type), std::move(fields)})};
}

template <ValueDomain Domain>
BasicValue<Domain> BasicValue<Domain>::none(DataType element)
{
    return BasicValue{std::make_shared<const Node>(Node{DataType::optional(std::move(element)), std::optional<BasicValue>{}})};
}

template <ValueDomain Domain>
BasicValue<Domain> BasicValue<Domain>::some(BasicValue value)
{
    auto type = DataType::optional(value.type());
    return BasicValue{std::make_shared<const Node>(Node{std::move(type), std::optional<BasicValue>{std::move(value)}})};
}

template <ValueDomain Domain>
const DataType& BasicValue<Domain>::type() const noexcept { return node_->type; }
template <ValueDomain Domain>
const bool* BasicValue<Domain>::as_bool() const noexcept { return std::get_if<bool>(&node_->payload); }
template <ValueDomain Domain>
const std::int64_t* BasicValue<Domain>::as_int64() const noexcept { return std::get_if<std::int64_t>(&node_->payload); }
template <ValueDomain Domain>
const double* BasicValue<Domain>::as_float64() const noexcept { return std::get_if<double>(&node_->payload); }
template <ValueDomain Domain>
const std::string* BasicValue<Domain>::as_string() const noexcept { return std::get_if<std::string>(&node_->payload); }
template <ValueDomain Domain>
const std::vector<std::uint8_t>* BasicValue<Domain>::as_binary() const noexcept { return std::get_if<std::vector<std::uint8_t>>(&node_->payload); }
template <ValueDomain Domain>
const EnumMemberId* BasicValue<Domain>::as_enum() const noexcept { return std::get_if<EnumMemberId>(&node_->payload); }
template <ValueDomain Domain>
const std::vector<BasicValue<Domain>>* BasicValue<Domain>::elements() const noexcept { return std::get_if<std::vector<BasicValue>>(&node_->payload); }
template <ValueDomain Domain>
const std::vector<BasicFieldValue<Domain>>* BasicValue<Domain>::fields() const noexcept { return std::get_if<std::vector<BasicFieldValue<Domain>>>(&node_->payload); }

template <ValueDomain Domain>
const BasicValue<Domain>* BasicValue<Domain>::field(const FieldId& id) const noexcept
{
    const auto* values = fields();
    if (values == nullptr) { return nullptr; }
    const auto found = std::ranges::find(*values, id, &BasicFieldValue<Domain>::id);
    return found == values->end() ? nullptr : &found->value;
}

template <ValueDomain Domain>
const BasicValue<Domain>* BasicValue<Domain>::optional_value() const noexcept
{
    const auto* value = std::get_if<std::optional<BasicValue>>(&node_->payload);
    return value != nullptr && value->has_value() ? &**value : nullptr;
}

template <ValueDomain Domain>
bool BasicValue<Domain>::operator==(const BasicValue& other) const
{
    return node_ == other.node_ || (type() == other.type() && node_->payload == other.node_->payload);
}

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::point2d(Point2D value)
{
    auto checked = validate_point(value);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
    return BasicValue{std::make_shared<const Node>(Node{DataType::point2d(), std::move(value)})};
}

template <ValueDomain Domain>
const Point2D* BasicValue<Domain>::as_point2d() const noexcept
{ return std::get_if<Point2D>(&node_->payload); }

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::line2d(Line2D value)
{
    auto checked = validate_line(value);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
    return BasicValue{std::make_shared<const Node>(Node{DataType::line2d(), std::move(value)})};
}

template <ValueDomain Domain>
const Line2D* BasicValue<Domain>::as_line2d() const noexcept
{ return std::get_if<Line2D>(&node_->payload); }

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::circle2d(Circle2D value)
{
    auto checked = validate_circle(value);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
    return BasicValue{std::make_shared<const Node>(Node{DataType::circle2d(), std::move(value)})};
}

template <ValueDomain Domain>
const Circle2D* BasicValue<Domain>::as_circle2d() const noexcept
{ return std::get_if<Circle2D>(&node_->payload); }

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::pose2d(Pose2D value)
{
    auto checked = validate_pose(value);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
    return BasicValue{std::make_shared<const Node>(Node{DataType::pose2d(), std::move(value)})};
}

template <ValueDomain Domain>
const Pose2D* BasicValue<Domain>::as_pose2d() const noexcept
{ return std::get_if<Pose2D>(&node_->payload); }

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::transform2d(Transform2D value)
{
    auto checked = validate_transform(value);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
    return BasicValue{std::make_shared<const Node>(Node{DataType::transform2d(), std::move(value)})};
}

template <ValueDomain Domain>
const Transform2D* BasicValue<Domain>::as_transform2d() const noexcept
{ return std::get_if<Transform2D>(&node_->payload); }

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::region2d(const RegionDefinition& value)
{
    auto checked = validate_region(value);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
    return BasicValue{std::make_shared<const Node>(Node{DataType::region2d(), value})};
}

template <ValueDomain Domain>
const RegionDefinition* BasicValue<Domain>::as_region2d() const noexcept
{ return std::get_if<RegionDefinition>(&node_->payload); }

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::resource_reference(ResourceReference value)
{
    auto checked = validate_resource_reference(value);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
    return BasicValue{std::make_shared<const Node>(Node{DataType::resource_reference(), std::move(value)})};
}

template <ValueDomain Domain>
const ResourceReference* BasicValue<Domain>::as_resource_reference() const noexcept
{ return std::get_if<ResourceReference>(&node_->payload); }

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::image(ImageFrame value) requires (Domain == ValueDomain::Data)
{
    return BasicValue{std::make_shared<const Node>(Node{DataType::image(), std::move(value)})};
}

template <ValueDomain Domain>
Result<BasicValue<Domain>> BasicValue<Domain>::raster_mask(RasterMask value) requires (Domain == ValueDomain::Data)
{
    auto checked = validate_raster_mask(value);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
    return BasicValue{std::make_shared<const Node>(Node{DataType::raster_mask(), std::move(value)})};
}

template <ValueDomain Domain>
const ImageFrame* BasicValue<Domain>::as_image() const noexcept requires (Domain == ValueDomain::Data)
{ return std::get_if<ImageFrame>(&node_->payload); }

template <ValueDomain Domain>
const RasterMask* BasicValue<Domain>::as_raster_mask() const noexcept requires (Domain == ValueDomain::Data)
{ return std::get_if<RasterMask>(&node_->payload); }

template class BasicValue<ValueDomain::Config>;
template class BasicValue<ValueDomain::Data>;

} // namespace visionpro::core
