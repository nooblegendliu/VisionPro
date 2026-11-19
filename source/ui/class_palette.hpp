#pragma once

#include <visionpro/workflow/model.hpp>
#include <QColor>

class QWidget;

namespace visionpro::ui {

[[nodiscard]] QColor class_color(const workflow::ClassPalette& palette, std::uint8_t id);
[[nodiscard]] std::optional<workflow::ClassPalette> edit_class_color(QWidget* owner, const workflow::ClassPalette& existing);

} // namespace visionpro::ui
