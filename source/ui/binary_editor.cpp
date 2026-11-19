#include "binary_editor.hpp"
#include <visionpro/core/text.hpp>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace visionpro::ui {
namespace {
QString hex_bytes(std::span<const std::uint8_t> bytes, bool wrap = false)
{
    if (bytes.empty()) { return {}; }
    const auto hex = core::encode_hex(bytes);
    QByteArray formatted;
    formatted.reserve(static_cast<qsizetype>(bytes.size() * 3 - 1));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) { formatted.append(wrap && i % 16 == 0 ? '\n' : ' '); }
        formatted.append(hex.data() + 2 * i, 2);
    }
    return QString::fromLatin1(formatted.toUpper());
}
}

QString binary_summary(std::span<const std::uint8_t> bytes)
{
    auto summary = QStringLiteral("Binary (%1 bytes)").arg(bytes.size());
    if (bytes.empty()) { return summary; }
    constexpr std::size_t preview = 32;
    summary += QStringLiteral(" · ") + hex_bytes(bytes.first(std::min(bytes.size(), preview)));
    if (bytes.size() > preview) { summary += QStringLiteral(" …"); }
    return summary;
}

core::Result<std::vector<std::uint8_t>> parse_hex_bytes(QStringView text)
{
    std::string hex;
    hex.reserve(static_cast<std::size_t>(text.size()));
    for (const auto c : text) {
        if (c.isSpace() && hex.size() % 2 == 0) { continue; }
        hex.push_back(c.toLatin1());
    }
    if (auto bytes = core::decode_hex(hex)) { return std::move(*bytes); }
    return core::Diagnostic{core::ErrorCode::InvalidValue, core::Severity::Error, {},
        "Use two hexadecimal digits per byte, with optional spaces between bytes."};
}

std::optional<core::ConfigValue> edit_binary(QWidget* parent, const core::ConfigValue* previous)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Binary constant"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Hexadecimal bytes (for example, 41 00 42)"), &dialog));
    auto* editor = new QPlainTextEdit(&dialog);
    editor->setObjectName(QStringLiteral("binaryHexEditor"));
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    if (previous && previous->as_binary()) { editor->setPlainText(hex_bytes(*previous->as_binary(), true)); }
    layout->addWidget(editor);
    auto* error = new QLabel(&dialog);
    error->setObjectName(QStringLiteral("binaryHexError"));
    layout->addWidget(error);
    error->hide();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    std::optional<core::ConfigValue> value;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto parsed = parse_hex_bytes(editor->toPlainText());
        if (const auto* issue = std::get_if<core::Diagnostic>(&parsed)) {
            error->setText(QString::fromStdString(issue->message));
            error->show();
            return;
        }
        value = core::ConfigValue::binary(std::get<std::vector<std::uint8_t>>(parsed));
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
    return value;
}

void show_binary(QWidget* parent, core::DataValue value)
{
    const auto* buffer = value.as_binary();
    if (!buffer) { return; }
    const std::span<const std::uint8_t> bytes{*buffer};
    constexpr std::size_t page_size = 256;
    std::size_t offset = 0;
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Binary output"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* row = new QHBoxLayout;
    auto* position = new QLabel(&dialog);
    auto* previous = new QPushButton(QStringLiteral("Previous"), &dialog);
    auto* next = new QPushButton(QStringLiteral("Next"), &dialog);
    row->addWidget(position, 1);
    row->addWidget(previous);
    row->addWidget(next);
    layout->addLayout(row);
    auto* editor = new QPlainTextEdit(&dialog);
    editor->setObjectName(QStringLiteral("binaryHexOutput"));
    editor->setReadOnly(true);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(editor);
    const auto refresh = [&] {
        const auto count = std::min(page_size, bytes.size() - offset);
        position->setText(bytes.empty() ? QStringLiteral("0 bytes")
            : QStringLiteral("Bytes %1–%2 of %3 (offsets start at 0)").arg(offset).arg(offset + count - 1).arg(bytes.size()));
        previous->setEnabled(offset > 0);
        next->setEnabled(offset + count < bytes.size());
        editor->setPlainText(hex_bytes(bytes.subspan(offset, count), true));
    };
    QObject::connect(previous, &QPushButton::clicked, &dialog, [&] { offset -= page_size; refresh(); });
    QObject::connect(next, &QPushButton::clicked, &dialog, [&] { offset += page_size; refresh(); });
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(close);
    refresh();
    dialog.resize(540, 430);
    dialog.exec();
}

} // namespace visionpro::ui
