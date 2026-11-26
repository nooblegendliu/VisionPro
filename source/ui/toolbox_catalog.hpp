#pragma once

#include <QIcon>
#include <QString>

#include <string_view>

namespace visionpro::ui {

enum class ToolboxCategory {
    Acquisition, Location, Measurement, Recognition, Defect, EdgeLearning, DeepLearning,
    Calibration, Calculation, ImageProcessing, Color, SplitCombine, Geometry, Logic,
    Communication, Count
};

struct ToolboxEntry {
    ToolboxCategory category;
    QString name;
    QIcon icon;
};

QString toolbox_category_name(ToolboxCategory category);
QString display_field(const QString& declared);
QString camera_diagnostic_name(std::string_view output);
ToolboxEntry toolbox_entry(std::string_view type, const QString& original);

} // namespace visionpro::ui
