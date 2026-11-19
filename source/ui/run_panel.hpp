#pragma once

#include <visionpro/workflow/run_handle.hpp>

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace visionpro::ui {

QString shared_value_text(const core::DataValue& value);
QString shared_value_text(const core::ConfigValue& value);

class RunPanel final : public QWidget {
    Q_OBJECT

public:
    explicit RunPanel(QWidget* parent = nullptr);

    void update_runs(std::span<const workflow::RunHandle> runs, std::uint64_t dropped);
    void clear();

    [[nodiscard]] std::optional<workflow::RunHandle> selected_run() const;
    [[nodiscard]] std::optional<core::InvocationKey> selected_invocation() const;
    [[nodiscard]] std::optional<std::int64_t> selected_iteration_index() const;
    [[nodiscard]] QString selected_scope_path() const;
    [[nodiscard]] std::optional<core::OutputId> selected_output() const;

signals:
    void image_requested();

private:
    void rebuild_history();
    void select_history(int row);
    void refresh_run();
    void refresh_call();
    void refresh_image_button();
    void reset_display();
    void show_details(const QString& text);
    void show_refresh_failure();
    const workflow::RunNodeProgress* selected_node() const;

    QComboBox* history_{};
    QLabel* identity_{};
    QLabel* status_{};
    QLabel* completion_{};
    QLabel* cleanup_{};
    QLabel* notifications_{};
    QTreeWidget* nodes_{};
    QLabel* judgement_{};
    QComboBox* outputs_{};
    QPushButton* show_image_{};
    QPlainTextEdit* details_{};

    std::vector<workflow::RunHandle> runs_;
    std::optional<workflow::RunHandle> pinned_run_;
    std::optional<workflow::RunIdentity> displayed_run_;
    std::optional<workflow::RunProgress> progress_;
    std::optional<workflow::NodeResult> displayed_result_;
    QString details_text_;
};

} // namespace visionpro::ui
