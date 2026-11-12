#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/execution_plan.hpp>

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>

namespace visionpro::infrastructure {
namespace {
namespace fs = std::filesystem;
using namespace core;
using namespace workflow;
using Check = Result<std::monostate>;

constexpr std::array file_kinds{ResourceKind::Image, ResourceKind::Mask, ResourceKind::Template,
    ResourceKind::Calibration, ResourceKind::Binary};

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

struct ResourcePaths {
    fs::path directory;
    std::map<SchemaId, fs::path> roots;
};

Result<fs::path> absolute_path(const ResourcePath& declaration, const ResourcePaths& paths, bool managed)
{
    if (auto checked = validate_resource_path(declaration, managed);
        const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    auto spelling = declaration.path;
    std::ranges::replace(spelling, '\\', '/');
    fs::path path{std::u8string(spelling.begin(), spelling.end())};
    if (declaration.base == PathBase::Absolute) {
        if (path.is_absolute()) { return path; }
        return error(ErrorCode::InvalidPath, "Absolute resource path requires an explicit filesystem root.");
    }
    if (path.has_root_path()) { return error(ErrorCode::InvalidPath, "Relative resource path has a filesystem root."); }
    if (declaration.base == PathBase::SolutionDirectory) { return paths.directory / path; }
    const auto root = paths.roots.find(*declaration.named_root);
    if (root == paths.roots.end()) {
        return error(ErrorCode::ResourceNotFound, "Named resource root is not configured: "
            + std::string(declaration.named_root->text()));
    }
    return root->second / path;
}

Result<fs::path> existing_path(const ResourcePath& declaration, const ResourcePaths& paths,
    bool managed = false, bool directory = false)
{
    const auto located = absolute_path(declaration, paths, managed);
    if (const auto* invalid = std::get_if<Diagnostic>(&located)) { return *invalid; }
    std::error_code io;
    auto canonical = fs::canonical(std::get<fs::path>(located), io);
    if (io == std::errc::no_such_file_or_directory || io == std::errc::not_a_directory) {
        return error(ErrorCode::ResourceNotFound, "Resource path is missing: " + declaration.path);
    }
    if (io) { return error(ErrorCode::InvalidPath, "Cannot resolve resource path: " + declaration.path); }
    if (managed && std::mismatch(paths.directory.begin(), paths.directory.end(),
        canonical.begin(), canonical.end()).first != paths.directory.end()) {
        return error(ErrorCode::InvalidPath, "Managed resource resolves outside the solution directory: " + declaration.path);
    }
    const auto status = fs::status(canonical, io);
    if (io || (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))) {
        return error(ErrorCode::InvalidResource, (directory ? "Resource requires a directory: " : "Resource requires a regular file: ")
            + declaration.path);
    }
    return canonical;
}

struct FileDeclaration {
    const ResourcePath& path;
    std::optional<std::string_view> digest;
    std::optional<std::uint64_t> revision;
    bool managed;
    bool locked;
};

std::optional<FileDeclaration> file_declaration(const ResourceDefinition& resource)
{
    if (const auto* managed = std::get_if<ManagedResource>(&resource.location)) {
        return FileDeclaration{managed->path, managed->sha256, managed->revision, true, true};
    }
    if (const auto* external = std::get_if<ExternalFileResource>(&resource.location)) {
        return FileDeclaration{external->path, external->expected_sha256, external->revision,
            false, external->read_policy == ResourceReadPolicy::SnapshotLocked};
    }
    return std::nullopt;
}

bool same_file(const FileDeclaration& left, const FileDeclaration& right)
{
    return left.path.base == right.path.base && left.path.path == right.path.path
        && left.path.named_root == right.path.named_root && left.digest == right.digest
        && left.revision == right.revision && left.managed == right.managed && left.locked == right.locked;
}

struct Sha256Handles {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};

    ~Sha256Handles()
    {
        if (hash) { BCryptDestroyHash(hash); }
        if (algorithm) { BCryptCloseAlgorithmProvider(algorithm, 0); }
    }
};

Check check_digest(std::span<std::byte> bytes, std::string_view expected)
{
    const auto failure = [] {
        return error(ErrorCode::ExecutionBoundaryFailure, "Windows CNG could not compute the resource SHA-256 digest.");
    };
    Sha256Handles handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0
        || BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) != 0) { return failure(); }
    while (!bytes.empty()) {
        const auto count = std::min<std::size_t>(bytes.size(), std::numeric_limits<ULONG>::max());
        if (BCryptHashData(handles.hash, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(count), 0) != 0) {
            return failure();
        }
        bytes = bytes.subspan(count);
    }
    std::array<UCHAR, 32> hash{};
    if (BCryptFinishHash(handles.hash, hash.data(), static_cast<ULONG>(hash.size()), 0) != 0) { return failure(); }
    constexpr std::string_view hex = "0123456789abcdef";
    std::array<char, 64> digest{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
        digest[2 * index] = hex[hash[index] >> 4];
        digest[2 * index + 1] = hex[hash[index] & 0xf];
    }
    if (!std::ranges::equal(digest, expected, [](char actual, char wanted) {
        return actual == (wanted >= 'A' && wanted <= 'F' ? static_cast<char>(wanted + ('a' - 'A')) : wanted);
    })) {
        return error(ErrorCode::ResourceVersionMismatch, "File resource SHA-256 does not match its declared content.");
    }
    return std::monostate{};
}

struct FileContents {
    fs::path path;
    std::shared_ptr<const std::vector<std::byte>> bytes;
};

Result<FileContents> read_file(const FileDeclaration& file, const ResourcePaths& paths)
{
    auto located = existing_path(file.path, paths, file.managed);
    if (const auto* invalid = std::get_if<Diagnostic>(&located)) { return *invalid; }
    auto path = std::get<fs::path>(std::move(located));
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = static_cast<std::streamoff>(input.tellg());
    if (length < 0) { return error(ErrorCode::InvalidResource, "Cannot open file resource: " + file.path.path); }
    std::vector<std::byte> bytes;
    const auto size = static_cast<std::uintmax_t>(length);
    if (size > bytes.max_size() || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return error(ErrorCode::SizeOverflow, "File resource exceeds addressable storage: " + file.path.path);
    }
    bytes.resize(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.peek() != std::char_traits<char>::eof() || input.bad()) {
        return error(ErrorCode::InvalidResource, "File resource could not be read at a consistent size: " + file.path.path);
    }
    if (file.digest) {
        if (auto checked = check_digest(bytes, *file.digest); const auto* invalid = std::get_if<Diagnostic>(&checked)) {
            return *invalid;
        }
    }
    return FileContents{std::move(path), std::make_shared<const std::vector<std::byte>>(std::move(bytes))};
}

Check probe(const ResourceDefinition& resource, const ResourcePaths& paths)
{
    if (const auto file = file_declaration(resource)) {
        const auto path = existing_path(file->path, paths, file->managed);
        if (const auto* invalid = std::get_if<Diagnostic>(&path)) { return *invalid; }
    } else if (const auto* sequence = std::get_if<ImageSequenceResource>(&resource.location)) {
        if (const auto* directory = std::get_if<DirectorySelection>(&sequence->selection)) {
            const auto path = existing_path(directory->directory, paths, false, true);
            if (const auto* invalid = std::get_if<Diagnostic>(&path)) { return *invalid; }
        } else {
            for (const auto& file_path : std::get<std::vector<ResourcePath>>(sequence->selection)) {
                const auto path = existing_path(file_path, paths);
                if (const auto* invalid = std::get_if<Diagnostic>(&path)) {
                    if (sequence->missing == SequenceMissing::Fail || invalid->code != ErrorCode::ResourceNotFound) {
                        return *invalid;
                    }
                }
            }
        }
    } else if (const auto* output = std::get_if<OutputPathResource>(&resource.location)) {
        const auto path = absolute_path(output->directory, paths, false);
        if (const auto* invalid = std::get_if<Diagnostic>(&path)) { return *invalid; }
    }
    return std::monostate{};
}

struct Entry {
    ResourceDefinition definition;
    std::optional<Result<FileContents>> locked;
};

using ResourceEntries = std::map<ResourceId, Entry>;

Result<const Entry*> find_entry(const ResourceEntries& entries, const ResourceReference& reference)
{
    if (auto checked = validate_resource_reference(reference);
        const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    const auto entry = entries.find(reference.id);
    if (entry == entries.end()) { return error(ErrorCode::ResourceNotFound, "File resource is absent from the captured catalog."); }
    if (entry->second.definition.kind != reference.kind) {
        return error(ErrorCode::ResourceKindMismatch, "Resource kind differs from the captured definition.");
    }
    const auto file = file_declaration(entry->second.definition);
    if (reference.revision && (!file || file->revision != reference.revision)) {
        return error(ErrorCode::ResourceVersionMismatch, "Pinned resource revision is unavailable.");
    }
    return &entry->second;
}

template<class Inspect> Check inspect_transform(const std::optional<Transform2D>& transform, const Inspect& inspect)
{
    return transform && transform->calibration ? inspect(*transform->calibration) : Check{std::monostate{}};
}

template<class Inspect> Check inspect_region_source(const RegionSource& source, const Inspect& inspect)
{
    if (const auto* primitive = std::get_if<RegionPrimitive>(&source)) {
        if (const auto* mask = std::get_if<MaskConfig>(&primitive->geometry)) {
            if (auto checked = inspect(mask->resource); std::holds_alternative<Diagnostic>(checked)) { return checked; }
            if (auto checked = inspect_transform(mask->mapping.transform, inspect);
                std::holds_alternative<Diagnostic>(checked)) { return checked; }
        }
        return inspect_transform(primitive->transform, inspect);
    }
    return inspect_transform(std::get<InheritedRegion>(source).transform, inspect);
}

template<ValueDomain Domain, class Inspect> Check inspect_value(const BasicValue<Domain>& value, const Inspect& inspect)
{
    if (const auto* ref = value.as_resource_reference()) { return inspect(*ref); }
    if (const auto* transform = value.as_transform2d(); transform && transform->calibration) {
        return inspect(*transform->calibration);
    }
    if (const auto* region = value.as_region2d()) {
        if (auto checked = inspect_region_source(region->source, inspect);
            std::holds_alternative<Diagnostic>(checked)) { return checked; }
        for (const auto& exclusion : region->exclusions) {
            if (auto checked = inspect_region_source(exclusion, inspect);
                std::holds_alternative<Diagnostic>(checked)) { return checked; }
        }
    }
    if constexpr (Domain == ValueDomain::Data) {
        if (const auto* raster = value.as_raster_mask()) { return inspect_transform(raster->mapping.transform, inspect); }
    }
    if (const auto* payload = value.optional_value()) { return inspect_value(*payload, inspect); }
    if (const auto* elements = value.elements()) {
        for (std::size_t index = 0; index < elements->size(); ++index) {
            auto checked = inspect_value((*elements)[index], inspect);
            if (auto* failure = std::get_if<Diagnostic>(&checked)) {
                failure->location.path.insert(failure->location.path.begin(), DiagnosticElementIndex{index});
                return checked;
            }
        }
    }
    if (const auto* fields = value.fields()) {
        for (const auto& field : *fields) {
            auto checked = inspect_value(field.value, inspect);
            if (auto* failure = std::get_if<Diagnostic>(&checked)) {
                failure->location.path.insert(failure->location.path.begin(), field.id);
                return checked;
            }
        }
    }
    return std::monostate{};
}

const DataValue* selected_value(const DataValue& source, const Selector& selector)
{
    const auto* value = &source;
    for (const auto& step : selector) {
        if (const auto* field = std::get_if<FieldSelection>(&step)) {
            value = value->field(field->field);
        } else {
            const auto* elements = value->elements();
            const auto index = std::get<ElementSelection>(step).index;
            value = elements && index < elements->size() ? &(*elements)[index] : nullptr;
        }
        if (!value) { return nullptr; }
    }
    return value;
}

template<class Inspect> Check inspect_source(const BindingSource& source, const RunRequest& request, const Inspect& inspect)
{
    if (const auto* ref = std::get_if<ResourceReference>(&source)) { return inspect(*ref); }
    if (const auto* value = std::get_if<ConfigValue>(&source)) { return inspect_value(*value, inspect); }
    const DataValue* fixed = nullptr;
    if (const auto* input = std::get_if<ScopedInputReference>(&source)) {
        if (const auto* value = request.input(input->input)) { fixed = selected_value(value->value, input->selector); }
    } else if (const auto* global = std::get_if<GlobalReference>(&source)) {
        const auto globals = request.globals();
        const auto entry = std::ranges::find(globals, global->variable, &GlobalVariableEntry::variable);
        if (entry != globals.end() && (!global->required_revision || entry->revision == *global->required_revision)) {
            fixed = selected_value(entry->current_value, global->selector);
        }
    }
    return fixed ? inspect_value(*fixed, inspect) : Check{std::monostate{}};
}

void set_target(DiagnosticLocation& location, const BindingTarget& target)
{
    if (const auto* input = std::get_if<InputId>(&target)) { location.input = *input; }
    else { location.parameter = std::get<ParameterId>(target); }
}

template<class Inspect> Check inspect_node(const Node& node, const ToolExecutionConfiguration& prepared,
    const RunRequest& request, const Inspect& inspect)
{
    const auto& config = prepared.data();
    const bool independent = std::holds_alternative<NodeTarget>(request.target());
    DiagnosticLocation node_location;
    node_location.workflow = config.workflow;
    node_location.node = node.id;
    node_location.tool = config.tool.id;
    for (const auto& slot : config.resources) {
        if (auto checked = inspect(slot.resource, node_location); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    for (const auto& binding : node.bindings) {
        if (independent && std::holds_alternative<InputId>(binding.target)) { continue; }
        auto location = node_location;
        set_target(location, binding.target);
        for (const auto& source : binding.sources) {
            auto checked = inspect_source(source, request,
                [&](const auto& ref) { return inspect(ref, location); });
            if (std::holds_alternative<Diagnostic>(checked)) { return checked; }
        }
    }
    const auto check_default = [&](const auto& descriptor) -> Check {
        const BindingTarget target{descriptor.id};
        if (descriptor.default_value
            && std::ranges::find(node.bindings, target, &Binding::target) == node.bindings.end()) {
            auto location = node_location;
            set_target(location, target);
            return inspect_value(*descriptor.default_value,
                [&](const auto& ref) { return inspect(ref, location); });
        }
        return std::monostate{};
    };
    if (independent) {
        for (const auto& input : request.inputs()) {
            auto location = node_location;
            location.input = input.id;
            if (auto checked = inspect_value(input.value, [&](const auto& ref) { return inspect(ref, location); });
                std::holds_alternative<Diagnostic>(checked)) { return checked; }
        }
    } else {
        for (const auto& input : config.tool.inputs) {
            if (auto checked = check_default(input); std::holds_alternative<Diagnostic>(checked)) { return checked; }
        }
    }
    for (const auto& parameter : config.tool.parameters) {
        if (auto checked = check_default(parameter); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    const auto& state = prepared.effective_state();
    return state ? inspect_value(state->initial_value, [&](const auto& ref) { return inspect(ref, node_location); })
        : Check{std::monostate{}};
}
} // namespace

struct FileResourceProvider::Storage {
    SolutionId solution;
    ResourcePaths paths;
    ResourceEntries entries;
    std::vector<Diagnostic> diagnostics;
};

FileResourceLease::FileResourceLease(core::ResourceReference reference, fs::path path,
    std::shared_ptr<const std::vector<std::byte>> bytes)
    : ResourceLease(std::move(reference)), path_(std::move(path)), bytes_(std::move(bytes))
{}

FileResourceProvider::FileResourceProvider(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}

Result<std::shared_ptr<FileResourceProvider>> FileResourceProvider::create(const Solution& solution,
    const fs::path& solution_directory, std::span<const NamedResourceRoot> roots)
{
    const auto fail = [&](Diagnostic diagnostic) -> Result<std::shared_ptr<FileResourceProvider>> {
        diagnostic.location.solution = solution.id;
        return diagnostic;
    };
    try {
        if (auto checked = validate_resource_catalog(solution.resources);
            const auto* invalid = std::get_if<Diagnostic>(&checked)) { return fail(*invalid); }
        if (!solution_directory.is_absolute()) {
            return fail(error(ErrorCode::InvalidPath, "Solution directory must be an explicit absolute path."));
        }
        std::error_code io;
        auto base = fs::canonical(solution_directory, io);
        if (io || !fs::is_directory(base, io) || io) {
            return fail(error(ErrorCode::InvalidPath, "Solution directory must name an existing directory."));
        }
        auto storage = std::make_shared<Storage>(Storage{solution.id, {std::move(base), {}}, {}, {}});
        for (const auto& root : roots) {
            if (!root.directory.is_absolute()) {
                return fail(error(ErrorCode::InvalidPath, "Named resource roots must be explicit absolute paths."));
            }
            if (!storage->paths.roots.emplace(root.id, root.directory).second) {
                return fail(error(ErrorCode::DuplicateIdentity, "Named resource root is configured more than once."));
            }
        }
        const auto report = [&](Diagnostic failure, const ResourceId& resource) {
            failure.location.solution = solution.id;
            failure.location.resource = resource;
            storage->diagnostics.push_back(std::move(failure));
        };
        for (const auto& resource : solution.resources) {
            auto& entry = storage->entries.emplace(resource.id, Entry{resource, {}}).first->second;
            const auto file = file_declaration(resource);
            if (file && file->locked) {
                entry.locked = read_file(*file, storage->paths);
                if (const auto* invalid = std::get_if<Diagnostic>(&*entry.locked)) { report(*invalid, resource.id); }
            } else if (auto checked = probe(resource, storage->paths);
                const auto* invalid = std::get_if<Diagnostic>(&checked)) { report(*invalid, resource.id); }
        }
        return std::shared_ptr<FileResourceProvider>(new FileResourceProvider(std::move(storage)));
    } catch (const fs::filesystem_error&) {
        return fail(error(ErrorCode::InvalidPath, "Could not interpret a filesystem path in the resource catalog."));
    } catch (...) {
        return fail(error(ErrorCode::ExecutionBoundaryFailure, "Could not capture file resources."));
    }
}

std::span<const Diagnostic> FileResourceProvider::diagnostics() const noexcept { return storage_->diagnostics; }

std::span<const ResourceKind> FileResourceProvider::kinds() const noexcept { return file_kinds; }

Check FileResourceProvider::preflight(const ExecutionPlan& plan) const
{
    const auto& request = plan.request();
    const auto& solution = request.snapshot().solution();
    if (solution.id != storage_->solution) {
        auto failure = error(ErrorCode::InvalidResource, "File resource provider belongs to another solution.");
        failure.location.solution = solution.id;
        return failure;
    }
    try {
        std::set<std::tuple<ResourceId, ResourceKind, std::optional<std::uint64_t>>> checked_refs;
        const auto inspect = [&](const ResourceReference& ref, const DiagnosticLocation& location) -> Check {
            if (!checked_refs.emplace(ref.id, ref.kind, ref.revision).second) { return std::monostate{}; }
            const auto contextualize = [&](Diagnostic failure) {
                failure.location = location;
                failure.location.solution = solution.id;
                failure.location.resource = ref.id;
                return failure;
            };
            const auto found = find_entry(storage_->entries, ref);
            if (const auto* invalid = std::get_if<Diagnostic>(&found)) { return contextualize(*invalid); }
            const auto& entry = *std::get<const Entry*>(found);
            const auto current = std::ranges::find(solution.resources, ref.id, &ResourceDefinition::id);
            if (current == solution.resources.end()) {
                return contextualize(error(ErrorCode::ResourceNotFound, "Referenced resource is absent from the run snapshot."));
            }
            const auto captured_file = file_declaration(entry.definition);
            const auto current_file = file_declaration(*current);
            if (current->kind != ref.kind || captured_file.has_value() != current_file.has_value()
                || (captured_file && !same_file(*captured_file, *current_file))) {
                return contextualize(error(ErrorCode::ResourceVersionMismatch,
                    "Run snapshot resource declaration differs from the captured file resources."));
            }
            if (entry.locked) {
                if (const auto* invalid = std::get_if<Diagnostic>(&*entry.locked)) { return contextualize(*invalid); }
            } else if (auto checked = probe(*current, storage_->paths);
                const auto* invalid = std::get_if<Diagnostic>(&checked)) { return contextualize(*invalid); }
            return std::monostate{};
        };
        for (const auto& scope : plan.dependencies().scopes) {
            const auto& flow = *find_workflow(solution, scope.workflow);
            for (const auto& id : scope.reachable) {
                const auto& node = *find_node(flow, id);
                if (!node.enabled) { continue; }
                if (auto checked = inspect_node(node, plan.tool(id)->configuration(), request, inspect);
                    std::holds_alternative<Diagnostic>(checked)) { return checked; }
            }
        }
        return std::monostate{};
    } catch (...) {
        auto failure = error(ErrorCode::ExecutionBoundaryFailure, "Could not preflight resources for the selected execution plan.");
        failure.location.solution = solution.id;
        return failure;
    }
}

Result<std::shared_ptr<const ResourceLease>> FileResourceProvider::acquire(
    const ToolExecutionConfiguration& configuration, const ToolResourceBinding& binding)
{
    const auto fail = [&](Diagnostic diagnostic) -> Result<std::shared_ptr<const ResourceLease>> {
        diagnostic.location.solution = storage_->solution;
        diagnostic.location.workflow = configuration.data().workflow;
        diagnostic.location.node = configuration.data().node;
        diagnostic.location.tool = configuration.data().tool.id;
        diagnostic.location.resource = binding.resource.id;
        return diagnostic;
    };
    try {
        const auto found = find_entry(storage_->entries, binding.resource);
        if (const auto* invalid = std::get_if<Diagnostic>(&found)) { return fail(*invalid); }
        const auto& entry = *std::get<const Entry*>(found);
        const auto file = file_declaration(entry.definition);
        if (!file) { return fail(error(ErrorCode::MissingCapability, "Resource slot does not identify a file resource.")); }
        auto contents = entry.locked ? *entry.locked : read_file(*file, storage_->paths);
        if (const auto* invalid = std::get_if<Diagnostic>(&contents)) { return fail(*invalid); }
        auto captured = std::get<FileContents>(std::move(contents));
        return std::shared_ptr<const ResourceLease>(
            new FileResourceLease(binding.resource, std::move(captured.path), std::move(captured.bytes)));
    } catch (...) {
        return fail(error(ErrorCode::ExecutionBoundaryFailure, "Could not read file resource for invocation."));
    }
}

} // namespace visionpro::infrastructure
