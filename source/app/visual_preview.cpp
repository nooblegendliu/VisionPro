#include "visual_preview.hpp"

#include "vision/read_file.hpp"
#include "vision/roi.hpp"
#include "vision/vision_tools.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace visionpro::app {
namespace {
using namespace core;
using namespace workflow;
using namespace tools::vision;
using infrastructure::FileResourceLease;
using infrastructure::FileResourceProvider;

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

FileImageDecodeMode decode_mode(bool grayscale)
{ return grayscale ? FileImageDecodeMode::Gray8 : FileImageDecodeMode::Native8; }

bool is_tool(const Node& node, std::string_view type)
{
    const auto* tool = std::get_if<ToolConfiguration>(&node.configuration);
    return tool && tool->type.text() == type && tool->config_version == 1;
}

template<class Target> const BindingSource* single_source(const Node& node, std::string_view id)
{
    const BindingSource* source = nullptr;
    for (const auto& binding : node.bindings) {
        const auto* target = std::get_if<Target>(&binding.target);
        if (!target || target->text() != id) { continue; }
        if (source || binding.cardinality != BindingCardinality::SingleSource || binding.conversion != ConversionKind::Exact
            || !binding.availability.accepted_absence.empty() || binding.sources.size() != 1) { return nullptr; }
        source = &binding.sources.front();
    }
    return source;
}

bool is_local_image_source(const Node& node)
{
    if (is_tool(node, "visionpro.image.read_file")) { return true; }
    if (!is_tool(node, "visionpro.image.source")) { return false; }
    const auto source = single_source<ParameterId>(node, "source_type");
    if (!source) {
        // Local is the descriptor default when there is no explicit selection.
        return std::ranges::none_of(node.bindings, [](const Binding& binding) {
            const auto* id = std::get_if<ParameterId>(&binding.target);
            return id && id->text() == "source_type";
        });
    }
    const auto* value = std::get_if<ConfigValue>(source);
    return value && value->as_enum() && value->as_enum()->text() == "Local";
}

Result<std::optional<NodeId>> scope_of(const Workflow& flow, const Node& node)
{
    std::optional<NodeId> scope;
    for (const auto& edge : flow.containment) {
        if (edge.child != node.id) { continue; }
        const auto* parent = find_node(flow, edge.parent);
        if (scope || !parent || !std::holds_alternative<GroupConfiguration>(parent->configuration)) {
            return error(ErrorCode::InvalidContainment, "Cannot determine the ROI preview node's immediate scope.");
        }
        scope = edge.parent;
    }
    return scope;
}

Result<const Node*> find_source(const Workflow& flow, const Node& consumer, const WorkflowId& workflow,
    const NodeId& source)
{
    if (workflow != flow.id) { return error(ErrorCode::ScopeViolation, "Preview sources must be in the same workflow."); }
    const auto* producer = find_node(flow, source);
    if (!producer) { return error(ErrorCode::UnknownTarget, "Preview source node does not exist."); }
    if (!producer->enabled) { return error(ErrorCode::InputUnavailable, "Preview source node is disabled."); }
    const auto consumer_scope = scope_of(flow, consumer);
    if (const auto* failure = std::get_if<Diagnostic>(&consumer_scope)) { return *failure; }
    const auto producer_scope = scope_of(flow, *producer);
    if (const auto* failure = std::get_if<Diagnostic>(&producer_scope)) { return *failure; }
    if (std::get<std::optional<NodeId>>(consumer_scope) != std::get<std::optional<NodeId>>(producer_scope)) {
        return error(ErrorCode::ScopeViolation, "Preview sources must be in the same Group scope.");
    }
    return producer;
}

Result<NodeId> image_reader(const Solution& solution, const Workflow& flow, const Node& target,
    const CoordinateSystemId& frame)
{
    const Node* consumer = &target;
    std::set<NodeId> visited{consumer->id};
    for (;;) {
        const auto* input = single_source<InputId>(*consumer, is_tool(*consumer, "visionpro.image.blob") ? "binary" : "image");
        const auto* reference = input ? std::get_if<OutputReference>(input) : nullptr;
        const auto* port = reference ? std::get_if<OutputId>(&reference->output) : nullptr;
        if (!port || !reference->selector.empty()) { break; }
        auto found = find_source(flow, *consumer, reference->workflow, reference->node);
        if (auto* failure = std::get_if<Diagnostic>(&found)) { return std::move(*failure); }
        const auto* producer = std::get<const Node*>(found);
        if (!visited.insert(producer->id).second) {
            return error(ErrorCode::DependencyCycle, "Image input declarations form a preview cycle.");
        }
        if (port->text() == "binary" && is_tool(*producer, "visionpro.image.threshold")) {
            consumer = producer;
            continue;
        }
        if (port->text() != "image" || !is_local_image_source(*producer)) { break; }
        const auto* origin_source = single_source<ParameterId>(*producer, "frame_origin");
        const auto* origin_value = origin_source ? std::get_if<ConfigValue>(origin_source) : nullptr;
        const auto* origin = origin_value ? origin_value->as_point2d() : nullptr;
        if (!origin) { break; }
        if (auto checked = validate_file_frame_origin(*origin); auto* failure = std::get_if<Diagnostic>(&checked)) {
            return std::move(*failure);
        }
        if (origin->coordinate_frame != frame) {
            return error(ErrorCode::CoordinateMismatch, "Configured image origin does not match the selected preview image frame.");
        }
        const auto local = [](const NodeResourceBinding& binding) {
            return binding.slot.text() == "source" || binding.slot.text() == "sequence";
        };
        if (std::ranges::count_if(producer->resources, local) != 1) { break; }
        const auto& slot = *std::ranges::find_if(producer->resources, local);
        const auto& resource = slot.resource;
        const auto expected = slot.slot.text() == "source" ? ResourceKind::Image : ResourceKind::DirectorySequence;
        if (resource.kind != expected) {
            auto failure = error(ErrorCode::ResourceKindMismatch, "The local image preview resource does not match its source slot.");
            failure.location.resource = resource.id;
            return failure;
        }
        if (auto checked = validate_resource_reference(resource, solution.resources); auto* failure = std::get_if<Diagnostic>(&checked)) {
            return std::move(*failure);
        }
        return producer->id;
    }
    return error(ErrorCode::InputUnavailable, "Inherited ROI preview requires a configured file-image input chain.");
}

struct PreviewFiles {
    const Solution& solution;
    const std::optional<std::filesystem::path>& directory;
    std::span<const infrastructure::NamedResourceRoot> roots;
    std::map<ResourceId, std::shared_ptr<const FileResourceLease>> captured;
};

Result<std::optional<InvocationResourceLease>> mask_lease(PreviewFiles& files, const Node& node,
    const RegionDefinition& region, const RegionDefinition* inherited)
{
    std::optional<ResourceReference> reference;
    const auto combine = [&](const ResourceReference& resource) -> Result<std::monostate> {
        const auto fail = [&](ErrorCode code, std::string message) {
            auto failure = error(code, std::move(message));
            failure.location.resource = resource.id;
            return failure;
        };
        if (resource.kind != ResourceKind::Mask) {
            return fail(ErrorCode::ResourceKindMismatch, "ROI preview requires a Mask resource.");
        }
        if (reference && reference->id != resource.id) {
            return fail(ErrorCode::InvalidResource, "ROI preview masks must use one resource identity.");
        }
        if (reference && reference->revision && resource.revision && reference->revision != resource.revision) {
            return fail(ErrorCode::ResourceVersionMismatch, "ROI preview masks must use the same resource revision.");
        }
        if (!reference || resource.revision) { reference = resource; }
        return std::monostate{};
    };
    const auto collect = [&](const RegionSource& source) -> Result<std::monostate> {
        const auto* primitive = std::get_if<RegionPrimitive>(&source);
        const auto* mask = primitive ? std::get_if<MaskConfig>(&primitive->geometry) : nullptr;
        return mask ? combine(mask->resource) : Result<std::monostate>{std::monostate{}};
    };
    const auto collect_region = [&](const RegionDefinition& definition) -> Result<std::monostate> {
        if (auto checked = collect(definition.source); auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
        for (const auto& exclusion : definition.exclusions) {
            if (auto checked = collect(exclusion); auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
        }
        return std::monostate{};
    };
    if (auto checked = collect_region(region); auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
    if (inherited) {
        if (auto checked = collect_region(*inherited); auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
    }
    if (!reference) { return std::optional<InvocationResourceLease>{}; }

    const auto slot = std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>("mask"));
    bool found_slot = false;
    for (const auto& binding : node.resources) {
        if (binding.slot != slot) { continue; }
        if (found_slot) { return error(ErrorCode::InvalidResource, "ROI preview mask slot must have one binding."); }
        found_slot = true;
        if (auto checked = combine(binding.resource); auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
    }
    auto captured = files.captured.find(reference->id);
    if (captured == files.captured.end()) {
        auto file = FileResourceProvider::read_resource(files.solution, *reference, files.directory, files.roots);
        if (auto* failure = std::get_if<Diagnostic>(&file)) { return std::move(*failure); }
        captured = files.captured.emplace(reference->id, std::get<std::shared_ptr<const FileResourceLease>>(std::move(file))).first;
    }
    if (reference->revision && reference->revision != captured->second->reference().revision) {
        auto failure = error(ErrorCode::ResourceVersionMismatch, "Requested mask revision differs from the contents captured for preview.");
        failure.location.resource = reference->id;
        return failure;
    }
    return std::optional<InvocationResourceLease>{{slot, captured->second}};
}

Result<RoiEvaluation> evaluate_draft(const ImageHandle& image, PreviewFiles& files, const Node& node,
    const ToolValueTarget& target, const RegionDefinition& region, const RegionDefinition* inherited)
{
    auto resource = mask_lease(files, node, region, inherited);
    if (auto* failure = std::get_if<Diagnostic>(&resource)) { return std::move(*failure); }
    const auto& lease = std::get<std::optional<InvocationResourceLease>>(resource);
    return evaluate_roi_preview(image, region, target, inherited,
        lease ? std::span<const InvocationResourceLease>{&*lease, 1} : std::span<const InvocationResourceLease>{});
}

struct InheritanceLayer { const Node* producer; const RegionDefinition* region; };

Result<std::vector<InheritanceLayer>> inheritance(const ImageHandle& image, const Solution& solution, const Workflow& flow,
    const Node& target_node, const RegionDefinition& region)
{
    std::vector<InheritanceLayer> layers;
    if (!std::holds_alternative<InheritedRegion>(region.source)) { return layers; }
    const auto target_reader = image_reader(solution, flow, target_node, image.coordinate_frame());
    if (const auto* failure = std::get_if<Diagnostic>(&target_reader)) { return *failure; }
    const Node* consumer = &target_node;
    const RegionDefinition* draft = &region;
    std::set<NodeId> visited{consumer->id};
    while (const auto* reference = std::get_if<InheritedRegion>(&draft->source)) {
        auto found = find_source(flow, *consumer, reference->workflow, reference->node);
        if (auto* failure = std::get_if<Diagnostic>(&found)) { return std::move(*failure); }
        const auto* producer = std::get<const Node*>(found);
        if (!visited.insert(producer->id).second) {
            return error(ErrorCode::DependencyCycle, "Configured ROI inheritance forms a preview cycle.");
        }
        if (reference->output.text() != "effective_roi" || !is_tool(*producer, "visionpro.image.threshold")) {
            return error(ErrorCode::InputUnavailable, "Inherited output requires a runtime result from the declared source.");
        }
        const auto* binding = single_source<ParameterId>(*producer, "roi");
        const auto* value = binding ? std::get_if<ConfigValue>(binding) : nullptr;
        const auto* configured = value ? value->as_region2d() : nullptr;
        if (!configured) { return error(ErrorCode::InputUnavailable, "The source threshold ROI requires a runtime binding value."); }
        auto reader = image_reader(solution, flow, *producer, image.coordinate_frame());
        if (auto* failure = std::get_if<Diagnostic>(&reader)) { return std::move(*failure); }
        if (std::get<NodeId>(reader) != std::get<NodeId>(target_reader)) {
            return error(ErrorCode::InputUnavailable, "Inherited ROI preview requires the source and target to use the same file-image input.");
        }
        layers.push_back({producer, configured});
        consumer = producer;
        draft = configured;
    }
    return layers;
}
} // namespace

core::Result<core::ImageHandle> read_preview_image(const std::filesystem::path& absolute_file,
    const core::CoordinateSystemId& frame, bool grayscale)
{
    auto file = infrastructure::read_file_bytes(absolute_file);
    if (auto* failure = std::get_if<Diagnostic>(&file)) { return std::move(*failure); }
    return decode_file_image(*std::get<std::shared_ptr<const std::vector<std::byte>>>(file), decode_mode(grayscale), frame);
}

core::Result<core::ImageHandle> read_preview_resource(const workflow::Solution& solution,
    const core::ResourceReference& reference, const core::CoordinateSystemId& frame, bool grayscale,
    const std::optional<std::filesystem::path>& solution_directory,
    std::span<const infrastructure::NamedResourceRoot> roots)
{
    if (reference.kind != ResourceKind::Image) {
        auto failure = error(ErrorCode::ResourceKindMismatch, "Image preview requires an Image resource.");
        failure.location.solution = solution.id;
        failure.location.resource = reference.id;
        return failure;
    }
    auto file = FileResourceProvider::read_resource(solution, reference, solution_directory, roots);
    if (auto* failure = std::get_if<Diagnostic>(&file)) { return std::move(*failure); }
    auto image = decode_file_image(std::get<std::shared_ptr<const FileResourceLease>>(file)->bytes(), decode_mode(grayscale), frame);
    if (auto* failure = std::get_if<Diagnostic>(&image)) {
        failure->location.solution = solution.id;
        failure->location.resource = reference.id;
    }
    return image;
}

core::Result<RoiPreview> preview_roi(const core::ImageHandle& image, const workflow::Solution& solution,
    const core::WorkflowId& workflow, const core::NodeId& node, const core::ToolValueTarget& target,
    const core::RegionDefinition& region, const std::optional<std::filesystem::path>& solution_directory,
    std::span<const infrastructure::NamedResourceRoot> roots)
{
    const auto locate = [&](Diagnostic failure, const NodeId& owner, const ToolValueTarget& binding,
        const RegionDefinition& definition) {
        failure.location.solution = solution.id;
        failure.location.workflow = workflow;
        failure.location.node = owner;
        if (!failure.location.region) { failure.location.region = definition.id; }
        if (const auto* parameter = std::get_if<ParameterId>(&binding)) { failure.location.parameter = *parameter; }
        else { failure.location.input = std::get<InputId>(binding); }
        return failure;
    };
    const auto* flow = find_workflow(solution, workflow);
    const auto* owner = flow ? find_node(*flow, node) : nullptr;
    if (!owner) { return locate(error(ErrorCode::UnknownTarget, "ROI preview target node does not exist."), node, target, region); }
    auto layers = inheritance(image, solution, *flow, *owner, region);
    if (auto* failure = std::get_if<Diagnostic>(&layers)) { return locate(std::move(*failure), node, target, region); }
    PreviewFiles files{solution, solution_directory, roots, {}};
    std::optional<RegionDefinition> inherited;
    const ToolValueTarget producer_binding = std::get<ParameterId>(SchemaIdFactory::parse<ParameterId>("roi"));
    const auto& chain = std::get<std::vector<InheritanceLayer>>(layers);
    for (auto layer = chain.rbegin(); layer != chain.rend(); ++layer) {
        auto preview = evaluate_draft(image, files, *layer->producer, producer_binding, *layer->region,
            inherited ? &*inherited : nullptr);
        if (auto* failure = std::get_if<Diagnostic>(&preview)) {
            return locate(std::move(*failure), layer->producer->id, producer_binding, *layer->region);
        }
        auto& effective = std::get<RoiEvaluation>(preview).effective_region;
        if (auto* failure = std::get_if<Diagnostic>(&effective)) {
            return locate(std::move(*failure), layer->producer->id, producer_binding, *layer->region);
        }
        inherited = std::get<RegionDefinition>(std::move(effective));
    }
    auto preview = evaluate_draft(image, files, *owner, target, region, inherited ? &*inherited : nullptr);
    if (auto* failure = std::get_if<Diagnostic>(&preview)) { return locate(std::move(*failure), node, target, region); }
    auto evaluated = std::get<RoiEvaluation>(std::move(preview));
    RoiPreview result{std::move(evaluated.mask), {}, evaluated.member_count, {}};
    if (auto* issue = std::get_if<Diagnostic>(&evaluated.effective_region)) { result.outline_issue = locate(std::move(*issue), node, target, region); }
    else { result.effective_region = std::get<RegionDefinition>(std::move(evaluated.effective_region)); }
    return result;
}

} // namespace visionpro::app
