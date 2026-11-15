#include "roi_mask.hpp"

#include "opencv_image.hpp"

#include <visionpro/infrastructure/file_resources.hpp>

#include <cmath>

namespace visionpro::tools::vision {
namespace {
using namespace core;

constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};

Diagnostic error(ErrorCode code, std::string message, std::optional<ResourceId> resource = {})
{
    DiagnosticLocation location;
    location.resource = resource;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

bool within(Vector2D point, std::size_t width, std::size_t height)
{
    return point.x >= 0.0 && point.x < static_cast<double>(width)
        && point.y >= 0.0 && point.y < static_cast<double>(height);
}

Result<std::optional<Transform2D>> sampling_transform(const MaskConfig& config)
{
    const auto& mapping = config.mapping;
    const auto& transform = mapping.transform;
    if (mapping.sampling == MaskSampling::Linear || (transform
        && ((transform->model != TransformModel::Affine && transform->model != TransformModel::Similarity)
            || transform->valid_domain || transform->calibration))) {
        return error(ErrorCode::UnsupportedRegion,
            "Mask sampling requires direct mapping or Nearest with an unbounded, uncalibrated Similarity or Affine transform.");
    }
    if (!transform) { return std::optional<Transform2D>{}; }
    auto inverse = inverse_transform(*transform);
    if (auto* failure = std::get_if<Diagnostic>(&inverse)) { return std::move(*failure); }
    return std::optional{std::get<Transform2D>(std::move(inverse))};
}

} // namespace

PreparedRoiMask::PreparedRoiMask(core::ImageHandle pixels, std::size_t target_width, std::size_t target_height,
    std::optional<core::Transform2D> inverse, core::MaskValueRule rule)
    : pixels_(std::move(pixels)), target_width_(target_width), target_height_(target_height),
    inverse_(std::move(inverse)), rule_(rule) {}

bool PreparedRoiMask::contains(core::Vector2D point) const
{
    if (!within(point, target_width_, target_height_)) { return false; }
    if (inverse_) {
        const auto& m = inverse_->matrix;
        point = {std::fma(m[0], point.x, std::fma(m[1], point.y, m[2])),
            std::fma(m[3], point.x, std::fma(m[4], point.y, m[5]))};
    }
    const Vector2D sample{std::floor(point.x + 0.5), std::floor(point.y + 0.5)};
    if (!within(sample, pixels_.width(), pixels_.height())) { return false; }
    const auto row = std::get<std::span<const std::byte>>(pixels_.row(static_cast<std::size_t>(sample.y)));
    const auto value = std::to_integer<unsigned int>(row[static_cast<std::size_t>(sample.x)]);
    return rule_.mode == MaskValueMode::NonZero ? value != 0 : value >= rule_.threshold;
}

RoiMaskResource::RoiMaskResource(std::span<const core::InvocationResourceLease> leases)
{
    for (const auto& binding : leases) {
        if (binding.slot.text() != "mask") {
            if (binding.lease && binding.lease->reference().kind == ResourceKind::Mask) {
                slot_failure_ = error(ErrorCode::InvalidResource, "A mask lease is only accepted in the mask slot.",
                    binding.lease->reference().id);
                return;
            }
            continue;
        }
        if (lease_) {
            slot_failure_ = error(ErrorCode::InvalidResource, "The mask slot must have a single lease.",
                binding.lease ? binding.lease->reference().id : lease_->reference().id);
            return;
        }
        if (!binding.lease) {
            slot_failure_ = error(ErrorCode::InvalidResource, "The mask slot requires an owning resource lease.");
            return;
        }
        lease_ = binding.lease;
    }
}

core::Result<PreparedRoiMask> RoiMaskResource::prepare(const core::MaskConfig& config)
{
    const auto locate = [&](Diagnostic failure) {
        if (!failure.location.resource) { failure.location.resource = config.resource.id; }
        return failure;
    };
    if (slot_failure_) { return locate(*slot_failure_); }
    if (auto checked = validate_mask_config(config); auto* failure = std::get_if<Diagnostic>(&checked)) {
        return locate(std::move(*failure));
    }
    if (config.format != mono8) {
        return locate(error(ErrorCode::UnsupportedPixelFormat, "ROI mask pixels must be UInt8 Mono."));
    }
    if (!lease_) {
        return locate(error(ErrorCode::MissingRequiredValue, "Configured ROI masks require a lease in the mask slot."));
    }
    const auto& fixed = lease_->reference();
    if (fixed.kind != ResourceKind::Mask) {
        return locate(error(ErrorCode::ResourceKindMismatch, "The mask slot must provide a Mask resource."));
    }
    if (fixed.id != config.resource.id) {
        return locate(error(ErrorCode::InvalidResource, "The configured ROI mask does not match the mask slot's resource identity."));
    }
    if (config.resource.revision && config.resource.revision != fixed.revision) {
        return locate(error(ErrorCode::ResourceVersionMismatch, "The configured ROI mask revision does not match the mask slot."));
    }
    auto inverse = sampling_transform(config);
    if (auto* failure = std::get_if<Diagnostic>(&inverse)) { return locate(std::move(*failure)); }
    if (!decoded_) {
        const auto file = std::dynamic_pointer_cast<const infrastructure::FileResourceLease>(lease_);
        if (!file) { return locate(error(ErrorCode::InvalidResource, "The mask slot requires a file resource lease.")); }
        auto matrix = decode_image_bytes(file->bytes());
        if (auto* failure = std::get_if<Diagnostic>(&matrix)) {
            if (failure->code == ErrorCode::InvalidImage) { failure->code = ErrorCode::InvalidMask; }
            return locate(std::move(*failure));
        }
        auto image = copy_from_cv_mat(std::get<cv::Mat>(matrix), mono8, config.mask_frame);
        if (auto* failure = std::get_if<Diagnostic>(&image)) { return locate(std::move(*failure)); }
        decoded_ = std::get<ImageHandle>(std::move(image));
    }
    if (decoded_->width() != config.width || decoded_->height() != config.height) {
        return locate(error(ErrorCode::InvalidMask, "Decoded mask dimensions differ from their declaration."));
    }
    used_ = true;
    return PreparedRoiMask{*decoded_, config.mapping.target_width, config.mapping.target_height,
        std::get<std::optional<Transform2D>>(std::move(inverse)), config.valid_pixels};
}

core::Result<std::monostate> RoiMaskResource::finish() const
{
    if (slot_failure_) { return *slot_failure_; }
    if (lease_ && !used_) {
        return error(ErrorCode::InvalidResource, "The mask slot has a lease unused by the evaluated ROI.", lease_->reference().id);
    }
    return std::monostate{};
}

std::optional<core::ResourceReference> RoiMaskResource::reference() const
{
    if (!used_) { return {}; }
    return lease_->reference();
}

} // namespace visionpro::tools::vision
