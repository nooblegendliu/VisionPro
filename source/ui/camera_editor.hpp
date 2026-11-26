#pragma once

#include <visionpro/workflow/model.hpp>

#include <QGroupBox>

class QComboBox;
class QPushButton;
class QSpinBox;

namespace visionpro::ui {

bool is_camera_device(const core::ResourceDefinition& definition);
std::optional<core::ResourceDefinition> edit_camera_device(QWidget* parent, const core::ResourceDefinition* existing = nullptr);

class CameraTriggerEditor final : public QGroupBox {
public:
    CameraTriggerEditor(QWidget* parent, const workflow::Solution& solution, const workflow::Workflow& flow);
    core::Result<workflow::CameraTriggerBinding> binding() const;
    const core::ResourceDefinition* new_device() const;
    void retain_binding(const workflow::CameraTriggerBinding& current);
    void retain_image_input(core::InputId input);

private:
    QComboBox* devices_;
    QComboBox* inputs_;
    QComboBox* coordinates_;
    QComboBox* modes_;
    QSpinBox* timeout_;
    QPushButton* add_device_;
    std::vector<core::ResourceId> device_ids_;
    std::vector<core::InputId> input_ids_;
    std::vector<core::CoordinateSystemId> frame_ids_;
    std::optional<core::ResourceDefinition> created_;
};

} // namespace visionpro::ui
