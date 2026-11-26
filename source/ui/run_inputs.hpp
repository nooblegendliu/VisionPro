#pragma once

#include <visionpro/workflow/run.hpp>

#include <filesystem>

class QWidget;

namespace visionpro::ui {

struct RunImageInput {
    core::InputId id;
    std::filesystem::path file;
    core::CoordinateSystemId frame;
    bool grayscale;
};

struct EnteredRunInputs {
    std::vector<workflow::RunInput> values;
    std::vector<RunImageInput> images;
};

[[nodiscard]] std::optional<EnteredRunInputs> choose_run_inputs(QWidget* parent, const workflow::Solution& solution,
    std::span<const core::InputDescriptor> inputs, bool isolated, std::span<const core::PixelFormat> image_formats = {},
    bool group = false, bool sdk_sample = false);

} // namespace visionpro::ui
