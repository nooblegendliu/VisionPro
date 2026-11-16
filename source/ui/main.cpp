#include "canvas_example.hpp"
#include "editor_window.hpp"

#include <tool_catalog.hpp>

#include <QApplication>
#include <QMessageBox>

#include <stdexcept>

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("VisionPro"));
    try {
        auto registered = visionpro::app::make_tool_registry();
        if (const auto* failure = std::get_if<visionpro::core::Diagnostic>(&registered)) {
            throw std::runtime_error(failure->message);
        }
        visionpro::ui::EditorWindow window(visionpro::ui::canvas_example(),
            std::get<visionpro::core::ToolRegistry>(std::move(registered)));
        window.show();
        return application.exec();
    } catch (const std::exception& failure) {
        QMessageBox::critical(nullptr, QStringLiteral("VisionPro"), QString::fromUtf8(failure.what()));
        return 1;
    }
}
