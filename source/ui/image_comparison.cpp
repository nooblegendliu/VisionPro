#include "image_comparison.hpp"
#include "image_panel.hpp"
#include "toolbox_catalog.hpp"
#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace visionpro::ui {
using namespace core;
using namespace workflow;

ImageComparison::ImageComparison(QWidget* parent) : QWidget(parent)
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0,0,0,0);
    auto* splitter = new QSplitter(Qt::Horizontal,this);
    row->addWidget(splitter);
    for (int side=0;side<2;++side) {
        auto& pane = panes_[side];
        auto* column = new QWidget(splitter);
        auto* layout = new QVBoxLayout(column);
        layout->setContentsMargins(0,0,0,0);
        auto* toolbar = new QHBoxLayout;
        pane.source = new QComboBox(column);
        pane.source->setObjectName(QStringLiteral("comparisonSource%1").arg(side+1));
        pane.source->setAccessibleName(tr("画面 %1 图像源").arg(side+1));
        pane.source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        pane.source->setMinimumContentsLength(5);
        toolbar->addWidget(pane.source,1);
        auto* button = new QToolButton(column);
        button->setObjectName(QStringLiteral("comparisonGraphics%1").arg(side+1));
        button->setText(tr("图形源"));
        button->setAccessibleName(tr("画面 %1 图形源").arg(side+1));
        button->setPopupMode(QToolButton::InstantPopup);
        pane.graphics = new QMenu(button);
        button->setMenu(pane.graphics);
        toolbar->addWidget(button);
        layout->addLayout(toolbar);
        pane.image = new ImagePanel(column);
        pane.image->setObjectName(QStringLiteral("comparisonImage%1").arg(side+1));
        layout->addWidget(pane.image,1);
        splitter->addWidget(column);
        connect(pane.source,&QComboBox::currentIndexChanged,this,[this,side](int index) {
            if (!flow_ || index < 0 || index > static_cast<int>(images_.size())) { return; }
            chosen_[*flow_][side].image = index ? std::optional{images_[index-1].channel} : std::nullopt;
            render(side);
        });
    }
    splitter->setSizes({400,400});
}

void ImageComparison::reset_document()
{
    chosen_.clear();
    suspend();
}

void ImageComparison::suspend()
{
    if (!flow_ && !run_ && images_.empty() && graphics_.empty()) { return; }
    run_.reset();
    flow_.reset();
    scope_.reset();
    quiescent_ = false;
    images_.clear();
    graphics_.clear();
    for (int side=0;side<2;++side) { populate(side); render(side); }
}

void ImageComparison::update_run(const Workflow* flow,const ToolRegistry& registry,
    const std::optional<RunHandle>& run,std::optional<ExecutionScopeId> scope)
{
    const auto id = flow ? std::optional{flow->id} : std::nullopt;
    const bool same_run = (!run && !run_) || (run && run_ && run_->identity() == run->identity());
    if (run && quiescent_ && same_run && flow_ == id && scope_ == scope) { return; }
    std::vector<Choice> images,graphics;
    std::map<NodeId,std::vector<const RunNodeProgress*>> calls;
    const auto report = run ? run->inspect() : Result<RunProgress>{RunProgress{}};
    quiescent_ = false;
    if (const auto* progress = std::get_if<RunProgress>(&report)) {
        quiescent_ = progress->quiescent;
        for (const auto& call : progress->nodes) {
            if (call.scope && call.scope != scope && call.scope->value() != 0) { continue; }
            calls[call.node].push_back(&call);
        }
    }
    if (flow) {
        for (const auto& node : flow->nodes) {
            const auto* config = std::get_if<ToolConfiguration>(&node.configuration);
            const auto* tool = config ? (run ? run->request().snapshot().find_tool(node) : registry.find_descriptor(config->type)) : nullptr;
            const auto* group = std::get_if<GroupConfiguration>(&node.configuration);
            const auto* ports = group ? &group->interface.outputs : tool ? &tool->outputs : nullptr;
            if (!ports) { continue; }
            const auto& records = calls[node.id];
            const auto count = std::max<std::size_t>(1,records.size());
            for (std::size_t i=0;i<count;++i) {
                const auto* call = records.empty() ? nullptr : records[i];
                const auto suffix = count == 1 ? QString{} : tr(" · 调用 %1").arg(i+1);
                for (const auto& port : *ports) {
                    const auto label = QString::fromStdString(node.name) + QStringLiteral(" / ");
                    const auto add = [&](std::vector<Choice>& target,std::optional<SchemaId> item,const QString& field) {
                        target.push_back({{node.id,port.id,item,i},label+field+suffix,
                            call ? call->invocation : std::nullopt,call ? call->status : ExecutionStatus::Pending});
                    };
                    if (port.type.kind() == TypeKind::Image) { add(images,{},display_field(QString::fromStdString(port.display_name))); }
                    for (const auto& item : port.display_items) {
                        add(graphics,item,display_field(QString::fromUtf8(item.text())));
                    }
                }
            }
        }
    }
    if (flow_ == id && same_run && scope_ == scope && images_ == images && graphics_ == graphics) { return; }
    flow_ = id;
    run_ = run;
    scope_ = scope;
    images_ = std::move(images);
    graphics_ = std::move(graphics);
    for (int side=0;side<2;++side) { populate(side); render(side); }
}

void ImageComparison::populate(int side)
{
    auto& pane = panes_[side];
    const QSignalBlocker block(pane.source);
    pane.source->clear();
    pane.source->addItem(tr("画面 %1 · 选择图像源").arg(side+1));
    pane.graphics->clear();
    if (!flow_) { return; }
    const auto& selection = chosen_[*flow_][side];
    for (const auto& item : images_) {
        pane.source->addItem(item.text);
        if (selection.image == item.channel) { pane.source->setCurrentIndex(pane.source->count()-1); }
    }
    for (const auto& item : graphics_) {
        auto* action = pane.graphics->addAction(item.text);
        action->setCheckable(true);
        action->setChecked(std::ranges::find(selection.graphics,item.channel) != selection.graphics.end());
        connect(action,&QAction::toggled,this,[this,side,key=item.channel](bool on) {
            if (!flow_) { return; }
            auto& selected = chosen_[*flow_][side].graphics;
            if (on) { selected.push_back(key); } else { std::erase(selected,key); }
            render(side);
        });
    }
}

void ImageComparison::render(int side)
{
    auto* image = panes_[side].image;
    const auto clear = [&](const QString& reason) { image->clear_frame(true); image->set_provenance(reason); };
    if (!flow_ || !run_) { clear(tr("请选择已执行的轮次")); return; }
    const auto& selection = chosen_[*flow_][side];
    const auto slot = std::ranges::find_if(images_,[&](const auto& v){return selection.image == v.channel;});
    const auto origin = tr("第 %1 轮").arg(run_->identity().run.value());
    if (slot == images_.end()) { clear(origin + tr(" · 请选择图像源")); return; }
    const auto record = [&](const Choice& value) -> std::optional<NodeResult> {
        if (!value.call) { return {}; }
        auto found = run_->lookup(*value.call);
        auto* entry = std::get_if<std::optional<NodeResult>>(&found);
        return entry ? std::move(*entry) : std::nullopt;
    };
    const auto result = record(*slot);
    if (!result) { clear(origin + tr(" · 此图像源无本轮结果")); return; }
    auto shown = image->present_result(*result,slot->channel.port,nullptr,nullptr);
    if (const auto* error = std::get_if<Diagnostic>(&shown)) { clear(origin + " · " + QString::fromStdString(error->message)); return; }
    QStringList details{origin,slot->text,tr("调用 %1").arg(slot->call->invocation.value())};
    for (const auto& selected : selection.graphics) {
        const auto item = std::ranges::find(graphics_,selected,&Choice::channel);
        if (item == graphics_.end()) { details << tr("图形源在本轮不可用"); continue; }
        const auto source = record(*item);
        if (!source) { details << tr("%1 本轮未产出").arg(item->text); continue; }
        auto overlay = image->append_graphic(*source,item->channel.port,*item->channel.item);
        if (const auto* error = std::get_if<Diagnostic>(&overlay)) { details << item->text + " · " + QString::fromStdString(error->message); }
        else { details << item->text; }
    }
    image->set_provenance(details.join(QStringLiteral(" · ")));
}
} // namespace visionpro::ui
