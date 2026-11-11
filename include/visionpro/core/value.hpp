#pragma once

#include <visionpro/core/result.hpp>
#include <visionpro/core/geometry.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace visionpro::core {

enum class TypeKind {
    Bool, Int64, Float64, String, Binary, Enum, Record, List, Optional,
    Image, Point2D, Line2D, Circle2D, Pose2D, Transform2D, Region2D, RasterMask, ResourceReference
};
struct EnumMember;
struct FieldSchema;
namespace detail { struct TypeNode; }

// Factories copy caller-owned containers before publishing an immutable Schema,
// including rvalue inputs whose writable element aliases may still exist.
// Copies of the resulting DataType share const data.
class DataType final {
public:
    DataType(const DataType&) noexcept = default;
    DataType& operator=(const DataType&) noexcept = default;
    // Preserve the invariant even in moved-from values.
    DataType(DataType&& other) noexcept : node_(other.node_) {}
    DataType& operator=(DataType&& other) noexcept { node_ = other.node_; return *this; }

    [[nodiscard]] static DataType boolean();
    [[nodiscard]] static DataType integer();
    [[nodiscard]] static DataType real();
    [[nodiscard]] static DataType string(); // UTF-8 Unicode scalar values.
    [[nodiscard]] static DataType binary(); // Uninterpreted, length-preserving bytes.
    [[nodiscard]] static DataType image();
    [[nodiscard]] static DataType raster_mask();
    [[nodiscard]] static DataType point2d();
    [[nodiscard]] static DataType line2d();
    [[nodiscard]] static DataType circle2d();
    [[nodiscard]] static DataType pose2d();
    [[nodiscard]] static DataType transform2d();
    [[nodiscard]] static DataType region2d();
    [[nodiscard]] static DataType resource_reference();
    [[nodiscard]] static Result<DataType> enumeration(SchemaId id, std::uint32_t version,
        const std::vector<EnumMember>& members);
    [[nodiscard]] static Result<DataType> record(SchemaId id, std::uint32_t version,
        const std::vector<FieldSchema>& fields);
    [[nodiscard]] static DataType list(DataType element);
    [[nodiscard]] static DataType optional(DataType element);

    [[nodiscard]] TypeKind kind() const noexcept;
    [[nodiscard]] const DataType* element_type() const noexcept;
    [[nodiscard]] const std::optional<SchemaId>& schema_id() const noexcept;
    [[nodiscard]] std::uint32_t schema_version() const noexcept;
    [[nodiscard]] const std::vector<EnumMember>& enum_members() const noexcept;
    [[nodiscard]] const std::vector<FieldSchema>& fields() const noexcept;
    [[nodiscard]] bool operator==(const DataType& other) const;

private:
    explicit DataType(std::shared_ptr<const detail::TypeNode> node) : node_(std::move(node)) {}
    [[nodiscard]] static DataType scalar(TypeKind kind);
    std::shared_ptr<const detail::TypeNode> node_;
};

struct EnumMember { EnumMemberId id; std::string display_name; };
struct FieldSchema { FieldId id; DataType type; };

enum class ValueDomain { Config, Data };
template <ValueDomain Domain> struct BasicFieldValue;

// There is no null/invalid sentinel. None is an explicitly typed Optional value.
template <ValueDomain Domain>
class BasicValue final {
public:
    BasicValue(const BasicValue&) noexcept = default;
    BasicValue& operator=(const BasicValue&) noexcept = default;
    BasicValue(BasicValue&& other) noexcept : node_(other.node_) {}
    BasicValue& operator=(BasicValue&& other) noexcept { node_ = other.node_; return *this; }

    [[nodiscard]] static BasicValue boolean(bool value);
    [[nodiscard]] static BasicValue integer(std::int64_t value);
    [[nodiscard]] static Result<BasicValue> real(double value);
    [[nodiscard]] static Result<BasicValue> string(std::string_view value);
    [[nodiscard]] static BasicValue binary(const std::vector<std::uint8_t>& value);
    [[nodiscard]] static Result<BasicValue> point2d(Point2D value);
    [[nodiscard]] static Result<BasicValue> line2d(Line2D value);
    [[nodiscard]] static Result<BasicValue> circle2d(Circle2D value);
    [[nodiscard]] static Result<BasicValue> pose2d(Pose2D value);
    [[nodiscard]] static Result<BasicValue> transform2d(Transform2D value);
    [[nodiscard]] static Result<BasicValue> region2d(const RegionDefinition& value);
    [[nodiscard]] static Result<BasicValue> resource_reference(ResourceReference value);
    [[nodiscard]] static Result<BasicValue> image(ImageFrame value) requires (Domain == ValueDomain::Data);
    [[nodiscard]] static Result<BasicValue> raster_mask(RasterMask value) requires (Domain == ValueDomain::Data);
    [[nodiscard]] static Result<BasicValue> enumeration(DataType type, EnumMemberId value);
    [[nodiscard]] static Result<BasicValue> list(DataType element, const std::vector<BasicValue>& values);
    [[nodiscard]] static Result<BasicValue> record(DataType type,
        const std::vector<BasicFieldValue<Domain>>& fields);
    [[nodiscard]] static BasicValue none(DataType element);
    [[nodiscard]] static BasicValue some(BasicValue value);

    [[nodiscard]] const DataType& type() const noexcept;
    [[nodiscard]] const bool* as_bool() const noexcept;
    [[nodiscard]] const std::int64_t* as_int64() const noexcept;
    [[nodiscard]] const double* as_float64() const noexcept;
    [[nodiscard]] const std::string* as_string() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>* as_binary() const noexcept;
    [[nodiscard]] const Point2D* as_point2d() const noexcept;
    [[nodiscard]] const Line2D* as_line2d() const noexcept;
    [[nodiscard]] const Circle2D* as_circle2d() const noexcept;
    [[nodiscard]] const Pose2D* as_pose2d() const noexcept;
    [[nodiscard]] const Transform2D* as_transform2d() const noexcept;
    [[nodiscard]] const RegionDefinition* as_region2d() const noexcept;
    [[nodiscard]] const ResourceReference* as_resource_reference() const noexcept;
    [[nodiscard]] const ImageFrame* as_image() const noexcept requires (Domain == ValueDomain::Data);
    [[nodiscard]] const RasterMask* as_raster_mask() const noexcept requires (Domain == ValueDomain::Data);
    [[nodiscard]] const EnumMemberId* as_enum() const noexcept;
    [[nodiscard]] const std::vector<BasicValue>* elements() const noexcept;
    [[nodiscard]] const std::vector<BasicFieldValue<Domain>>* fields() const noexcept;
    [[nodiscard]] const BasicValue* field(const FieldId& id) const noexcept;
    [[nodiscard]] const BasicValue* optional_value() const noexcept;
    [[nodiscard]] bool operator==(const BasicValue& other) const;

private:
    struct Node;
    explicit BasicValue(std::shared_ptr<const Node> node) : node_(std::move(node)) {}
    std::shared_ptr<const Node> node_;
};

template <ValueDomain Domain>
struct BasicFieldValue {
    FieldId id;
    BasicValue<Domain> value;
    bool operator==(const BasicFieldValue&) const = default;
};

using ConfigValue = BasicValue<ValueDomain::Config>;
using DataValue = BasicValue<ValueDomain::Data>;
using ConfigFieldValue = BasicFieldValue<ValueDomain::Config>;
using DataFieldValue = BasicFieldValue<ValueDomain::Data>;

extern template class BasicValue<ValueDomain::Config>;
extern template class BasicValue<ValueDomain::Data>;

} // namespace visionpro::core
