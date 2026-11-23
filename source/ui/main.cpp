#include "canvas_example.hpp"
#include "editor_window.hpp"
#include "workspace_style.hpp"

#include <tool_catalog.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QMessageBox>

#include <stdexcept>

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    application.setFont(visionpro::ui::workspace_font());
    application.setApplicationName(QStringLiteral("VisionPro"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("solution"), QStringLiteral("要打开的方案目录或 solution.json"), QStringLiteral("[solution]"));
    parser.process(application);
    try {
        auto registered = visionpro::app::make_tool_registry();
        if (const auto* failure = std::get_if<visionpro::core::Diagnostic>(&registered)) {
            throw std::runtime_error(failure->message);
        }
        visionpro::ui::EditorWindow window(visionpro::ui::canvas_example(),
            std::get<visionpro::core::ToolRegistry>(std::move(registered)));
        window.show();
        const auto paths = parser.positionalArguments();
        if (paths.size() > 1) { throw std::invalid_argument("Specify a single solution directory or solution.json."); }
        if (!paths.isEmpty()) {
            const QFileInfo file(paths.front());
            const auto directory = file.isFile() && file.fileName() == QStringLiteral("solution.json")
                ? file.absolutePath() : file.absoluteFilePath();
            window.open_document(std::filesystem::path(directory.toStdU16String()));
        }
        return application.exec();
    } catch (const std::exception& failure) {
        QMessageBox::critical(nullptr, QStringLiteral("VisionPro"), QString::fromUtf8(failure.what()));
        return 1;
    }
}
