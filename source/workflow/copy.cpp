#include <visionpro/workflow/copy.hpp>
#include <visionpro/core/id_factory.hpp>

#include <algorithm>
#include <set>
#include <type_traits>
#include <utility>

namespace visionpro::workflow {
namespace {
using namespace core;

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

bool valid(ExternalReferencePolicy policy)
{ return policy == ExternalReferencePolicy::Preserve || policy == ExternalReferencePolicy::Reject; }

class Copier {
public:
    explicit Copier(ExternalReferencePolicy policy) : policy_(policy) {}
    IdentityRemap identities;
    CopyReport report;
    std::optional<Diagnostic> failure;
    std::map<NodeId, WorkflowId> node_owners;
    std::set<RegionId> region_ids;

    template <PersistentId Id>
    void assign(const Id& old, std::map<Id, Id>& mapping, std::set<Id>& reserved)
    {
        if (failure || mapping.contains(old)) { return; }
        for (int attempt = 0; attempt < 64; ++attempt) {
            auto generated = IdFactory::generate<Id>();
            if (const auto* invalid = std::get_if<Diagnostic>(&generated)) { failure = *invalid; return; }
            const auto replacement = std::get<Id>(generated);
            if (reserved.insert(replacement).second) { mapping.emplace(old, replacement); return; }
        }
        failure = error(ErrorCode::IdGenerationFailed, "Unable to allocate a collision-free copy identity.");
    }

    void collect(const ConfigValue& value)
    {
        if (const auto* region = value.as_region2d()) { region_ids.insert(region->id); }
        if (const auto* fields = value.fields()) { for (const auto& field : *fields) { collect(field.value); } }
        if (const auto* elements = value.elements()) { for (const auto& element : *elements) { collect(element); } }
        if (const auto* item = value.optional_value()) { collect(*item); }
    }
    void collect(const PublicInterface& interface)
    {
        for (const auto& input : interface.inputs) {
            if (input.default_value) { collect(*input.default_value); }
            for (const auto& value : input.constraints.allowed_values) { collect(value); }
        }
    }
    void collect(const Node& node)
    {
        for (const auto& binding : node.bindings) {
            for (const auto& source : binding.sources) {
                if (const auto* value = std::get_if<ConfigValue>(&source)) { collect(*value); }
            }
        }
        if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) { collect(group->interface); }
    }
    void allocate_regions()
    {
        const auto originals = region_ids;
        for (const auto& id : originals) { assign(id, identities.regions, region_ids); }
    }

    template <class Reference>
    void external(const Reference& reference)
    {
        if (failure) { return; }
        if (policy_ == ExternalReferencePolicy::Reject) {
            auto invalid = error(ErrorCode::ExternalReference, "Copy contains a reference outside the copied ownership boundary.");
            invalid.location.node = owner_;
            failure = std::move(invalid);
        } else { report.preserved_references.push_back({owner_, reference}); }
    }
    ResourceReference resource(ResourceReference reference)
    {
        if (const auto mapped = identities.resources.find(reference.id); mapped != identities.resources.end()) {
            reference.id = mapped->second;
        } else { external(reference); }
        return reference;
    }
    bool internal(const WorkflowId& workflow, const NodeId& node) const
    {
        const auto owner = node_owners.find(node);
        return owner != node_owners.end() && owner->second == workflow && identities.nodes.contains(node);
    }
    OutputReference output(OutputReference reference)
    {
        if (internal(reference.workflow, reference.node)) {
            reference.node = identities.nodes.at(reference.node);
            if (const auto workflow = identities.workflows.find(reference.workflow); workflow != identities.workflows.end()) {
                reference.workflow = workflow->second;
            }
        } else { external(reference); }
        return reference;
    }
    ScopedInputReference scoped_input(ScopedInputReference reference)
    {
        const bool owned = reference.group ? internal(reference.workflow, *reference.group)
            : identities.workflows.contains(reference.workflow);
        if (!owned) { external(reference); return reference; }
        if (reference.group) { reference.group = identities.nodes.at(*reference.group); }
        if (const auto workflow = identities.workflows.find(reference.workflow); workflow != identities.workflows.end()) {
            reference.workflow = workflow->second;
        }
        return reference;
    }
    GlobalReference global(GlobalReference reference)
    {
        if (const auto mapped = identities.variables.find(reference.variable); mapped != identities.variables.end()) {
            reference.variable = mapped->second;
        } else { external(reference); }
        return reference;
    }
    ControlOutputMapping control_export(ControlOutputMapping mapping, const WorkflowId& workflow)
    {
        if (internal(workflow, mapping.source)) { mapping.source = identities.nodes.at(mapping.source); }
        else { external(ControlExportReference{workflow, mapping}); }
        return mapping;
    }
    Transform2D transform(Transform2D value)
    {
        if (value.calibration) { value.calibration = resource(*value.calibration); }
        return value;
    }
    RegionSource region_source(RegionSource source)
    {
        std::visit([&](auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::same_as<T, InheritedRegion>) {
                const auto mapped = output({item.workflow, item.node, item.output, {}});
                item.workflow = mapped.workflow;
                item.node = mapped.node;
            } else if (auto* mask = std::get_if<MaskConfig>(&item.geometry)) {
                mask->resource = resource(mask->resource);
                if (mask->mapping.transform) { mask->mapping.transform = transform(*mask->mapping.transform); }
            }
            if (item.transform) { item.transform = transform(*item.transform); }
        }, source);
        return source;
    }
    ConfigValue value(const ConfigValue& original)
    {
        const auto unpack = [&](Result<ConfigValue> result) {
            if (const auto* invalid = std::get_if<Diagnostic>(&result)) {
                if (!failure) { failure = *invalid; }
                return original;
            }
            return std::get<ConfigValue>(std::move(result));
        };
        switch (original.type().kind()) {
        case TypeKind::ResourceReference: return unpack(ConfigValue::resource_reference(resource(*original.as_resource_reference())));
        case TypeKind::Transform2D: return unpack(ConfigValue::transform2d(transform(*original.as_transform2d())));
        case TypeKind::Region2D: {
            auto region = *original.as_region2d();
            region.id = identities.regions.at(region.id);
            region.source = region_source(region.source);
            for (auto& exclusion : region.exclusions) { exclusion = region_source(exclusion); }
            return unpack(ConfigValue::region2d(region));
        }
        case TypeKind::Record: {
            auto fields = *original.fields();
            for (auto& field : fields) { field.value = value(field.value); }
            return unpack(ConfigValue::record(original.type(), fields));
        }
        case TypeKind::List: {
            auto elements = *original.elements();
            for (auto& element : elements) { element = value(element); }
            return unpack(ConfigValue::list(*original.type().element_type(), elements));
        }
        case TypeKind::Optional:
            return original.optional_value() ? ConfigValue::some(value(*original.optional_value())) : original;
        default: return original; // Other immutable payloads have no persistent references.
        }
    }
    PublicInterface public_interface(PublicInterface interface)
    {
        for (auto& input : interface.inputs) {
            if (input.default_value) { input.default_value = value(*input.default_value); }
            for (auto& allowed : input.constraints.allowed_values) { allowed = value(allowed); }
        }
        return interface;
    }
    BindingSource source(const BindingSource& source)
    {
        return std::visit([&](const auto& item) -> BindingSource {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::same_as<T, ConfigValue>) { return value(item); }
            else if constexpr (std::same_as<T, OutputReference>) { return output(item); }
            else if constexpr (std::same_as<T, ScopedInputReference>) { return scoped_input(item); }
            else if constexpr (std::same_as<T, ResourceReference>) { return resource(item); }
            else { return global(item); }
        }, source);
    }
    Node node(const Node& original)
    {
        owner_ = original.id;
        auto result = original;
        result.id = identities.nodes.at(original.id);
        for (auto& binding : result.bindings) {
            for (auto& item : binding.sources) { item = source(item); }
        }
        for (auto& binding : result.resources) { binding.resource = resource(binding.resource); }
        if (auto* group = std::get_if<GroupConfiguration>(&result.configuration)) {
            group->interface = public_interface(group->interface);
            for (auto& entry : group->entries) {
                if (const auto found = identities.nodes.find(entry); found != identities.nodes.end()) { entry = found->second; }
                else { failure = error(ErrorCode::InvalidContainment, "A copied Group entry is not contained in its copied subtree."); }
            }
            for (auto& mapping : group->outputs) { mapping.source = output(mapping.source); }
            for (auto& mapping : group->control_exports) {
                mapping = control_export(mapping, node_owners.at(original.id));
            }
        }
        owner_.reset();
        return result;
    }
private:
    ExternalReferencePolicy policy_;
    std::optional<NodeId> owner_;
};

template <class Id>
bool collect_unique(const Id& id, std::set<Id>& identities)
{ return identities.insert(id).second; }

} // namespace

Result<SubgraphCopy> clone_subgraph(const Workflow& workflow, std::span<const NodeId> selected,
    ExternalReferencePolicy external_policy)
{
    if (!valid(external_policy) || selected.empty()) {
        return error(ErrorCode::InvalidModel, "Copy requires a nonempty selection and an explicit recognized external-reference policy.");
    }
    Copier copier(external_policy);
    std::set<NodeId> reserved_nodes;
    std::set<ControlLinkId> reserved_links;
    copier.collect(workflow.interface);
    for (const auto& node : workflow.nodes) {
        if (!collect_unique(node.id, reserved_nodes)) { return error(ErrorCode::DuplicateIdentity, "Copy source repeats a node identity."); }
        copier.node_owners.emplace(node.id, workflow.id);
        copier.collect(node); // Reserve region IDs in unselected nodes too.
    }
    for (const auto& link : workflow.control_links) {
        if (!collect_unique(link.id, reserved_links)) { return error(ErrorCode::DuplicateIdentity, "Copy source repeats a control-link identity."); }
    }
    // Validate containment through the hierarchy API before expanding any Group.
    if (const auto roots = children(workflow); const auto* invalid = std::get_if<Diagnostic>(&roots)) { return *invalid; }
    std::set<NodeId> selection;
    for (const auto& id : selected) {
        const auto* node = find_node(workflow, id);
        if (!node) { return error(ErrorCode::UnknownTarget, "Copy selection names an absent node."); }
        selection.insert(id);
        if (std::holds_alternative<GroupConfiguration>(node->configuration)) {
            const auto nested = descendants(workflow, id);
            if (const auto* invalid = std::get_if<Diagnostic>(&nested)) { return *invalid; }
            for (const auto& child : std::get<std::vector<NodeId>>(nested)) { selection.insert(child); }
        }
    }
    for (const auto& id : selection) { copier.assign(id, copier.identities.nodes, reserved_nodes); }
    // Allocate only copied region identities while retaining all source IDs as a collision guard.
    const auto reserved_regions = copier.region_ids;
    copier.region_ids.clear();
    for (const auto& node : workflow.nodes) { if (selection.contains(node.id)) { copier.collect(node); } }
    const auto copied_regions = copier.region_ids;
    copier.region_ids = reserved_regions;
    for (const auto& id : copied_regions) { copier.assign(id, copier.identities.regions, copier.region_ids); }
    if (copier.failure) { return *copier.failure; }
    SubgraphCopy result{workflow.id, {}, {}, {}, {}, {}};
    for (const auto& node : workflow.nodes) { if (selection.contains(node.id)) { result.nodes.push_back(copier.node(node)); } }
    for (const auto& link : workflow.control_links) {
        const bool source = selection.contains(link.source), target = selection.contains(link.target);
        if (source && target) {
            copier.assign(link.id, copier.identities.control_links, reserved_links);
            if (copier.failure) { return *copier.failure; }
            result.control_links.push_back({copier.identities.control_links.at(link.id), copier.identities.nodes.at(link.source),
                link.output, copier.identities.nodes.at(link.target)});
        } else if (source || target) { copier.report.omitted_control_links.push_back(link); }
    }
    for (const auto& edge : workflow.containment) {
        if (!selection.contains(edge.child)) { continue; }
        const auto parent = copier.identities.nodes.find(edge.parent);
        if (parent == copier.identities.nodes.end()) {
            copier.external(edge);
            result.containment.push_back({edge.parent, copier.identities.nodes.at(edge.child)});
        } else { result.containment.push_back({parent->second, copier.identities.nodes.at(edge.child)}); }
    }
    if (copier.failure) { return *copier.failure; }
    result.identities = std::move(copier.identities);
    result.report = std::move(copier.report);
    return result;
}

Result<SolutionCopy> clone_solution(const Solution& solution, ExternalReferencePolicy external_policy)
{
    if (!valid(external_policy)) { return error(ErrorCode::InvalidModel, "Copy requires a recognized external-reference policy."); }
    if (auto checked = validate_model_shape(solution); const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    Copier copier(external_policy);
    std::set<SolutionId> solution_ids{solution.id};
    std::set<WorkflowId> workflow_ids;
    std::set<NodeId> node_ids;
    std::set<ControlLinkId> link_ids;
    std::set<ResourceId> resource_ids;
    std::set<VariableId> variable_ids;
    std::set<QueueId> queue_ids;
    for (const auto& workflow : solution.workflows) {
        workflow_ids.insert(workflow.id);
        copier.collect(workflow.interface);
        for (const auto& node : workflow.nodes) {
            node_ids.insert(node.id);
            copier.node_owners.emplace(node.id, workflow.id);
            copier.collect(node);
        }
        for (const auto& link : workflow.control_links) {
            if (!find_node(workflow, link.source) || !find_node(workflow, link.target)) {
                auto invalid = error(ErrorCode::InvalidModel, "A copied control link must connect nodes owned by its workflow.");
                invalid.location.workflow = workflow.id;
                invalid.location.control_link = link.id;
                return invalid;
            }
            link_ids.insert(link.id);
        }
    }
    for (const auto& resource : solution.resources) {
        resource_ids.insert(resource.id);
        if (const auto* device = std::get_if<DeviceResource>(&resource.location)) { copier.collect(device->configuration); }
    }
    for (const auto& variable : solution.variables) { variable_ids.insert(variable.id); copier.collect(variable.initial_value); }
    for (const auto& queue : solution.queues) { queue_ids.insert(queue.id); }
    copier.assign(solution.id, copier.identities.solutions, solution_ids);
    for (const auto& workflow : solution.workflows) {
        copier.assign(workflow.id, copier.identities.workflows, workflow_ids);
        for (const auto& node : workflow.nodes) { copier.assign(node.id, copier.identities.nodes, node_ids); }
        for (const auto& link : workflow.control_links) { copier.assign(link.id, copier.identities.control_links, link_ids); }
    }
    for (const auto& resource : solution.resources) { copier.assign(resource.id, copier.identities.resources, resource_ids); }
    for (const auto& variable : solution.variables) { copier.assign(variable.id, copier.identities.variables, variable_ids); }
    for (const auto& queue : solution.queues) { copier.assign(queue.id, copier.identities.queues, queue_ids); }
    copier.allocate_regions();
    if (copier.failure) { return *copier.failure; }
    auto result = solution;
    result.id = copier.identities.solutions.at(solution.id);
    for (auto& workflow : result.workflows) {
        const auto original_workflow = workflow.id;
        workflow.id = copier.identities.workflows.at(workflow.id);
        workflow.interface = copier.public_interface(workflow.interface);
        for (auto& node : workflow.nodes) { node = copier.node(node); }
        for (auto& link : workflow.control_links) {
            link.id = copier.identities.control_links.at(link.id);
            link.source = copier.identities.nodes.at(link.source);
            link.target = copier.identities.nodes.at(link.target);
        }
        for (auto& edge : workflow.containment) {
            edge.parent = copier.identities.nodes.at(edge.parent);
            edge.child = copier.identities.nodes.at(edge.child);
        }
        for (auto& entry : workflow.entries) { entry = copier.identities.nodes.at(entry); }
        for (auto& mapping : workflow.outputs) { mapping.source = copier.output(mapping.source); }
        for (auto& mapping : workflow.control_exports) { mapping = copier.control_export(mapping, original_workflow); }
    }
    for (auto& resource : result.resources) {
        resource.id = copier.identities.resources.at(resource.id);
        if (auto* device = std::get_if<DeviceResource>(&resource.location)) { device->configuration = copier.value(device->configuration); }
    }
    for (auto& variable : result.variables) {
        variable.id = copier.identities.variables.at(variable.id);
        variable.initial_value = copier.value(variable.initial_value);
    }
    for (auto& queue : result.queues) { queue.id = copier.identities.queues.at(queue.id); }
    for (auto& layout : result.layouts) {
        layout.workflow = copier.identities.workflows.at(layout.workflow);
        for (auto& placement : layout.nodes) { placement.node = copier.identities.nodes.at(placement.node); }
    }
    if (copier.failure) { return *copier.failure; }
    return SolutionCopy{std::move(result), std::move(copier.identities), std::move(copier.report)};
}

} // namespace visionpro::workflow
