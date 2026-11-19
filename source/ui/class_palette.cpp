#include "class_palette.hpp"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>

namespace visionpro::ui {

QColor class_color(const workflow::ClassPalette& palette, std::uint8_t id)
{
    const auto found = palette.find(id);
    if (found == palette.end()) {
        return id == 0 ? QColor(30, 34, 40) : QColor::fromHsv((191 + 137 * (id - 1)) % 360, 174, 223);
    }
    const auto& color = found->second;
    return {color.red, color.green, color.blue};
}

std::optional<workflow::ClassPalette> edit_class_color(QWidget* owner, const workflow::ClassPalette& existing)
{
    QDialog dialog(owner);
    dialog.setWindowTitle(QStringLiteral("Edit class color"));
    auto* form = new QFormLayout(&dialog);
    auto* id = new QSpinBox(&dialog);
    id->setObjectName(QStringLiteral("classColorId"));
    id->setRange(0, 255);
    form->addRow(QStringLiteral("Class ID"), id);
    auto* row = new QHBoxLayout;
    auto* rgb = new QLineEdit(&dialog);
    rgb->setObjectName(QStringLiteral("classColorRgb"));
    rgb->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}")), rgb));
    row->addWidget(rgb);
    auto* choose = new QPushButton(QStringLiteral("Choose…"), &dialog);
    row->addWidget(choose);
    form->addRow(QStringLiteral("RGB (#RRGGBB)"), row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, &dialog);
    form->addRow(buttons);
    const auto refresh = [&] { rgb->setText(class_color(existing, static_cast<std::uint8_t>(id->value())).name()); };
    QObject::connect(id, &QSpinBox::valueChanged, &dialog, refresh);
    QObject::connect(rgb, &QLineEdit::textChanged, &dialog, [&] { buttons->button(QDialogButtonBox::Ok)->setEnabled(rgb->hasAcceptableInput()); });
    QObject::connect(choose, &QPushButton::clicked, &dialog, [&] {
        const auto picked = QColorDialog::getColor(QColor::fromString(rgb->text()), &dialog);
        if (picked.isValid()) { rgb->setText(picked.name()); }
    });
    QObject::connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
        rgb->setText(class_color({}, static_cast<std::uint8_t>(id->value())).name());
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    refresh();
    if (dialog.exec() != QDialog::Accepted || !rgb->hasAcceptableInput()) { return {}; }
    const auto index = static_cast<std::uint8_t>(id->value());
    const auto color = QColor::fromString(rgb->text());
    auto changed = existing;
    if (color.rgb() == class_color({}, index).rgb()) { changed.erase(index); }
    else { changed[index] = {static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
        static_cast<std::uint8_t>(color.blue())}; }
    return changed;
}

} // namespace visionpro::ui
