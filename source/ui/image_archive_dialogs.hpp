#pragma once

#include <visionpro/workflow/model.hpp>
#include <QString>
#include <functional>

class QDialog;
class QFormLayout;
class QWidget;

namespace visionpro::core { struct ImageFtpSettings; }
namespace visionpro::ui {

void edit_archive_policy(QWidget* parent, const std::optional<workflow::ImageArchivePolicy>& current,
    const std::function<void(std::optional<workflow::ImageArchivePolicy>)>& apply);
QDialog* open_image_packages(QWidget* parent);
void image_ftp_controls(QWidget* owner, QFormLayout& form, core::ImageFtpSettings settings,
    const QString& default_credential, std::function<void(const std::string&)> select_credential);

} // namespace visionpro::ui
