#include "run_inputs.hpp"
#include "binary_editor.hpp"

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
#include <array>
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
    QComboBox* image_source{};
    std::vector<CoordinateSystemId> frames;
    QString unavailable;
};

QString text(std::string_view bytes)
{ return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size())); }

QString type_label(TypeKind kind)
{
    switch (kind) {
    case TypeKind::Bool: return QObject::tr("布尔");
    case TypeKind::Int64: return QStringLiteral("Int64");
    case TypeKind::Float64: return QStringLiteral("Float64");
    case TypeKind::String: return QObject::tr("字符串");
    case TypeKind::Binary: return QObject::tr("二进制");
    case TypeKind::Image: return QObject::tr("图像");
    default: return {};
    }
}

QString default_label(const ConfigValue& value)
{
    QString shown;
    if (const auto* flag = value.as_bool()) { shown = *flag ? QObject::tr("真") : QObject::tr("假"); }
    else if (const auto* integer = value.as_int64()) { shown = QString::number(*integer); }
    else if (const auto* real = value.as_float64()) { shown = QString::number(*real, 'g', 17); }
    else if (const auto* bytes = value.as_binary()) { shown = binary_summary(*bytes); }
    else if (const auto* string = value.as_string()) {
        shown = string->empty() ? QObject::tr("空字符串") : text(*string).simplified();
        if (shown.size() > 48) { shown = shown.left(48) + QStringLiteral("…"); }
    }
    return shown.isEmpty() ? QObject::tr("使用声明的默认值") : QObject::tr("使用默认值（%1）").arg(shown);
}

void image_controls(InputControls& field, QWidget* owner, QVBoxLayout& layout, const workflow::Solution& solution,
    std::span<const PixelFormat> formats, bool sdk_sample)
{
    if (!image_decoder_available && !sdk_sample) {
        field.unavailable = QObject::tr("从文件读取图像需要启用 OpenCV。");
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
        field.unavailable = QObject::tr("请先在方案中添加与输入匹配的像素坐标系。");
        return;
    }
    const bool mono = formats.empty() || std::ranges::find(formats, PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}) != formats.end();
    const bool bgr = formats.empty() || std::ranges::find(formats, PixelFormat{PixelDepth::UInt8, ChannelOrder::BGR}) != formats.end();
    if (!mono && !bgr) {
        field.unavailable = QObject::tr("文件输入不支持此端口要求的图像格式。");
        return;
    }

    const auto id = text(field.descriptor.id.text());
    if (sdk_sample) {
        field.image_source = new QComboBox(owner);
        field.image_source->setObjectName(QStringLiteral("sdkDebugImageSource"));
        if (image_decoder_available) { field.image_source->addItem(QObject::tr("从文件注入图像"), false); }
        field.image_source->addItem(QObject::tr("注入示例帧（2 × 2，Mono8）"), true);
        layout.addWidget(field.image_source);
    }
    auto* file = new QLineEdit(owner);
    file->setObjectName(QStringLiteral("runInputFile.") + id);
    file->setPlaceholderText(QObject::tr("图像文件的绝对路径"));
    auto* browse = new QPushButton(QObject::tr("浏览…"), owner);
    browse->setObjectName(QStringLiteral("runInputBrowse.") + id);
    QObject::connect(browse, &QPushButton::clicked, owner, [owner, file] {
        const auto path = QFileDialog::getOpenFileName(owner, QObject::tr("选择输入图像"), file->text(),
            QObject::tr("图像 (*.bmp *.png *.jpg *.jpeg *.tif *.tiff *.pgm *.ppm);;所有文件 (*)"));
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
    options->addRow(QObject::tr("像素坐标系"), field.frame);
    field.decode = new QComboBox(owner);
    field.decode->setObjectName(QStringLiteral("runInputDecode.") + id);
    if (bgr) {
        field.decode->addItem(mono ? QObject::tr("保留原格式（8 位灰度或 BGR）")
                                  : QObject::tr("保留原格式（限 8 位 BGR）"), false);
    }
    if (mono) { field.decode->addItem(QObject::tr("转为 8 位灰度"), true); }
    options->addRow(QObject::tr("解码方式"), field.decode);
    layout.addLayout(options);
    if (field.image_source) {
        const auto toggle = [source = field.image_source, file, browse, decode = field.decode] {
            const bool from_file = !source->currentData().toBool();
            file->setEnabled(from_file);
            browse->setEnabled(from_file);
            decode->setEnabled(from_file);
        };
        QObject::connect(field.image_source, &QComboBox::currentIndexChanged, owner, toggle);
        toggle();
    }
}

InputControls add_input(QWidget* parent, QFormLayout& form, const workflow::Solution& solution,
    const InputDescriptor& input, std::span<const PixelFormat> formats, bool sdk_sample)
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
        image_controls(field, entry, *editors, solution, formats, sdk_sample);
    } else if (kind == TypeKind::Bool) {
        auto* choice = new QComboBox(entry);
        choice->setPlaceholderText(QObject::tr("请选择"));
        choice->addItem(QObject::tr("假"), false);
        choice->addItem(QObject::tr("真"), true);
        choice->setCurrentIndex(-1);
        field.value = choice;
    } else if (kind == TypeKind::Int64 || kind == TypeKind::Float64) {
        field.value = new QLineEdit(entry);
    } else if (kind == TypeKind::String || kind == TypeKind::Binary) {
        auto* string = new QPlainTextEdit(entry);
        if (kind == TypeKind::Binary) { string->setPlaceholderText(QObject::tr("十六进制字节，例如 41 00 42")); }
        string->setMaximumHeight(76);
        field.value = string;
    } else {
        field.unavailable = QObject::tr("此输入类型不支持在窗口中填写。");
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
        field.mode->addItem(QObject::tr("指定本轮输入值"));
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
        if (widget->currentIndex() < 0) { return invalid_input("请选择布尔值。"); }
        return checked_scalar(input, widget->currentData().toBool(), ConfigValue::boolean, DataValue::boolean);
    }
    case TypeKind::Int64: {
        bool valid = false;
        const auto value = static_cast<QLineEdit*>(field.value)->text().toLongLong(&valid, 10);
        if (!valid) { return invalid_input("请输入有效的 64 位整数。"); }
        return checked_scalar(input, value, ConfigValue::integer, DataValue::integer);
    }
    case TypeKind::Float64: {
        bool valid = false;
        const auto value = static_cast<QLineEdit*>(field.value)->text().toDouble(&valid);
        if (!valid) { return invalid_input("请输入有效的双精度浮点数。"); }
        return checked_scalar(input, value, ConfigValue::real, DataValue::real);
    }
    case TypeKind::String: {
        auto value = static_cast<QPlainTextEdit*>(field.value)->document()->toRawText();
        value.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        return checked_scalar(input, value.toStdString(), ConfigValue::string, DataValue::string);
    }
    case TypeKind::Binary: {
        auto bytes = parse_hex_bytes(static_cast<QPlainTextEdit*>(field.value)->toPlainText());
        if (const auto* issue = std::get_if<Diagnostic>(&bytes)) { return *issue; }
        return checked_scalar(input, std::get<std::vector<std::uint8_t>>(std::move(bytes)), ConfigValue::binary, DataValue::binary);
    }
    default:
        return invalid_input("此输入类型不支持填写标量值。");
    }
}

QString collect_input(const InputControls& field, EnteredRunInputs& result)
{
    const auto& input = field.descriptor;
    if (!input.required && input.default_value && (!field.mode || field.mode->currentIndex() == 0)) { return {}; }
    if (!field.unavailable.isEmpty()) { return field.unavailable; }
    if (input.type.kind() == TypeKind::Image) {
        const auto index = field.frame->currentIndex();
        if (index < 0 || field.decode->currentIndex() < 0) { return QObject::tr("请选择像素坐标系和解码方式。"); }
        if (field.image_source && field.image_source->currentData().toBool()) {
            constexpr std::array bytes{std::byte{0}, std::byte{85}, std::byte{170}, std::byte{255}};
            auto pixels = ImageHandle::copy({2, 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono}}, bytes, field.frames.at(index));
            if (const auto* bad = std::get_if<Diagnostic>(&pixels)) { return text(bad->message); }
            auto value = DataValue::image({std::get<ImageHandle>(std::move(pixels)), {}, {}});
            if (const auto* bad = std::get_if<Diagnostic>(&value)) { return text(bad->message); }
            result.values.push_back({input.id, std::get<DataValue>(std::move(value))});
            return {};
        }
        const std::filesystem::path file{static_cast<QLineEdit*>(field.value)->text().toStdU16String()};
        if (!file.is_absolute()) { return QObject::tr("请选择图像文件或输入其绝对路径。"); }
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
    std::span<const core::InputDescriptor> inputs, bool isolated, std::span<const core::PixelFormat> image_formats, bool group, bool sdk_sample)
{
    if (inputs.empty()) { return EnteredRunInputs{}; }
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("runInputsDialog"));
    dialog.setWindowTitle(sdk_sample ? QObject::tr("SDK 图像输入") : group ? QObject::tr("工具组输入")
        : isolated ? QObject::tr("本次模块调试输入") : QObject::tr("流程输入"));
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
    for (const auto& input : inputs) { fields.push_back(add_input(contents, *form, solution, input, image_formats, sdk_sample)); }
    auto* error = new QLabel(&dialog);
    error->setObjectName(QStringLiteral("runInputError"));
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("runInputButtons"));
    buttons->button(QDialogButtonBox::Ok)->setText(isolated ? QObject::tr("调试") : QObject::tr("运行"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QObject::tr("取消"));
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
            error->setText(QObject::tr("读取本轮输入失败。"));
            error->show();
        }
    });
    dialog.resize(540, 360);
    dialog.exec();
    return result;
}

} // namespace visionpro::ui
