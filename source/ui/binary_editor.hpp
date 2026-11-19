#pragma once

#include <visionpro/core/value.hpp>

#include <QString>

class QWidget;

namespace visionpro::ui {

QString binary_summary(std::span<const std::uint8_t> bytes);
core::Result<std::vector<std::uint8_t>> parse_hex_bytes(QStringView text);
std::optional<core::ConfigValue> edit_binary(QWidget* parent, const core::ConfigValue* previous);
void show_binary(QWidget* parent, core::DataValue value);

} // namespace visionpro::ui
