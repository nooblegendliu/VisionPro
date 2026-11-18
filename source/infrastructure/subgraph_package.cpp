#include "windows_path.hpp"
#include <visionpro/infrastructure/subgraph_package.hpp>
#include <visionpro/workflow/copy.hpp>

#include "resource_codec.hpp"
#include "imported_asset.hpp"
#include "package_assets.hpp"
#include "resource_files.hpp"
#include "solution_codec.hpp"
#include "solution_storage.hpp"
#include "value_codec.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <type_traits>

namespace visionpro::infrastructure {
namespace {
namespace fs = std::filesystem;
using namespace core;
using namespace workflow;
using detail::Json;
using detail::Reader;

template<class T> T require(Result<T> result)
{
    if (auto* d = std::get_if<Diagnostic>(&result)) { throw detail::CodecError{std::move(*d)}; }
    return std::get<T>(std::move(result));
}
[[noreturn]] void fail(ErrorCode code, std::string message, DiagnosticLocation location = {})
{ throw detail::CodecError{{code, Severity::Error, std::move(location), std::move(message)}}; }

template<class T, class Id> const T& by_id(const std::vector<T>& items, Id id, std::string_view what)
{
    const auto item = std::ranges::find(items, id, &T::id);
    if (item == items.end()) { fail(ErrorCode::UnknownTarget, std::string(what) + " was not found."); }
    return *item;
}

void check_document(const Solution& draft, const SolutionDocument* document)
{
    if (draft.format_version != solution_format_version) { fail(ErrorCode::UnsupportedSolutionFormat, "Unsupported host solution version."); }
    if (!document) { return; }
    if (!document->can_save()) { fail(ErrorCode::UnsupportedSolutionContent, "The source has unknown fields."); }
    if (draft.id != document->solution().id) { fail(ErrorCode::ResultIdentityMismatch, "The document is not the draft's file base."); }
}

// A Group's body is a self-contained workflow in the package. Only references
// to its immediate public inputs change owners when detaching/reinserting it.
void rebase_inputs(Workflow& flow, std::optional<NodeId> from, std::optional<NodeId> to)
{
    for (auto& node : flow.nodes) {
        for (auto& binding : node.bindings) {
            for (auto& source : binding.sources) {
                if (auto* ref = std::get_if<ScopedInputReference>(&source); ref && ref->workflow == flow.id) {
                    if (from && !ref->group) { fail(ErrorCode::ScopeViolation, "A Group body cannot capture its enclosing workflow's public inputs."); }
                    if (ref->group == from) { ref->group = to; }
                }
            }
        }
    }
}

Solution extract(const Solution& draft, WorkflowId workflow, std::optional<NodeId> group)
{
    const auto& source = by_id(draft.workflows, workflow, "Export workflow");
    auto flow = source;
    std::set<NodeId> kept;
    if (group) {
        const auto& root = by_id(source.nodes, *group, "Export Group");
        const auto* config = std::get_if<GroupConfiguration>(&root.configuration);
        if (!config || config->loop) { fail(ErrorCode::InvalidModel, "Export a workflow or an ordinary Group."); }
        const auto body = require(descendants(source, *group));
        kept.insert(body.begin(), body.end());
        for (const auto& edge : flow.control_links) {
            if (kept.contains(edge.source) != kept.contains(edge.target)) {
                fail(ErrorCode::ScopeViolation, "Control connections cannot cross the exported Group's boundary.");
            }
        }
        std::erase_if(flow.nodes, [&](const auto& n) { return !kept.contains(n.id); });
        std::erase_if(flow.control_links, [&](const auto& e) { return !kept.contains(e.source) || !kept.contains(e.target); });
        std::erase_if(flow.containment, [&](const auto& e) { return !kept.contains(e.parent) || !kept.contains(e.child); });
        flow.name = root.name;
        flow.interface = config->interface;
        flow.entries = config->entries;
        flow.outputs = config->outputs;
        flow.control_exports = config->control_exports;
        flow.enabled = root.enabled;
        rebase_inputs(flow, group, {});
    }
    Solution model{draft.id, flow.name, draft.format_version, 1, {std::move(flow)}};
    const auto layout = std::ranges::find(draft.layouts, workflow, &WorkflowLayout::workflow);
    if (layout != draft.layouts.end()) {
        model.layouts.push_back(*layout);
        if (group) { std::erase_if(model.layouts.back().nodes, [&](const auto& n) { return !kept.contains(n.node); }); }
    }
    return model;
}

struct HostServices {
    std::map<VariableId, DataType> variables;
    std::map<ResourceId, ResourceKind> resources;
    bool node_state{};
    bool file_leases{};
};

struct Package {
    Solution model;
    bool group;
    HostServices services;
    std::map<ResourceId, std::string> digests;
};

SubgraphHostCapabilities required_capabilities(const Package& pack, const ToolRegistry& registry)
{
    SubgraphHostCapabilities needs{false, !pack.model.resources.empty()};
    for (const auto& flow : pack.model.workflows) {
        for (const auto& node : flow.nodes) {
            const auto* config = std::get_if<ToolConfiguration>(&node.configuration);
            if (!config) { continue; }
            const auto* tool = registry.find_descriptor(config->type);
            if (!tool) { DiagnosticLocation where; where.node = node.id; where.tool = config->type;
                fail(ErrorCode::UnknownTool, "The package requires a tool missing from the host registry.", where); }
            needs.node_state |= tool->state.has_value();
            for (const auto& slot : tool->resources) {
                const bool file = slot.kind == ResourceKind::Image || slot.kind == ResourceKind::Mask ||
                    slot.kind == ResourceKind::Template || slot.kind == ResourceKind::Calibration || slot.kind == ResourceKind::Binary;
                if (file && (slot.required || std::ranges::find(node.resources, slot.slot, &NodeResourceBinding::slot) != node.resources.end())) {
                    needs.file_leases = true;
                }
            }
        }
    }
    return needs;
}

ResourcePath* file_path(ResourceDefinition& resource)
{
    if (auto* managed = std::get_if<ManagedResource>(&resource.location)) { return &managed->path; }
    if (auto* file = std::get_if<ExternalFileResource>(&resource.location)) { return &file->path; }
    return nullptr;
}

void resolve_dependencies(Package& pack, const Solution& source)
{
    // The copier already walks defaults, ROI/mask/transform references, output
    // selectors and resource slots. Empty catalogs reveal all non-local references.
    const auto scan = require(clone_solution(pack.model, ExternalReferencePolicy::Preserve));
    std::set<ResourceId> resources;
    for (const auto& external : scan.report.preserved_references) {
        if (const auto* global = std::get_if<GlobalReference>(&external.reference)) {
            const auto& variable = by_id(source.variables, global->variable, "Required host variable");
            pack.services.variables.emplace(variable.id, variable.type);
        } else if (const auto* ref = std::get_if<ResourceReference>(&external.reference)) {
            require(validate_resource_reference(*ref, source.resources));
            if (!resources.insert(ref->id).second) { continue; }
            auto resource = by_id(source.resources, ref->id, "Required resource");
            if (file_path(resource)) { pack.model.resources.push_back(std::move(resource)); }
            else { pack.services.resources.emplace(resource.id, resource.kind); }
        } else {
            DiagnosticLocation where; where.node = external.owner;
            fail(ErrorCode::ScopeViolation, "A package cannot capture a node or public input outside its boundary.", where);
        }
    }
}

// Validate against the host's own declarations. Only the identity and Schema
// of a required host service are serialized; the host owns its data and state.
void validate_package(const Package& pack, const Solution& host, const ToolRegistry& registry)
{
    auto candidate = pack.model;
    if (candidate.workflows.size() != 1 || candidate.format_version != solution_format_version
        || !candidate.variables.empty() || !candidate.queues.empty()) {
        fail(ErrorCode::InvalidModel, "A reusable package must contain exactly one workflow body and no embedded host state.");
    }
    for (const auto& [id, type] : pack.services.variables) {
        const auto it = std::ranges::find(host.variables, id, &GlobalVariableDefinition::id);
        if (it == host.variables.end() || it->type != type) {
            DiagnosticLocation where; where.variable = id;
            fail(ErrorCode::MissingCapability, "Host variable with the declared identity and type is required.", where);
        }
        candidate.variables.push_back(*it);
    }
    for (const auto& [id, kind] : pack.services.resources) {
        const auto it = std::ranges::find(host.resources, id, &ResourceDefinition::id);
        if (it == host.resources.end() || it->kind != kind) {
            DiagnosticLocation where; where.resource = id;
            fail(ErrorCode::MissingCapability, "A host resource with the declared identity and kind is required.", where);
        }
        candidate.resources.push_back(*it);
    }
    require(validate_solution(candidate, registry.descriptors()));
}

Json encode(const Package& pack)
{
    Json variables = Json::array(), resources = Json::array(), files = Json::array();
    for (const auto& [id, type] : pack.services.variables) {
        variables.push_back({{"id", detail::write_id(id)}, {"type", detail::encode_type(type)}});
    }
    for (const auto& [id, kind] : pack.services.resources) {
        resources.push_back({{"id", detail::write_id(id)}, {"kind", detail::write_enum(kind)}});
    }
    for (const auto& [id, digest] : pack.digests) {
        files.push_back({{"id", detail::write_id(id)}, {"sha256", digest}});
    }
    return {{"format", "visionpro.subgraph"}, {"version", 1}, {"kind", pack.group ? "group" : "workflow"},
        {"model", detail::encode_solution(pack.model)}, {"files", std::move(files)},
        {"host", {{"variables", std::move(variables)}, {"resources", std::move(resources)},
            {"node_state", pack.services.node_state}, {"file_leases", pack.services.file_leases}}}};
}

Package decode(const fs::path& directory)
{
    const auto json = detail::parse_manifest(require(detail::read_manifest(directory / subgraph_manifest_name)));
    std::vector<Diagnostic> unknown;
    const Reader root(json, unknown);
    root.object({"format", "version", "kind", "model", "files", "host"});
    if (root.field("format").text() != "visionpro.subgraph" || root.field("version").u32() != 1) {
        fail(ErrorCode::UnsupportedSolutionFormat, "Unsupported subgraph package format or version.");
    }
    const auto kind = root.field("kind").text();
    if (kind != "group" && kind != "workflow") { fail(ErrorCode::UnsupportedSolutionContent, "Unknown subgraph package kind."); }
    Package pack{detail::decode_solution(root.field("model")), kind == "group", {}, {}};
    const auto host = root.field("host");
    host.object({"variables", "resources", "node_state", "file_leases"});
    pack.services.node_state = host.field("node_state").boolean();
    pack.services.file_leases = host.field("file_leases").boolean();
    const auto variables = host.field("variables"), resources = host.field("resources"), files = root.field("files");
    for (std::size_t i = 0; i < variables.size(); ++i) {
        const auto service = variables.at(i); service.object({"id", "type"});
        if (!pack.services.variables.emplace(detail::read_id<VariableId>(service.field("id")),
                detail::decode_type(service.field("type"))).second) { fail(ErrorCode::DuplicateIdentity, "Repeated host variable requirement."); }
    }
    for (std::size_t i = 0; i < resources.size(); ++i) {
        const auto service = resources.at(i); service.object({"id", "kind"});
        if (!pack.services.resources.emplace(detail::read_id<ResourceId>(service.field("id")),
                detail::read_enum<ResourceKind>(service.field("kind"))).second) { fail(ErrorCode::DuplicateIdentity, "Repeated host resource requirement."); }
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto file = files.at(i); file.object({"id", "sha256"});
        if (!pack.digests.emplace(detail::read_id<ResourceId>(file.field("id")), file.field("sha256").text()).second) {
            fail(ErrorCode::DuplicateIdentity, "Repeated package asset identity.");
        }
    }
    if (!unknown.empty()) { throw detail::CodecError{unknown.front()}; }
    return pack;
}

class CoordinateUses {
public:
    std::set<CoordinateSystemId> ids;
    void transform(const Transform2D& t) { ids.insert(t.from); ids.insert(t.to); }
    void region_source(const RegionSource& source)
    {
        std::visit([&](const auto& item) {
            ids.insert(item.coordinate_frame);
            if (item.transform) { transform(*item.transform); }
            if constexpr (std::same_as<std::decay_t<decltype(item)>, RegionPrimitive>) {
                if (const auto* mask = std::get_if<MaskConfig>(&item.geometry)) {
                    ids.insert(mask->mask_frame); ids.insert(mask->mapping.target_frame);
                    if (mask->mapping.transform) { transform(*mask->mapping.transform); }
                }
            }
        }, source);
    }
    void value(const ConfigValue& v)
    {
        if (const auto* p = v.as_point2d()) { ids.insert(p->coordinate_frame); }
        if (const auto* p = v.as_pose2d()) { ids.insert(p->position.coordinate_frame); }
        if (const auto* p = v.as_circle2d()) { ids.insert(p->center.coordinate_frame); }
        if (const auto* p = v.as_line2d()) { ids.insert(p->start.coordinate_frame); ids.insert(p->end.coordinate_frame); }
        if (const auto* t = v.as_transform2d()) { transform(*t); }
        if (const auto* r = v.as_region2d()) {
            ids.insert(r->coordinate_frame); region_source(r->source);
            for (const auto& s : r->exclusions) { region_source(s); }
        }
        if (const auto* fields = v.fields()) { for (const auto& f : *fields) { value(f.value); } }
        if (const auto* elements = v.elements()) { for (const auto& e : *elements) { value(e); } }
        if (const auto* child = v.optional_value()) { value(*child); }
    }
    template<class Port> void port(const Port& p)
    {
        if (p.semantics.coordinate_system) { ids.insert(*p.semantics.coordinate_system); }
        if constexpr (requires { p.default_value; }) {
            if (p.default_value) { value(*p.default_value); }
            for (const auto& allowed : p.constraints.allowed_values) { value(allowed); }
        }
    }
    void interface(const PublicInterface& ports)
    {
        for (const auto& i : ports.inputs) { port(i); }
        for (const auto& o : ports.outputs) { port(o); }
    }
};

void collect_coordinates(Package& package, const Solution& draft, const ToolRegistry& registry)
{
    CoordinateUses seen;
    const auto& flow = package.model.workflows.front();
    seen.interface(flow.interface);
    for (const auto& node : flow.nodes) {
        if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) { seen.interface(group->interface); }
        else if (const auto* tool = registry.find_descriptor(std::get<ToolConfiguration>(node.configuration).type)) {
            for (const auto& p : tool->inputs) { seen.port(p); }
            for (const auto& p : tool->parameters) { seen.port(p); }
            for (const auto& p : tool->outputs) { seen.port(p); }
            if (tool->state) { seen.value(tool->state->initial_value); }
        }
        for (const auto& binding : node.bindings) {
            for (const auto& source : binding.sources) {
                if (const auto* v = std::get_if<ConfigValue>(&source)) { seen.value(*v); }
            }
        }
    }
    for (const auto& id : seen.ids) { package.model.coordinates.push_back(by_id(draft.coordinates, id, "Coordinate frame")); }
}

bool equal_digest(std::string_view left, std::string_view right)
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'F' ? static_cast<char>(c + ('a' - 'A')) : c; };
    return left.size() == 64 && std::ranges::equal(left, right, {}, lower, lower);
}

void copy_assets(Package& pack, const detail::ResourcePaths& from, const fs::path& destination,
    detail::PackageAssets& transaction, const IdentityRemap* remap)
{
    const bool importing = remap != nullptr;
    if (importing && pack.digests.size() != pack.model.resources.size()) {
        fail(ErrorCode::InvalidResource, "Every bundled asset needs exactly one digest.");
    }
    if (pack.model.resources.empty()) { return; }
    const auto directory = "assets-" + require(IdFactory::generate<MessageId>()).to_string();
    require(transaction.create_directory(destination / directory, true));
    for (auto& resource : pack.model.resources) {
        DiagnosticLocation where; where.resource = resource.id;
        auto* path = file_path(resource);
        if (!path) { fail(ErrorCode::InvalidResource, "Only regular file assets may be embedded.", where); }
        const auto* managed = std::get_if<ManagedResource>(&resource.location);
        if (importing && (path->base != PathBase::SolutionDirectory || path->named_root)) {
            fail(ErrorCode::InvalidPath, "Bundled assets must be located inside the package.", where);
        }
        try {
            const auto actual = require(detail::existing_path(*path, from, importing || managed));
            const auto contents = require(read_file_bytes(actual));
            const auto digest = require(detail::sha256(*contents));
            const auto expected = managed ? std::optional{managed->sha256} : std::get<ExternalFileResource>(resource.location).expected_sha256;
            if (expected && !equal_digest(digest, *expected)) { fail(ErrorCode::ResourceVersionMismatch, "The asset differs from its declared digest."); }
            if (importing) {
                const auto saved = pack.digests.find(resource.id);
                if (saved == pack.digests.end() || !equal_digest(digest, saved->second)) {
                    fail(ErrorCode::ResourceVersionMismatch, "The package asset failed its integrity check.");
                }
            } else { pack.digests.emplace(resource.id, digest); }
            const auto new_id = remap ? remap->resources.at(resource.id) : resource.id;
            auto filename = fs::path(new_id.to_string());
            if (importing && managed) { filename = detail::imported_filename(new_id, *managed); }
            else { filename += actual.extension(); }
            const auto relative = fs::path(directory) / filename;
            require(transaction.write(destination / relative, *contents));
            const auto utf8 = relative.generic_u8string();
            *path = {PathBase::SolutionDirectory, {utf8.begin(), utf8.end()}, {}};
        } catch (detail::CodecError& e) { e.diagnostic.location.resource = resource.id; throw; }
    }
}

SubgraphImport insert(const Package& pack, SolutionCopy copied, const Solution& host, std::optional<WorkflowId> target,
    std::optional<NodeId> parent)
{
    if (pack.group && !target) { fail(ErrorCode::UnknownTarget, "Select a target workflow for the imported Group."); }
    auto& fragment = copied.solution;
    for (const auto& original : pack.model.resources) {
        auto found = std::ranges::find(fragment.resources, copied.identities.resources.at(original.id), &ResourceDefinition::id);
        found->location = original.location;
    }
    auto& incoming = fragment.workflows.front();
    SubgraphImport result{host, incoming.id, {}};
    auto& model = result.solution;
    for (const auto& frame : fragment.coordinates) {
        auto found = std::ranges::find(model.coordinates, frame.id, &CoordinateFrameDefinition::id);
        if (found == model.coordinates.end()) { model.coordinates.push_back(frame); }
        else if (*found != frame) { fail(ErrorCode::CoordinateMismatch, "The target defines this coordinate identity differently."); }
    }
    model.resources.insert(model.resources.end(), fragment.resources.begin(), fragment.resources.end());
    if (pack.group) {
        auto flow = std::ranges::find(model.workflows, *target, &Workflow::id);
        if (flow == model.workflows.end()) { fail(ErrorCode::UnknownTarget, "The target workflow does not exist."); }
        if (parent && !std::holds_alternative<GroupConfiguration>(by_id(flow->nodes, *parent, "Parent Group").configuration)) {
            fail(ErrorCode::InvalidContainment, "An imported Group must have a Group parent or use the workflow root.");
        }
        result.group = require(IdFactory::generate<NodeId>());
        const auto top = require(children(incoming));
        rebase_inputs(incoming, {}, result.group);
        flow->nodes.push_back({*result.group, incoming.name, GroupConfiguration{incoming.interface, incoming.entries, incoming.outputs,
            incoming.control_exports}, {}, {}, incoming.enabled});
        flow->nodes.insert(flow->nodes.end(), incoming.nodes.begin(), incoming.nodes.end());
        flow->control_links.insert(flow->control_links.end(), incoming.control_links.begin(), incoming.control_links.end());
        flow->containment.insert(flow->containment.end(), incoming.containment.begin(), incoming.containment.end());
        for (const auto& child : top) { flow->containment.push_back({*result.group, child}); }
        if (parent) { flow->containment.push_back({*parent, *result.group}); }
    } else { model.workflows.push_back(std::move(incoming)); }
    for (const auto& layout : fragment.layouts) {
        const auto found = std::ranges::find(model.layouts, layout.workflow, &WorkflowLayout::workflow);
        if (found == model.layouts.end()) { model.layouts.push_back(layout); }
        else { found->nodes.insert(found->nodes.end(), layout.nodes.begin(), layout.nodes.end()); }
    }
    require(validate_model_shape(model));
    return result;
}

} // namespace

core::Result<std::monostate> export_subgraph_package(const fs::path& directory, const Solution& draft,
    WorkflowId workflow, std::optional<NodeId> group, const ToolRegistry& registry, const SolutionDocument* source)
{
    try {
        check_document(draft, source);
        if (!directory.is_absolute()) { fail(ErrorCode::InvalidPath, "Use an absolute package directory."); }
        const auto target = detail::windows_display_path(fs::weakly_canonical(detail::windows_access_path(directory)));
        const auto manifest = target / subgraph_manifest_name;
        if (fs::exists(detail::windows_access_path(manifest))) { fail(ErrorCode::SolutionAlreadyExists, "The subgraph package already exists."); }
        Package pack{extract(draft, workflow, group), group.has_value(), {}, {}};
        resolve_dependencies(pack, draft);
        collect_coordinates(pack, draft, registry);
        const auto needs = required_capabilities(pack, registry);
        pack.services.node_state = needs.node_state;
        pack.services.file_leases = needs.file_leases;
        validate_package(pack, draft, registry);
        const auto from = source ? require(detail::resource_paths(source->directory(), source->resource_roots()))
                                 : require(detail::resource_paths({}, {}));
        detail::PackageAssets assets(draft.id);
        require(assets.create_directory(target, false));
        copy_assets(pack, from, target, assets, nullptr);
        const auto temporary = target / ("subgraph-" + require(IdFactory::generate<MessageId>()).to_string() + ".tmp");
        const auto text = detail::serialize_manifest(encode(pack));
        require(assets.write(temporary, std::as_bytes(std::span(text))));
        require(detail::publish_file(temporary, manifest, false));
        assets.confirm_publication();
        return std::monostate{};
    } catch (detail::CodecError& e) { return std::move(e.diagnostic); }
    catch (const std::exception& e) { return Diagnostic{ErrorCode::SolutionIoFailure, Severity::Error, {}, e.what()}; }
}

core::Result<SubgraphImport> import_subgraph_package(const fs::path& directory, const Solution& draft,
    std::optional<WorkflowId> target, std::optional<NodeId> parent, const ToolRegistry& registry, const SolutionDocument* host,
    SubgraphHostCapabilities capabilities)
{
    try {
        check_document(draft, host);
        const auto from = require(detail::resource_paths(directory, {}));
        auto pack = decode(from.directory);
        const auto needs = required_capabilities(pack, registry);
        if (pack.services.node_state != needs.node_state || pack.services.file_leases != needs.file_leases) {
            fail(ErrorCode::InvalidModel, "The host-service manifest does not match its tool and resource dependencies.");
        }
        if (needs.node_state && !capabilities.node_state) { fail(ErrorCode::MissingCapability, "The package requires a host node state service."); }
        if (needs.file_leases && !capabilities.file_leases) { fail(ErrorCode::MissingCapability, "The package requires host file resource leases."); }
        validate_package(pack, draft, registry);
        auto copied = require(clone_solution(pack.model, ExternalReferencePolicy::Preserve, pack.group ? target : std::nullopt));
        if (!host && !pack.model.resources.empty()) { fail(ErrorCode::InvalidPath, "Save the target solution before importing file assets."); }
        detail::PackageAssets assets(draft.id);
        copy_assets(pack, from, host ? host->directory() : fs::path{}, assets, &copied.identities);
        auto result = insert(pack, std::move(copied), draft, target, parent);
        require(validate_public_catalog(result.solution, registry.descriptors()));
        assets.confirm_publication();
        return result;
    } catch (detail::CodecError& e) { return std::move(e.diagnostic); }
    catch (const std::exception& e) { return Diagnostic{ErrorCode::SolutionIoFailure, Severity::Error, {}, e.what()}; }
}

} // namespace visionpro::infrastructure
