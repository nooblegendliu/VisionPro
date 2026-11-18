#include "run_inputs.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace visionpro::ui {
namespace {
using namespace core;

#ifdef VISIONPRO_WITH_OPENCV
constexpr bool image_decoder_available = true;
#else
constexpr bool image_decoder_available = false;
#endif

struct InputControls {
    InputDescriptor descriptor;
    QComboBox* mode{};
    QWidget* value{};
    QComboBox* frame{};
    QComboBox* decode{};
    std::vector<CoordinateSystemId> frames;
    QString unavailable;
};

QString text(std::string_view bytes)
{ return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size())); }

QString type_label(TypeKind kind)
{
    switch (kind) {
    case TypeKind::Bool: return QStringLiteral("Bool");
    case TypeKind::Int64: return QStringLiteral("Int64");
    case TypeKind::Float64: return QStringLiteral("Float64");
    case TypeKind::String: return QStringLiteral("String");
    case TypeKind::Image: return QStringLiteral("Image");
    default: return {};
    }
}

QString default_label(const ConfigValue& value)
{
    QString shown;
    if (const auto* flag = value.as_bool()) { shown = *flag ? QStringLiteral("true") : QStringLiteral("false"); }
    else if (const auto* integer = value.as_int64()) { shown = QString::number(*integer); }
    else if (const auto* real = value.as_float64()) { shown = QString::number(*real, 'g', 17); }
    else if (const auto* string = value.as_string()) {
        shown = string->empty() ? QStringLiteral("empty string") : text(*string).simplified();
        if (shown.size() > 48) { shown = shown.left(48) + QStringLiteral("…"); }
    }
    return shown.isEmpty() ? QStringLiteral("Use declared default") : QStringLiteral("Use default (%1)").arg(shown);
}

void image_controls(InputControls& field, QWidget* owner, QVBoxLayout& layout, const workflow::Solution& solution,
    std::span<const PixelFormat> formats)
{
    if (!image_decoder_available) {
        field.unavailable = QStringLiteral("File images require an OpenCV build.");
        return;
    }
    const auto& semantics = field.descriptor.semantics;
    if (semantics.unit == MeasurementUnit::Unspecified || semantics.unit == MeasurementUnit::Pixel) {
        for (const auto& frame : solution.coordinates) {
            if (frame.kind == CoordinateKind::Image && frame.unit == MeasurementUnit::Pixel
                && (!semantics.coordinate_system || frame.id == *semantics.coordinate_system)) {
                field.frames.push_back(frame.id);
            }
        }
    }
    if (field.frames.empty()) {
        field.unavailable = QStringLiteral("A compatible pixel coordinate frame must be declared in the solution.");
        return;
    }
    const bool mono = formats.empty() || std::ranges::find(formats, PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}) != formats.end();
    const bool bgr = formats.empty() || std::ranges::find(formats, PixelFormat{PixelDepth::UInt8, ChannelOrder::BGR}) != formats.end();
    if (!mono && !bgr) {
        field.unavailable = QStringLiteral("This input requires an image format unavailable in the file editor.");
        return;
    }

    const auto id = text(field.descriptor.id.text());
    auto* file = new QLineEdit(owner);
    file->setObjectName(QStringLiteral("runInputFile.") + id);
    file->setPlaceholderText(QStringLiteral("Absolute path to an image"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), owner);
    browse->setObjectName(QStringLiteral("runInputBrowse.") + id);
    QObject::connect(browse, &QPushButton::clicked, owner, [owner, file] {
        const auto path = QFileDialog::getOpenFileName(owner, QStringLiteral("Open Input Image"), file->text(),
            QStringLiteral("Images (*.bmp *.png *.jpg *.jpeg *.tif *.tiff *.pgm *.ppm);;All files (*)"));
        if (!path.isEmpty()) { file->setText(path); }
    });
    auto* path_row = new QHBoxLayout;
    path_row->addWidget(file, 1);
    path_row->addWidget(browse);
    layout.addLayout(path_row);
    field.value = file;

    auto* options = new QFormLayout;
    field.frame = new QComboBox(owner);
    field.frame->setObjectName(QStringLiteral("runInputFrame.") + id);
    for (const auto& frame : field.frames) { field.frame->addItem(text(frame.text())); }
    options->addRow(QStringLiteral("Pixel frame"), field.frame);
    field.decode = new QComboBox(owner);
    field.decode->setObjectName(QStringLiteral("runInputDecode.") + id);
    if (bgr) {
        field.decode->addItem(mono ? QStringLiteral("Native UInt8 (gray or BGR)")
                                  : QStringLiteral("Native UInt8 (BGR required)"), false);
    }
    if (mono) { field.decode->addItem(QStringLiteral("Grayscale UInt8"), true); }
    options->addRow(QStringLiteral("Decode"), field.decode);
    layout.addLayout(options);
}

InputControls add_input(QWidget* parent, QFormLayout& form, const workflow::Solution& solution,
    const InputDescriptor& input, std::span<const PixelFormat> formats)
{
    InputControls field{.descriptor = input};
    auto* row = new QWidget(parent);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* entry = new QWidget(row);
    auto* editors = new QVBoxLayout(entry);
    editors->setContentsMargins(0, 0, 0, 0);

    const auto kind = input.type.kind();
    const auto name = QStringLiteral("runInputValue.") + text(input.id.text());
    if (kind == TypeKind::Image) {
        image_controls(field, entry, *editors, solution, formats);
    } else if (kind == TypeKind::Bool) {
        auto* choice = new QComboBox(entry);
        choice->setPlaceholderText(QStringLiteral("Select a value"));
        choice->addItem(QStringLiteral("False"), false);
        choice->addItem(QStringLiteral("True"), true);
        choice->setCurrentIndex(-1);
        field.value = choice;
    } else if (kind == TypeKind::Int64 || kind == TypeKind::Float64) {
        field.value = new QLineEdit(entry);
    } else if (kind == TypeKind::String) {
        auto* string = new QPlainTextEdit(entry);
        string->setMaximumHeight(76);
        field.value = string;
    } else {
        field.unavailable = QStringLiteral("This input type has no one-time value editor.");
    }
    if (field.value && !field.frame) {
        field.value->setObjectName(name);
        editors->addWidget(field.value);
    }
    if (!field.unavailable.isEmpty()) {
        auto* notice = new QLabel(field.unavailable, entry);
        notice->setWordWrap(true);
        editors->addWidget(notice);
    }
    if (!input.required && input.default_value && field.value) {
        field.mode = new QComboBox(row);
        field.mode->setObjectName(QStringLiteral("runInputMode.") + text(input.id.text()));
        field.mode->addItem(default_label(*input.default_value));
        field.mode->addItem(QStringLiteral("Provide a value"));
        layout->addWidget(field.mode);
        entry->setEnabled(false);
        QObject::connect(field.mode, &QComboBox::currentIndexChanged, entry,
            [entry](int index) { entry->setEnabled(index == 1); });
    } else if (!input.required && input.default_value) {
        auto* label = new QLabel(default_label(*input.default_value), row);
        label->setTextFormat(Qt::PlainText);
        layout->addWidget(label);
    }
    layout->addWidget(entry);

    const auto type = type_label(kind);
    auto* label = new QLabel(type.isEmpty() ? text(input.display_name) :
        QStringLiteral("%1 (%2)").arg(text(input.display_name), type), row);
    label->setTextFormat(Qt::PlainText);
    label->setToolTip(text(input.id.text()));
    label->setBuddy(field.mode ? static_cast<QWidget*>(field.mode) : field.value);
    form.addRow(label, row);
    return field;
}

Diagnostic invalid_input(std::string message)
{ return {ErrorCode::InvalidValue, Severity::Error, {}, std::move(message)}; }

template<class T, class ConfigFactory, class DataFactory>
Result<DataValue> checked_scalar(const InputDescriptor& input, T value, ConfigFactory configure, DataFactory materialize)
{
    Result<ConfigValue> configured = configure(value);
    if (const auto* failure = std::get_if<Diagnostic>(&configured)) { return *failure; }
    auto checked = validate_input_value(input, std::get<ConfigValue>(configured));
    if (const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    return materialize(value);
}

Result<DataValue> scalar_input(const InputControls& field)
{
    const auto& input = field.descriptor;
    switch (input.type.kind()) {
    case TypeKind::Bool: {
        auto* widget = static_cast<QComboBox*>(field.value);
        if (widget->currentIndex() < 0) { return invalid_input("Select a Bool value."); }
        return checked_scalar(input, widget->currentData().toBool(), ConfigValue::boolean, DataValue::boolean);
    }
    case TypeKind::Int64: {
        bool valid = false;
        const auto value = static_cast<QLineEdit*>(field.value)->text().toLongLong(&valid, 10);
        if (!valid) { return invalid_input("Enter a valid Int64 value."); }
        return checked_scalar(input, value, ConfigValue::integer, DataValue::integer);
    }
    case TypeKind::Float64: {
        bool valid = false;
        const auto value = static_cast<QLineEdit*>(field.value)->text().toDouble(&valid);
        if (!valid) { return invalid_input("Enter a valid Float64 value."); }
        return checked_scalar(input, value, ConfigValue::real, DataValue::real);
    }
    case TypeKind::String: {
        auto value = static_cast<QPlainTextEdit*>(field.value)->document()->toRawText();
        value.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        return checked_scalar(input, value.toStdString(), ConfigValue::string, DataValue::string);
    }
    default:
        return invalid_input("This input type has no scalar editor.");
    }
}

QString collect_input(const InputControls& field, EnteredRunInputs& result)
{
    const auto& input = field.descriptor;
    if (!input.required && input.default_value && (!field.mode || field.mode->currentIndex() == 0)) { return {}; }
    if (!field.unavailable.isEmpty()) { return field.unavailable; }
    if (input.type.kind() == TypeKind::Image) {
        const std::filesystem::path file{static_cast<QLineEdit*>(field.value)->text().toStdU16String()};
        if (!file.is_absolute()) { return QStringLiteral("Choose an image using an absolute file path."); }
        const auto index = field.frame->currentIndex();
        if (index < 0 || field.decode->currentIndex() < 0) { return QStringLiteral("Select a pixel frame and decode mode."); }
        result.images.push_back({input.id, file, field.frames[static_cast<std::size_t>(index)],
            field.decode->currentData().toBool()});
    } else {
        auto value = scalar_input(field);
        if (const auto* failure = std::get_if<Diagnostic>(&value)) { return text(failure->message); }
        result.values.push_back({input.id, std::get<DataValue>(std::move(value))});
    }
    return {};
}
} // namespace

std::optional<EnteredRunInputs> choose_run_inputs(QWidget* parent, const workflow::Solution& solution,
    std::span<const core::InputDescriptor> inputs, bool isolated, std::span<const core::PixelFormat> image_formats, bool group)
{
    if (inputs.empty()) { return EnteredRunInputs{}; }
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("runInputsDialog"));
    dialog.setWindowTitle(group ? QStringLiteral("Group Inputs") : isolated ? QStringLiteral("One-time Node Inputs") : QStringLiteral("Workflow Inputs"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* contents = new QWidget(scroll);
    auto* form = new QFormLayout(contents);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(contents);
    layout->addWidget(scroll);

    std::vector<InputControls> fields;
    fields.reserve(inputs.size());
    for (const auto& input : inputs) { fields.push_back(add_input(contents, *form, solution, input, image_formats)); }
    auto* error = new QLabel(&dialog);
    error->setObjectName(QStringLiteral("runInputError"));
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("runInputButtons"));
    buttons->button(QDialogButtonBox::Ok)->setText(isolated ? QStringLiteral("Debug") : QStringLiteral("Run"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    std::optional<EnteredRunInputs> result;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            EnteredRunInputs entered;
            for (const auto& field : fields) {
                const auto issue = collect_input(field, entered);
                if (!issue.isEmpty()) {
                    error->setText(QStringLiteral("%1 — %2").arg(text(field.descriptor.display_name), issue));
                    error->show();
                    if (field.value) {
                        scroll->ensureWidgetVisible(field.value);
                        field.value->setFocus();
                    }
                    return;
                }
            }
            result = std::move(entered);
            dialog.accept();
        } catch (const std::exception& failure) {
            error->setText(text(failure.what()));
            error->show();
        } catch (...) {
            error->setText(QStringLiteral("Could not collect the run inputs."));
            error->show();
        }
    });
    dialog.resize(540, 360);
    dialog.exec();
    return result;
}

} // namespace visionpro::ui
