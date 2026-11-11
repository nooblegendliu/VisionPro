#include <visionpro/core/tool_descriptor.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace visionpro::core;

template <class V>
concept ImageFactory = requires(ImageFrame frame) {
    { V::image(frame) } -> std::same_as<Result<V>>;
};
template <class V>
concept ImageAccessor = requires(V value) {
    { value.as_image() } -> std::same_as<const ImageFrame*>;
};
template <class V>
concept MaskFactory = requires(RasterMask mask) {
    { V::raster_mask(mask) } -> std::same_as<Result<V>>;
};
template <class V>
concept MaskAccessor = requires(V value) {
    { value.as_raster_mask() } -> std::same_as<const RasterMask*>;
};
template <class Destination, class Source>
concept CanWrapOptional = requires(Source value) { Destination::some(value); };
template <class Destination, class Source>
concept CanWrapList = requires(DataType type, std::vector<Source> values) {
    Destination::list(type, values);
};
template <class Destination, class Field>
concept CanWrapRecord = requires(DataType type, std::vector<Field> fields) {
    Destination::record(type, fields);
};
template <class V>
concept DomainFactories = requires(Point2D point, Line2D line, Circle2D circle, Pose2D pose,
    Transform2D transform, RegionDefinition region, ResourceReference resource, V value) {
    { V::point2d(point) } -> std::same_as<Result<V>>;
    { V::line2d(line) } -> std::same_as<Result<V>>;
    { V::circle2d(circle) } -> std::same_as<Result<V>>;
    { V::pose2d(pose) } -> std::same_as<Result<V>>;
    { V::transform2d(transform) } -> std::same_as<Result<V>>;
    { V::region2d(region) } -> std::same_as<Result<V>>;
    { V::resource_reference(resource) } -> std::same_as<Result<V>>;
    { value.as_point2d() } -> std::same_as<const Point2D*>;
    { value.as_line2d() } -> std::same_as<const Line2D*>;
    { value.as_circle2d() } -> std::same_as<const Circle2D*>;
    { value.as_pose2d() } -> std::same_as<const Pose2D*>;
    { value.as_transform2d() } -> std::same_as<const Transform2D*>;
    { value.as_region2d() } -> std::same_as<const RegionDefinition*>;
    { value.as_resource_reference() } -> std::same_as<const ResourceReference*>;
};

static_assert(ImageFactory<DataValue> && !ImageFactory<ConfigValue>);
static_assert(ImageAccessor<DataValue> && !ImageAccessor<ConfigValue>);
static_assert(MaskFactory<DataValue> && !MaskFactory<ConfigValue>);
static_assert(MaskAccessor<DataValue> && !MaskAccessor<ConfigValue>);
static_assert(DomainFactories<DataValue> && DomainFactories<ConfigValue>);
static_assert(!std::is_constructible_v<ConfigValue, ImageFrame>);
static_assert(!std::is_constructible_v<ConfigValue, RasterMask>);
static_assert(!std::is_constructible_v<ConfigValue, ImageHandle>);
static_assert(!std::is_constructible_v<ConfigValue, DataValue>);
static_assert(!CanWrapOptional<ConfigValue, DataValue>);
static_assert(!CanWrapList<ConfigValue, DataValue>);
static_assert(!CanWrapRecord<ConfigValue, DataFieldValue>);
static_assert(CanWrapOptional<ConfigValue, ConfigValue>);
static_assert(CanWrapList<ConfigValue, ConfigValue>);
static_assert(CanWrapRecord<ConfigValue, ConfigFieldValue>);
static_assert(!std::is_constructible_v<RegionShape, RasterMask>);
static_assert(std::is_same_v<decltype(std::declval<DataValue&>().as_image()->image.row(0)),
    Result<std::span<const std::byte>>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(
    (std::declval<ConfigValue&>().as_region2d()->exclusions))>>);

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
T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error("Invalid domain fixture: " + failure->message);
    }
    return std::get<T>(std::move(result));
}

template <class T>
Diagnostic error(const Result<T>& result, ErrorCode expected)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure != nullptr, "invalid domain data returns a Diagnostic");
    if (!failure) { throw std::runtime_error("Unexpectedly successful result"); }
    check(failure->code == expected, "domain diagnostic preserves the failure cause");
    check(failure->severity == Severity::Error && !failure->message.empty(),
        "domain diagnostic has severity and an explanation");
    return *failure;
}

template <SchemaIdentity Id>
Id key(std::string_view text) { return take(SchemaIdFactory::parse<Id>(text)); }
template <UuidIdentity Id>
Id uuid() { return take(IdFactory::parse<Id>("12345678-1234-4abc-8def-123456789abc")); }

CoordinateSystemId frame() { return key<CoordinateSystemId>("image.sensor"); }

ImageHandle image(PixelFormat format = {PixelDepth::UInt8, ChannelOrder::Mono})
{
    const auto pixel_bytes = take(bytes_per_pixel(format));
    std::vector<std::byte> pixels(4 * pixel_bytes);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        pixels[index] = static_cast<std::byte>(index + 1);
    }
    return take(ImageHandle::copy({2, 2, 2 * pixel_bytes, format}, pixels, frame()));
}

ResourceReference resource(ResourceKind kind = ResourceKind::Mask)
{ return {uuid<ResourceId>(), kind, 3}; }

RegionDefinition polygon_region()
{
    return {uuid<RegionId>(), frame(), MeasurementUnit::Pixel,
        RegionPrimitive{frame(), MeasurementUnit::Pixel,
            PolygonRegion{{{0, 0}, {8, 0}, {8, 8}, {0, 8}}}, 0.25, std::nullopt},
        {RegionPrimitive{frame(), MeasurementUnit::Pixel, CircleRegion{2, 2, 0.5}}},
        RegionBoundaryPolicy::Intersect, std::nullopt, EmptyRegionPolicy::ValidEmptyOutput};
}

template <class V>
void retained_container_aliases()
{
    std::vector<std::uint8_t> bytes{1, 2, 3};
    auto* byte_alias = &bytes.front();
    const auto binary = V::binary(std::move(bytes));
    const bool bytes_retained = bytes.size() == 3 && bytes.data() == byte_alias;
    check(bytes_retained, "publishing an rvalue Binary preserves the caller's buffer and aliases");
    if (bytes_retained) { *byte_alias = 99; }
    check(binary.as_binary() && *binary.as_binary() == std::vector<std::uint8_t>{1, 2, 3},
        "a retained writable byte alias cannot modify a published Binary");

    std::vector<V> elements{V::integer(7)};
    auto* element_alias = &elements.front();
    const auto list = take(V::list(DataType::integer(), std::move(elements)));
    const bool elements_retained = elements.size() == 1 && elements.data() == element_alias;
    check(elements_retained, "publishing an rvalue List preserves the caller's element aliases");
    if (elements_retained) { *element_alias = V::boolean(false); }
    check(list.elements() && list.elements()->front().as_int64()
        && *list.elements()->front().as_int64() == 7,
        "replacing a retained List element cannot change the published type or value");

    const auto field = key<FieldId>("count");
    const auto schema = take(DataType::record(key<SchemaId>("retained.record"), 1,
        {{field, DataType::integer()}}));
    using Fields = std::remove_cvref_t<decltype(*std::declval<V&>().fields())>;
    using Field = typename Fields::value_type;
    std::vector<Field> fields{{field, V::integer(11)}};
    auto* field_alias = &fields.front();
    const auto record = take(V::record(schema, std::move(fields)));
    const bool fields_retained = fields.size() == 1 && fields.data() == field_alias;
    check(fields_retained, "publishing an rvalue Record preserves the caller's field aliases");
    if (fields_retained) { field_alias->value = V::boolean(false); }
    check(record.field(field) && record.field(field)->as_int64()
        && *record.field(field)->as_int64() == 11,
        "replacing a retained Record field cannot change its published value");

    auto region = polygon_region();
    const auto expected = region;
    auto& vertices = std::get<PolygonRegion>(std::get<RegionPrimitive>(region.source).geometry).vertices;
    auto* vertex_alias = &vertices.front();
    auto* exclusion_alias = &region.exclusions.front();
    const auto roi = take(V::region2d(std::move(region)));
    const bool region_retained = vertices.size() == 4 && vertices.data() == vertex_alias
        && region.exclusions.size() == 1 && region.exclusions.data() == exclusion_alias;
    check(region_retained, "publishing an rvalue ROI preserves source vertex and exclusion aliases");
    if (region_retained) {
        vertex_alias->x = 100;
        std::get<RegionPrimitive>(*exclusion_alias).geometry = CircleRegion{7, 7, 3};
    }
    check(roi.as_region2d() && *roi.as_region2d() == expected,
        "retained writable vertex and exclusion aliases cannot modify the published ROI");
}

void retained_schema_aliases()
{
    const auto original_member = key<EnumMemberId>("original");
    const std::string original_label = "Original enumeration label longer than small string storage";
    std::vector<EnumMember> members{{original_member, original_label}};
    auto* member_alias = &members.front();
    auto* label_alias = members.front().display_name.data();
    const auto enumeration = take(DataType::enumeration(key<SchemaId>("retained.enum"), 1, std::move(members)));
    const bool members_retained = members.size() == 1 && members.data() == member_alias
        && members.front().display_name.data() == label_alias;
    check(members_retained, "publishing an rvalue Enum Schema preserves caller member and label aliases");
    if (members_retained) {
        member_alias->id = key<EnumMemberId>("replacement");
        *label_alias = 'X';
    }
    check(enumeration.enum_members().size() == 1 && enumeration.enum_members().front().id == original_member
        && enumeration.enum_members().front().display_name == original_label,
        "retained member and string aliases cannot change the published Enum Schema");

    const auto original_field = key<FieldId>("image");
    std::vector<FieldSchema> fields{{original_field, DataType::image()}};
    auto* field_alias = &fields.front();
    const auto record = take(DataType::record(key<SchemaId>("retained.schema"), 1, std::move(fields)));
    const bool fields_retained = fields.size() == 1 && fields.data() == field_alias;
    check(fields_retained, "publishing an rvalue Record Schema preserves caller field aliases");
    if (fields_retained) {
        field_alias->id = key<FieldId>("replacement");
        field_alias->type = DataType::boolean();
    }
    check(record.fields().size() == 1 && record.fields().front().id == original_field
        && record.fields().front().type == DataType::image(),
        "retained field aliases cannot change the published Record Schema");
}

template <class V>
void geometry_values()
{
    const Point2D point{frame(), MeasurementUnit::Pixel, 1.5, 2.5};
    const Line2D line{point, {frame(), MeasurementUnit::Pixel, 4, 6}};
    const Circle2D circle{point, 2};
    const Pose2D pose{point, 0.5};
    const Transform2D transform{frame(), key<CoordinateSystemId>("image.aligned"),
        MeasurementUnit::Pixel, MeasurementUnit::Pixel};
    const auto point_value = take(V::point2d(point));
    const auto line_value = take(V::line2d(line));
    const auto circle_value = take(V::circle2d(circle));
    const auto pose_value = take(V::pose2d(pose));
    const auto transform_value = take(V::transform2d(transform));
    check(point_value.type() == DataType::point2d() && point_value.as_point2d()
        && *point_value.as_point2d() == point, "Point2D retains coordinates and units");
    check(line_value.type() == DataType::line2d() && line_value.as_line2d()
        && *line_value.as_line2d() == line, "Line2D retains typed endpoints");
    check(circle_value.type() == DataType::circle2d() && circle_value.as_circle2d()
        && *circle_value.as_circle2d() == circle, "Circle2D retains center and radius");
    check(pose_value.type() == DataType::pose2d() && pose_value.as_pose2d()
        && *pose_value.as_pose2d() == pose, "Pose2D retains its orientation in radians");
    check(transform_value.type() == DataType::transform2d() && transform_value.as_transform2d()
        && *transform_value.as_transform2d() == transform, "Transform2D retains both coordinate endpoints and matrix");
    check(!point_value.as_line2d() && !point_value.as_circle2d() && !point_value.as_pose2d()
        && !point_value.as_transform2d() && !point_value.as_region2d()
        && !point_value.as_resource_reference(), "domain getters reject a different payload kind");

    auto region = polygon_region();
    const auto expected_region = region;
    const auto region_value = take(V::region2d(region));
    std::get<PolygonRegion>(std::get<RegionPrimitive>(region.source).geometry).vertices.clear();
    region.exclusions.clear();
    check(region_value.type() == DataType::region2d() && region_value.as_region2d()
        && *region_value.as_region2d() == expected_region,
        "Region2D owns polygon and exclusion vectors independently of the caller");
    const auto reference = resource();
    const auto reference_value = take(V::resource_reference(reference));
    check(reference_value.type() == DataType::resource_reference() && reference_value.as_resource_reference()
        && *reference_value.as_resource_reference() == reference,
        "resource values preserve structured identity, kind and pinned revision");

    auto invalid_point = point;
    invalid_point.x = std::numeric_limits<double>::quiet_NaN();
    error(V::point2d(invalid_point), ErrorCode::InvalidGeometry);
    error(V::line2d({point, point}), ErrorCode::InvalidGeometry);
    error(V::circle2d({point, -1}), ErrorCode::InvalidGeometry);
    error(V::pose2d({point, std::numeric_limits<double>::infinity()}), ErrorCode::InvalidGeometry);
    auto invalid_transform = transform;
    invalid_transform.matrix.fill(0);
    error(V::transform2d(invalid_transform), ErrorCode::InvalidTransform);
    const auto region_error = error(V::region2d(region), ErrorCode::InvalidGeometry);
    check(region_error.location.region == region.id, "invalid Region2D retains its region identity");
    auto invalid_reference = reference;
    invalid_reference.revision = 0;
    const auto reference_error = error(V::resource_reference(invalid_reference), ErrorCode::InvalidResource);
    check(reference_error.location.resource == reference.id, "invalid reference retains its resource identity");
    invalid_reference = reference;
    invalid_reference.kind = static_cast<ResourceKind>(-1);
    error(V::resource_reference(invalid_reference), ErrorCode::InvalidResource);
}

void persistent_values()
{
    const auto none_image = ConfigValue::none(DataType::image());
    const auto empty_images = take(ConfigValue::list(DataType::image(), {}));
    check(none_image.type() == DataType::optional(DataType::image()) && !none_image.optional_value(),
        "configuration may declare Optional<Image> with no runtime buffer");
    check(empty_images.type() == DataType::list(DataType::image()) && empty_images.elements()
        && empty_images.elements()->empty(), "an empty configuration List<Image> carries only its Schema");
    const auto none_mask = ConfigValue::none(DataType::raster_mask());
    check(!none_mask.optional_value() && none_mask.type() == DataType::optional(DataType::raster_mask()),
        "configuration may declare an absent runtime mask");

    const MaskConfig mask{resource(), frame(), 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono},
        {frame(), 2, 2, std::nullopt, MaskSampling::None}, {MaskValueMode::NonZero, 0}};
    const RegionDefinition region{uuid<RegionId>(), frame(), MeasurementUnit::Pixel,
        RegionPrimitive{frame(), MeasurementUnit::Pixel, mask}, {}};
    const auto roi = take(ConfigValue::region2d(region));
    const auto* stored = std::get_if<MaskConfig>(&std::get<RegionPrimitive>(roi.as_region2d()->source).geometry);
    check(stored && *stored == mask, "configured mask ROI retains its resource reference, mapping and value rule");
    const auto resource_field = key<FieldId>("resource");
    const auto region_field = key<FieldId>("roi");
    const auto image_field = key<FieldId>("image");
    const auto images_field = key<FieldId>("images");
    const auto schema = take(DataType::record(key<SchemaId>("persistent.domain"), 1,
        {{resource_field, DataType::resource_reference()}, {region_field, DataType::region2d()},
         {image_field, none_image.type()}, {images_field, empty_images.type()}}));
    const auto configuration = take(ConfigValue::record(schema,
        {{resource_field, take(ConfigValue::resource_reference(resource()))}, {region_field, roi},
         {image_field, none_image}, {images_field, empty_images}}));
    check(configuration.field(resource_field)->as_resource_reference()->revision == 3
        && *configuration.field(region_field)->as_region2d() == region,
        "configuration Record preserves persistable resource and ROI structures");
    check(!configuration.field(image_field)->optional_value()
        && configuration.field(images_field)->elements()->empty(),
        "nested configuration placeholders do not materialize image buffers");
}

void runtime_values()
{
    const auto image_field = key<FieldId>("image");
    const auto mask_field = key<FieldId>("mask");
    const auto schema = take(DataType::record(key<SchemaId>("runtime.frame"), 1,
        {{image_field, DataType::image()}, {mask_field, DataType::raster_mask()}}));
    const FrameId identity{uuid<ResourceId>(), 17};
    const auto timestamp = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{123456}};
    std::optional<DataValue> retained;
    {
        const auto pixels = image();
        ImageFrame captured{pixels, identity, timestamp};
        const auto frame_value = take(DataValue::image(captured));
        const RasterMask mask{pixels, {frame(), 2, 2, std::nullopt, MaskSampling::None},
            {MaskValueMode::AtLeastThreshold, 2}};
        const auto mask_value = take(DataValue::raster_mask(mask));
        check(frame_value.type() == DataType::image() && frame_value.as_image()
            && frame_value.as_image()->image.shares_storage_with(pixels),
            "DataValue image construction shares the immutable image storage");
        check(mask_value.type() == DataType::raster_mask() && mask_value.as_raster_mask()
            && mask_value.as_raster_mask()->pixels.shares_storage_with(pixels),
            "DataValue mask construction shares the immutable image storage");
        check(!frame_value.as_raster_mask() && !mask_value.as_image()
            && !frame_value.as_resource_reference(), "image, mask and resource getters remain distinct");
        captured.frame = FrameId{uuid<ResourceId>(), 99};
        captured.captured_at.reset();
        check(frame_value.as_image()->frame == identity && frame_value.as_image()->captured_at == timestamp,
            "ImageFrame metadata is an owned snapshot independent of its source struct");
        std::vector<DataFieldValue> input_fields{{image_field, frame_value}, {mask_field, mask_value}};
        auto* field_alias = &input_fields.front();
        const auto record = take(DataValue::record(schema, std::move(input_fields)));
        const bool fields_retained = input_fields.size() == 2 && input_fields.data() == field_alias;
        check(fields_retained, "publishing runtime fields from an rvalue preserves caller field aliases");
        if (fields_retained) { field_alias->value = DataValue::integer(-1); }
        check(record.field(image_field) && record.field(image_field)->as_image()
            && record.field(image_field)->as_image()->image.shares_storage_with(pixels),
            "replacing a retained runtime field cannot remove or replace the published image");
        std::vector<DataValue> input_images{frame_value};
        auto* image_alias = &input_images.front();
        const auto images = take(DataValue::list(DataType::image(), std::move(input_images)));
        const bool images_retained = input_images.size() == 1 && input_images.data() == image_alias;
        check(images_retained, "publishing an rvalue Image List preserves caller element aliases");
        if (images_retained) { *image_alias = DataValue::integer(-1); }
        check(images.elements() && images.elements()->front().as_image()
            && images.elements()->front().as_image()->image.shares_storage_with(pixels),
            "replacing a retained Image List element cannot change the published image or type");
        auto input_records = std::vector<DataValue>{record};
        auto* record_alias = &input_records.front();
        const auto records = take(DataValue::list(schema, std::move(input_records)));
        const bool records_retained = input_records.size() == 1 && input_records.data() == record_alias;
        check(records_retained, "publishing an rvalue record collection preserves caller element aliases");
        if (records_retained) { *record_alias = DataValue::boolean(false); }
        retained = DataValue::some(records);
        input_records.clear();
        check(retained->optional_value()->elements()->front().field(image_field)->as_image()->image.shares_storage_with(pixels),
            "Record/List/Optional nesting does not copy image buffers");
        check(retained->optional_value()->elements()->front().field(mask_field)->as_raster_mask()->pixels.shares_storage_with(pixels),
            "Record/List/Optional nesting does not copy mask buffers");

        const auto mismatch = error(DataValue::record(schema,
            {{image_field, mask_value}, {mask_field, mask_value}}), ErrorCode::TypeMismatch);
        check(mismatch.location.schema == schema.schema_id() && mismatch.location.path.size() == 1,
            "wrong image field type identifies its Schema and field path");
        if (mismatch.location.path.size() == 1) {
            const auto* bad_field = std::get_if<FieldId>(&mismatch.location.path.front());
            check(bad_field && *bad_field == image_field, "wrong image field diagnostic identifies the image field");
        }
        const auto list_mismatch = error(DataValue::list(DataType::image(), {frame_value, mask_value}), ErrorCode::TypeMismatch);
        check(list_mismatch.location.path.size() == 1
            && std::get_if<DiagnosticElementIndex>(&list_mismatch.location.path.front())
            && std::get<DiagnosticElementIndex>(list_mismatch.location.path.front()).value == 1,
            "a mixed Image/Mask List reports the failing element");
        auto invalid_mask = mask;
        invalid_mask.valid_pixels.threshold = std::numeric_limits<double>::infinity();
        error(DataValue::raster_mask(invalid_mask), ErrorCode::InvalidMask);
    }
    const auto& record = retained->optional_value()->elements()->front();
    const auto& retained_image = *record.field(image_field)->as_image();
    const auto& retained_mask = *record.field(mask_field)->as_raster_mask();
    const std::vector<std::byte> expected{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    check(retained_image.image.copy_pixels() == expected && retained_image.frame == identity
        && retained_image.captured_at == timestamp,
        "nested ImageFrame retains pixels and provenance after all source owners leave scope");
    check(retained_mask.pixels.copy_pixels() == expected
        && retained_mask.valid_pixels == MaskValueRule{MaskValueMode::AtLeastThreshold, 2},
        "nested RasterMask retains pixels and its rule after all source owners leave scope");
    const auto branch = *retained;
    check(branch.optional_value()->elements()->front().field(image_field)->as_image()->image
        .shares_storage_with(retained_image.image), "copying the complete runtime tree keeps shared read-only storage");
    const auto clone = take(DataValue::image({take(retained_image.image.clone()), retained_image.frame, retained_image.captured_at}));
    check(!clone.as_image()->image.shares_storage_with(retained_image.image)
        && clone.as_image()->image.copy_pixels() == expected && clone.as_image()->frame == identity,
        "explicit image cloning produces independent storage while retaining frame provenance");
    const auto mask_clone = take(DataValue::raster_mask(
        {take(retained_mask.pixels.clone()), retained_mask.mapping, retained_mask.valid_pixels}));
    check(!mask_clone.as_raster_mask()->pixels.shares_storage_with(retained_mask.pixels)
        && mask_clone.as_raster_mask()->pixels.copy_pixels() == expected,
        "explicit mask cloning preserves content with independent storage");
}

void domain_values()
{
    const std::array types{DataType::image(), DataType::raster_mask(), DataType::point2d(), DataType::line2d(),
        DataType::circle2d(), DataType::pose2d(), DataType::transform2d(), DataType::region2d(), DataType::resource_reference()};
    const std::array kinds{TypeKind::Image, TypeKind::RasterMask, TypeKind::Point2D, TypeKind::Line2D,
        TypeKind::Circle2D, TypeKind::Pose2D, TypeKind::Transform2D, TypeKind::Region2D, TypeKind::ResourceReference};
    for (std::size_t left = 0; left < types.size(); ++left) {
        check(types[left].kind() == kinds[left], "domain DataType factory retains its kind");
        for (std::size_t right = left + 1; right < types.size(); ++right) {
            check(types[left] != types[right], "different domain kinds have different types");
        }
    }
    retained_schema_aliases();
    retained_container_aliases<ConfigValue>();
    retained_container_aliases<DataValue>();
    geometry_values<ConfigValue>();
    geometry_values<DataValue>();
    persistent_values();
    runtime_values();
}

NodeStateDefinition state(DataType schema, ConfigValue initial)
{
    return {std::move(schema), std::move(initial), StateScope::SolutionSession,
        StateResetPolicy::DomainStartAndExplicit, StateCompatibilityPolicy::PreserveCompatible,
        StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
}

ToolDescriptor tool()
{
    return {key<ToolTypeId>("vision.domain"), "Domain tool", 1,
        {key<SchemaId>("domain.tool"), 1, {}, true}, {}, {},
        {{key<OutputId>("image"), "Image", DataType::image()}}, {},
        state(DataType::integer(), ConfigValue::integer(0)),
        {{key<SchemaId>("mask"), ResourceKind::Mask, true}},
        RegionCapabilities{{RegionShapeKind::Polygon, RegionShapeKind::RasterMask},
            {RegionBoundaryPolicy::Reject, RegionBoundaryPolicy::Intersect},
            GeometryMaskPolicy::Combine, false, false, true, {PixelDepth::UInt8}},
        {{PixelDepth::UInt8, ChannelOrder::Mono}, {PixelDepth::UInt8, ChannelOrder::RGB}}};
}

void descriptor_error(const ToolDescriptor& descriptor, ErrorCode expected)
{
    const auto failure = error(validate_descriptor(descriptor), expected);
    check(failure.location.tool == descriptor.id && failure.location.schema == descriptor.schema.id,
        "invalid domain metadata includes tool and tool Schema context");
}

void domain_metadata()
{
    const auto valid = tool();
    check(std::holds_alternative<std::monostate>(validate_descriptor(valid)),
        "tool metadata integrates state, resource slots, region capabilities and image formats");
    auto changed = valid;
    changed.resources.push_back(changed.resources.front());
    descriptor_error(changed, ErrorCode::InvalidDescriptor);
    changed = valid;
    changed.resources.front().kind = static_cast<ResourceKind>(-1);
    descriptor_error(changed, ErrorCode::InvalidResource);
    changed = valid;
    changed.image_formats.push_back(changed.image_formats.front());
    descriptor_error(changed, ErrorCode::InvalidDescriptor);
    changed = valid;
    changed.image_formats.front().depth = static_cast<PixelDepth>(-1);
    descriptor_error(changed, ErrorCode::UnsupportedPixelFormat);
    changed = valid;
    changed.region_capabilities->shapes.push_back(RegionShapeKind::Polygon);
    descriptor_error(changed, ErrorCode::UnsupportedRegion);
    changed = valid;
    changed.state->initial_value = ConfigValue::boolean(false);
    descriptor_error(changed, ErrorCode::InvalidState);
    changed = valid;
    changed.state->concurrency = StateConcurrencyPolicy::IsolatedPerInvocation;
    descriptor_error(changed, ErrorCode::InvalidState);

    const auto rgb = image({PixelDepth::UInt8, ChannelOrder::RGB});
    const auto bgr = image({PixelDepth::UInt8, ChannelOrder::BGR});
    check(std::holds_alternative<std::monostate>(validate_image_support(valid, rgb)),
        "an explicitly declared RGB format is accepted");
    const auto unsupported = error(validate_image_support(valid, bgr), ErrorCode::UnsupportedPixelFormat);
    check(unsupported.location.tool == valid.id, "unsupported image format identifies the tool");
    error(validate_image_support(valid, image({PixelDepth::UInt16, ChannelOrder::Mono})), ErrorCode::UnsupportedPixelFormat);
    changed = valid;
    changed.image_formats.clear();
    error(validate_image_support(changed, rgb), ErrorCode::UnsupportedPixelFormat);
    changed.image_formats.push_back({PixelDepth::UInt8, ChannelOrder::BGR});
    check(std::holds_alternative<std::monostate>(validate_image_support(changed, bgr)),
        "an explicit BGR declaration is accepted independently of RGB");
    error(validate_image_support(changed, rgb), ErrorCode::UnsupportedPixelFormat);
    changed = valid;
    changed.state->initial_value = ConfigValue::boolean(false);
    error(validate_image_support(changed, rgb), ErrorCode::InvalidState);

    const auto payload_field = key<FieldId>("payload");
    const auto nested_schema_id = key<SchemaId>("domain.state.payload");
    const auto state_record = take(DataType::record(nested_schema_id, 1, {{payload_field, DataType::integer()}}));
    const auto conflict_record = take(DataType::record(nested_schema_id, 1, {{payload_field, DataType::string()}}));
    const auto initial = ConfigValue::some(take(ConfigValue::list(state_record, {})));
    changed = valid;
    changed.state = state(initial.type(), initial);
    changed.outputs.push_back({key<OutputId>("state"), "State", DataType::list(DataType::optional(state_record))});
    check(std::holds_alternative<std::monostate>(validate_descriptor(changed)),
        "state and output may reuse a consistent nested Schema through Optional/List");
    changed.outputs.back().type = DataType::list(DataType::optional(conflict_record));
    const auto conflict = error(validate_descriptor(changed), ErrorCode::InvalidSchema);
    check(conflict.location.tool == changed.id && conflict.location.schema == nested_schema_id
        && conflict.location.output == changed.outputs.back().id,
        "state Schema traversal catches a conflicting nested output definition with context");
    changed = valid;
    const auto uninitialized_image = ConfigValue::none(DataType::image());
    changed.state = state(uninitialized_image.type(), uninitialized_image);
    check(std::holds_alternative<std::monostate>(validate_descriptor(changed)),
        "an optional runtime-image state can start with a persistable typed None");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "domain_values") { domain_values(); }
        else if (group == "domain_metadata") { domain_metadata(); }
        else { return 2; }
    } catch (const std::exception& failure) {
        std::cerr << "Unexpected exception: " << failure.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Unexpected non-standard exception\n";
        return 1;
    }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
