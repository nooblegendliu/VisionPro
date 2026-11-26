#include "image_output_settings.hpp"
#include "image_archive_dialogs.hpp"
#include "image_source_fields.hpp"
#include "toolbox_catalog.hpp"
#include <visionpro/infrastructure/image_archive.hpp>

#include <QCheckBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <set>

namespace visionpro::ui {
std::set<core::NodeId> preceding_nodes(const workflow::Workflow& flow,core::NodeId target)
{
    std::set<core::NodeId> found;
    std::vector<core::NodeId> pending{target};
    while (!pending.empty()) {
        const auto next = pending.back();
        pending.pop_back();
        for (const auto& link : flow.control_links) {
            if (link.target == next && link.source != target && found.insert(link.source).second) { pending.push_back(link.source); }
        }
    }
    return found;
}
namespace {
using namespace core;
using namespace workflow;

QFormLayout* section(QVBoxLayout& page, const QString& name)
{
    auto* group = new QGroupBox(name, page.parentWidget());
    auto* form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    page.addWidget(group);
    return form;
}

std::optional<DisplayItemStyle> choose_style(QWidget* parent, const DisplayItemStyle& initial)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("显示项样式"));
    QFormLayout form(&dialog);
    const auto color = [&](const QString& label, const std::string& value, const char* name) {
        auto* field = new QLineEdit(QString::fromStdString(value), &dialog);
        field->setObjectName(QLatin1String(name));
        field->setPlaceholderText(QObject::tr("沿用默认颜色"));
        form.addRow(label,field);
        return field;
    };
    auto* base = color(QObject::tr("默认颜色"),initial.color,"layerColor");
    auto* ok = color(QObject::tr("OK 颜色"),initial.ok_color,"layerOkColor");
    auto* ng = color(QObject::tr("NG 颜色"),initial.ng_color,"layerNgColor");
    auto* opacity = new QSpinBox(&dialog);
    opacity->setObjectName("layerOpacity");
    opacity->setRange(0,100);
    opacity->setSuffix("%");
    opacity->setValue(initial.opacity);
    form.addRow(QObject::tr("不透明度"),opacity);
    auto* width = new QSpinBox(&dialog);
    width->setObjectName("layerThickness");
    width->setRange(1,64);
    width->setValue(initial.thickness);
    form.addRow(QObject::tr("线宽（px）"),width);
    auto* error = new QLabel(&dialog);
    form.addRow(error);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    QObject::connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(&buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        for (auto* entry : {base,ok,ng}) {
            const auto text = entry->text().trimmed();
            if (!text.isEmpty() && (text.size() != 7 || !text.startsWith('#') || !QColor::isValidColorName(text))) {
                error->setText(QObject::tr("颜色应使用 #RRGGBB 格式。"));
                entry->setFocus();
                return;
            }
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) { return {}; }
    auto style = initial;
    style.color = base->text().trimmed().toStdString();
    style.ok_color = ok->text().trimmed().toStdString();
    style.ng_color = ng->text().trimmed().toStdString();
    style.opacity = opacity->value();
    style.thickness = width->value();
    return style;
}

void layer_choices(QVBoxLayout& page, const Workflow& flow, const Node& selected, const ToolRegistry& registry,
    const ToolDescriptor& consumer, const OutputEditorActions& edit)
{
    auto* form = section(page, QObject::tr("前项存储设置"));
    const auto preceding = preceding_nodes(flow,selected.id);
    for (const auto& node : flow.nodes) {
        if (!preceding.contains(node.id)) { continue; }
        const auto* tool = std::get_if<ToolConfiguration>(&node.configuration);
        const auto* known = tool ? registry.find_descriptor(tool->type) : nullptr;
        const auto* group = std::get_if<GroupConfiguration>(&node.configuration);
        std::optional<ToolDescriptor> actual;
        if (known && known->config_version == tool->config_version) { actual = specialize_tool(*known,node); }
        const auto* ports = actual ? &actual->outputs : group ? &group->interface.outputs : nullptr;
        if (!ports) { continue; }
        QFormLayout* items = nullptr;
        for (const auto& output : *ports) {
            const OutputReference source{flow.id, node.id, output.id, {}};
            for (const auto& item : output.display_items) {
                const auto target = source_key<InputId>(item.text());
                const auto field = std::ranges::find(consumer.inputs,target,&InputDescriptor::id);
                if (field == consumer.inputs.end() || field->binding.cardinality != BindingCardinality::MultipleSources
                    || !field->type.element_type() || *field->type.element_type() != output.type) { continue; }
                auto binding = std::ranges::find(selected.bindings, BindingTarget{target}, &Binding::target);
                if (!items) {
                    auto* group_box = new QGroupBox(QString::fromStdString(node.name),form->parentWidget());
                    items = new QFormLayout(group_box);
                    items->setRowWrapPolicy(QFormLayout::WrapLongRows);
                    form->addRow(group_box);
                }
                auto* check = new QCheckBox(display_field(QString::fromUtf8(item.text())), items->parentWidget());
                check->setObjectName(QStringLiteral("layer.%1.%2.%3").arg(QString::fromStdString(node.id.to_string()),
                    QString::fromUtf8(output.id.text()), QString::fromUtf8(item.text())));
                check->setChecked(binding != selected.bindings.end() && std::ranges::find(binding->sources, BindingSource{source}) != binding->sources.end());
                check->setToolTip(QString::fromStdString(node.name) + QStringLiteral(" / ") + QString::fromUtf8(output.id.text()));
                QObject::connect(check, &QCheckBox::toggled, check, [apply=edit.toggle_item, source, target](bool enabled) {
                    apply(target, source, enabled);
                });
                auto* row = new QHBoxLayout;
                row->addWidget(check,1);
                if (check->isChecked()) {
                    DisplayItemStyle style{};
                    style.opacity = item.text() == "masks" ? 35 : 100;
                    style.thickness = 1;
                    const auto existing = std::ranges::find_if(selected.display_styles, [&](const auto& entry) {
                        return entry.item == target && entry.source == source;
                    });
                    if (existing != selected.display_styles.end()) { style = existing->style; }
                    auto* configure = new QPushButton(QObject::tr("样式…"),items->parentWidget());
                    configure->setObjectName(check->objectName() + ".style");
                    QObject::connect(configure,&QPushButton::clicked,configure,[configure,style,apply=edit.style,source,target] {
                        if (auto value = choose_style(configure,style)) { apply(target,source,*value); }
                    });
                    row->addWidget(configure);
                }
                items->addRow(display_field(QString::fromStdString(output.display_name)),row);
            }
        }
    }
    if (form->rowCount() == 0) {
        auto* hint = new QLabel(QObject::tr("连接支持结果显示的前序模块后，可在此勾选标记。"), form->parentWidget());
        hint->setWordWrap(true);
        form->addRow(hint);
    }
}
} // namespace

void image_output_settings(QVBoxLayout& basic, QVBoxLayout& render, QVBoxLayout& layers,
    const Solution& solution, const Workflow& flow, const Node& node,
    const ToolRegistry& registry, const ToolDescriptor& descriptor, const OutputEditorActions& edit)
{
    const auto parameter = [&](std::string_view id) -> const ParameterDescriptor* {
        const auto it = std::ranges::find(descriptor.parameters, id, [](const auto& field) { return field.id.text(); });
        return it == descriptor.parameters.end() ? nullptr : &*it;
    };
    const auto value = [&](std::string_view id) -> const ConfigValue* {
        if (const auto* configured = source_value(node,id)) { return configured; }
        const auto* field = parameter(id);
        return field && field->default_value ? &*field->default_value : nullptr;
    };
    const auto flag = [&](std::string_view id) {
        const auto* configured = value(id);
        return configured && configured->as_bool() && *configured->as_bool();
    };
    const auto choice = [&](std::string_view id, std::string_view name) {
        const auto* configured = value(id);
        return configured && configured->as_enum() && configured->as_enum()->text() == name;
    };
    auto* form = section(basic, QObject::tr("图像输出"));
    const auto param = [&](std::string_view id, const char* label) {
        if (const auto* found = parameter(id)) {
            auto field = *found;
            field.display_name = QObject::tr(label).toStdString();
            edit.parameter(*form, field);
        }
    };
    const auto input = [&](std::string_view id, const char* label) {
        const auto it = std::ranges::find(descriptor.inputs, id, [](const auto& field) { return field.id.text(); });
        if (it != descriptor.inputs.end()) {
            auto field = *it;
            field.display_name = QObject::tr(label).toStdString();
            edit.input(*form, field);
        }
    };
    const auto directory = [&](std::string_view id, const char* label) {
        const auto it = std::ranges::find(descriptor.resources, id, [](const auto& slot) { return slot.slot.text(); });
        if (it != descriptor.resources.end()) { edit.directory(*form, *it, QObject::tr(label)); }
    };
    input("image", "输入源");
    param("output_enabled", "输出使能");
    param("pixel_mode", "像素格式");
    param("save_enabled", "存图使能");
    if (flag("save_enabled")) {
        form = section(basic, QObject::tr("存图条件"));
        param("trigger_enabled", "触发保存");
        const bool triggered = flag("trigger_enabled");
        if (triggered) {
            param("save_condition", "保存条件");
            if (!choice("save_condition","None")) {
                param("trigger_source_type", "触发变量类型");
                input("trigger", "触发变量");
            }
            param("save_interval", "存图间隔（张）");
        }
        param("save_original", "保存原图");
        param("save_rendered", "保存渲染图");
        if (solution.image_archive) {
            auto* notice = new QLabel(QObject::tr("大文件存图已启用，图像统一写入 %1。").arg(QString::fromStdString(solution.image_archive->directory.path)),form->parentWidget());
            notice->setWordWrap(true);
            form->addRow(notice);
        }
        for (bool original : {true,false}) {
            if (!flag(original ? "save_original" : "save_rendered")) { continue; }
            form = section(basic, original ? QObject::tr("原图归档") : QObject::tr("渲染图归档"));
            if (!solution.image_archive) {
                if (triggered) {
                    directory(original ? "original_ok_path":"rendered_ok_path", "OK 路径");
                    directory(original ? "original_ng_path":"rendered_ng_path", "NG 路径");
                }
                if (!triggered || choice("save_condition","All")) {
                    directory(original ? "original_path":"rendered_path", triggered ? "无判定时的路径" : "存储路径");
                }
            }
            param(original ? "original_prefix":"rendered_prefix", "文件名前缀");
            param(original ? "original_suffix":"rendered_suffix", "文件名后缀");
            param(original ? "original_cache":"rendered_cache", "存图缓存（张）");
        }
        form = section(basic, QObject::tr("存储策略"));
        param("date_directory","按月份和日期生成目录");
        param("sync_storage","同步存储");
        param("file_format","保存格式");
        if (choice("file_format","JPEG")) { param("jpeg_quality","JPEG 压缩质量"); }
        param("storage_policy","存储方式");
        param("max_files","最大文件数");
        param("min_free","磁盘剩余空间");
        param("space_unit","存储单位");
        param("retention_days","最大保存天数");
        form = section(basic, QObject::tr("FTP"));
        param("ftp_enabled","启用 FTP");
        if (flag("ftp_enabled")) {
            param("ftp_host","服务器地址"); param("ftp_port","端口");
            param("ftp_user","用户名"); param("ftp_credential","凭据引用");
            param("ftp_encoding","编码格式"); param("ftp_path","FTP 路径");
            const auto string_value = [&](const char* id) {
                const auto* field = value(id);
                return field && field->as_string() ? std::string(*field->as_string()) : std::string{};
            };
            const auto* port = value("ftp_port");
            infrastructure::ImageFtpSettings settings{string_value("ftp_host"), static_cast<std::uint16_t>(
                port && port->as_int64() ? *port->as_int64() : 21),string_value("ftp_user"), string_value("ftp_credential"),
                string_value("ftp_encoding"),string_value("ftp_path")};
            image_ftp_controls(form->parentWidget(), *form, std::move(settings),
                QStringLiteral("visionpro.image.output.") + QString::fromStdString(node.id.to_string()),edit.select_credential);
        }
    }
    form = section(render, QObject::tr("图形样式"));
    param("color","默认图形颜色（#RRGGBB）");
    param("scale_mode","图形倍率类型");
    if (choice("scale_mode","Custom")) { param("line_scale","线宽倍率"); param("text_scale","字宽倍率"); }
    param("line_type","线型");
    form = section(layers, QObject::tr("文本"));
    param("text","内容"); param("text_color","文字颜色"); param("text_ok_color","OK 文字颜色");
    param("text_ng_color","NG 文字颜色"); param("text_opacity","文字不透明度（%）");
    param("text_thickness","文字粗细"); param("font_size","字号");
    param("text_x","位置 X（px）"); param("text_y","位置 Y（px）");
    layer_choices(layers,flow,node,registry,descriptor,edit);
}
} // namespace visionpro::ui
