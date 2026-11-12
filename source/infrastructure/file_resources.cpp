#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/execution_plan.hpp>

#include "managed_cleanup.hpp"
#include "solution_storage.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
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

Result<ResourcePaths> resource_paths(const fs::path& directory, std::span<const NamedResourceRoot> roots)
{
    if (!directory.is_absolute()) {
        return error(ErrorCode::InvalidPath, "Solution directory must be an explicit absolute path.");
    }
    std::error_code io;
    auto base = fs::canonical(directory, io);
    if (io || !fs::is_directory(base, io) || io) {
        return error(ErrorCode::InvalidPath, "Solution directory must name an existing directory.");
    }
    ResourcePaths paths{std::move(base), {}};
    for (const auto& root : roots) {
        if (!root.directory.is_absolute()) {
            return error(ErrorCode::InvalidPath, "Named resource roots must be explicit absolute paths.");
        }
        if (!paths.roots.emplace(root.id, root.directory).second) {
            return error(ErrorCode::DuplicateIdentity, "Named resource root is configured more than once.");
        }
    }
    return paths;
}

fs::path utf8_path(std::string_view spelling)
{ return fs::path(std::u8string(spelling.begin(), spelling.end())); }

Result<fs::path> absolute_path(const ResourcePath& declaration, const ResourcePaths& paths, bool managed)
{
    if (auto checked = validate_resource_path(declaration, managed);
        const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    auto spelling = declaration.path;
    std::ranges::replace(spelling, '\\', '/');
    auto path = utf8_path(spelling);
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

Check check_digest(std::span<const std::byte> bytes, std::string_view expected)
{
    auto digest = detail::sha256(bytes);
    if (auto* failure = std::get_if<Diagnostic>(&digest)) { return std::move(*failure); }
    if (!std::ranges::equal(std::get<std::string>(digest), expected, [](char actual, char wanted) {
        return actual == (wanted >= 'A' && wanted <= 'F' ? static_cast<char>(wanted + ('a' - 'A')) : wanted);
    })) {
        return error(ErrorCode::ResourceVersionMismatch, "File resource SHA-256 does not match its declared content.");
    }
    return std::monostate{};
}

class NativeFile final {
public:
    explicit NativeFile(HANDLE handle) noexcept : handle_(handle) {}
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    ~NativeFile() { if (valid()) { CloseHandle(handle_); } }

    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

bool missing_file(DWORD code) noexcept
{ return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_DELETE_PENDING; }

bool file_in_use(DWORD code) noexcept
{ return code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION || code == ERROR_USER_MAPPED_FILE; }

Diagnostic file_error(ErrorCode code, std::string_view action, std::string_view path, DWORD native_error)
{
    return error(code, std::string(action) + ": " + std::string(path)
        + " (Win32 error " + std::to_string(native_error) + ").");
}

struct FileContents {
    fs::path path;
    std::shared_ptr<const std::vector<std::byte>> bytes;
};

struct FileReadFailure {
    Diagnostic diagnostic;
    DWORD native_error{};
};

std::variant<std::vector<std::byte>, FileReadFailure> read_bytes(HANDLE handle, std::string_view path)
{
    const auto read_error = [&](std::string_view action, DWORD code) {
        return FileReadFailure{file_error(ErrorCode::InvalidResource, action, path, code), code};
    };
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle, &length)) { return read_error("Could not read the resource size", GetLastError()); }
    std::vector<std::byte> bytes;
    if (length.QuadPart < 0 || static_cast<std::uintmax_t>(length.QuadPart) > bytes.max_size()) {
        return FileReadFailure{error(ErrorCode::SizeOverflow, "File resource exceeds addressable storage: " + std::string(path))};
    }
    bytes.resize(static_cast<std::size_t>(length.QuadPart));
    constexpr std::size_t chunk_size = 1 << 20;
    std::size_t offset = 0;
    while (offset != bytes.size()) {
        const auto count = static_cast<DWORD>(std::min(bytes.size() - offset, chunk_size));
        DWORD read = 0;
        if (!ReadFile(handle, bytes.data() + offset, count, &read, nullptr)) {
            return read_error("Could not read the resource", GetLastError());
        }
        if (read == 0) { return read_error("Resource ended before its recorded size", ERROR_HANDLE_EOF); }
        offset += read;
    }
    std::byte extra{};
    DWORD read = 0;
    if (!ReadFile(handle, &extra, 1, &read, nullptr)) {
        const auto code = GetLastError();
        if (code != ERROR_HANDLE_EOF) { return read_error("Could not check the resource end", code); }
    } else if (read != 0) {
        return FileReadFailure{error(ErrorCode::InvalidResource, "File resource changed size during reading: " + std::string(path))};
    }
    return bytes;
}

Result<FileContents> read_file(const FileDeclaration& file, const ResourcePaths& paths)
{
    auto located = existing_path(file.path, paths, file.managed);
    if (const auto* invalid = std::get_if<Diagnostic>(&located)) { return *invalid; }
    auto path = std::get<fs::path>(std::move(located));
    NativeFile handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid()) {
        const auto code = GetLastError();
        return file_error(missing_file(code) ? ErrorCode::ResourceNotFound : ErrorCode::InvalidResource,
            "Could not open the resource", file.path.path, code);
    }
    auto contents = read_bytes(handle.get(), file.path.path);
    if (auto* failure = std::get_if<FileReadFailure>(&contents)) { return std::move(failure->diagnostic); }
    auto bytes = std::get<std::vector<std::byte>>(std::move(contents));
    if (file.digest) {
        if (auto checked = check_digest(bytes, *file.digest); const auto* invalid = std::get_if<Diagnostic>(&checked)) {
            return *invalid;
        }
    }
    return FileContents{std::move(path), std::make_shared<const std::vector<std::byte>>(std::move(bytes))};
}

using ProtectedFiles = std::set<fs::path>;

Result<std::optional<fs::path>> referenced_path(const ResourcePath& path, const ResourcePaths& paths,
    bool managed = false, bool directory = false)
{
    if (path.named_root && !paths.roots.contains(*path.named_root)) {
        return error(ErrorCode::ResourceNotFound, "Cannot check references through an unconfigured named root: "
            + std::string(path.named_root->text()));
    }
    auto resolved = existing_path(path, paths, managed, directory);
    if (auto* invalid = std::get_if<Diagnostic>(&resolved)) {
        if (invalid->code == ErrorCode::ResourceNotFound) { return std::optional<fs::path>{}; }
        return std::move(*invalid);
    }
    return std::optional<fs::path>{std::get<fs::path>(std::move(resolved))};
}

Check protect_file(ProtectedFiles& files, const ResourcePath& path, const ResourcePaths& paths, bool managed = false)
{
    auto resolved = referenced_path(path, paths, managed);
    if (auto* invalid = std::get_if<Diagnostic>(&resolved)) { return std::move(*invalid); }
    if (auto& file = std::get<std::optional<fs::path>>(resolved)) { files.insert(std::move(*file)); }
    return std::monostate{};
}

Check protect_sequence(ProtectedFiles& files, const DirectorySelection& selection, const ResourcePaths& paths)
{
    auto resolved = referenced_path(selection.directory, paths, false, true);
    if (auto* invalid = std::get_if<Diagnostic>(&resolved)) { return std::move(*invalid); }
    const auto& directory = std::get<std::optional<fs::path>>(resolved);
    if (!directory) { return std::monostate{}; }
    const auto search_path = *directory / utf8_path(selection.filename_pattern);
    WIN32_FIND_DATAW entry{};
    struct Search {
        HANDLE handle;
        ~Search() { if (handle != INVALID_HANDLE_VALUE) { FindClose(handle); } }
    } search{FindFirstFileW(search_path.c_str(), &entry)};
    const auto search_error = [&](DWORD code) {
        return file_error(ErrorCode::InvalidPath, "Could not check selected sequence files", selection.directory.path, code);
    };
    if (search.handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (missing_file(code)) { return std::monostate{}; }
        return search_error(code);
    }
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            files.insert(*directory / entry.cFileName);
        }
    } while (FindNextFileW(search.handle, &entry));
    const auto code = GetLastError();
    if (code != ERROR_NO_MORE_FILES) { return search_error(code); }
    return std::monostate{};
}

Result<ProtectedFiles> protected_managed_files(const Solution& solution, const ResourcePaths& paths)
{
    ProtectedFiles files;
    for (const auto& resource : solution.resources) {
        if (const auto* managed = std::get_if<ManagedResource>(&resource.location)) {
            auto checked = protect_file(files, managed->path, paths, true);
            if (auto* invalid = std::get_if<Diagnostic>(&checked)) { return std::move(*invalid); }
        }
    }
    return files;
}

Check protect_external_files(ProtectedFiles& files, const Solution& solution, const ResourcePaths& paths)
{
    for (const auto& resource : solution.resources) {
        if (const auto* external = std::get_if<ExternalFileResource>(&resource.location)) {
            auto checked = protect_file(files, external->path, paths);
            if (auto* invalid = std::get_if<Diagnostic>(&checked)) { return std::move(*invalid); }
        } else if (const auto* sequence = std::get_if<ImageSequenceResource>(&resource.location)) {
            if (const auto* selection = std::get_if<DirectorySelection>(&sequence->selection)) {
                auto checked = protect_sequence(files, *selection, paths);
                if (auto* invalid = std::get_if<Diagnostic>(&checked)) { return std::move(*invalid); }
            } else {
                for (const auto& path : std::get<std::vector<ResourcePath>>(sequence->selection)) {
                    auto checked = protect_file(files, path, paths);
                    if (auto* invalid = std::get_if<Diagnostic>(&checked)) { return std::move(*invalid); }
                }
            }
        }
    }
    return std::monostate{};
}

Result<bool> aliases_protected_file(const fs::path& path, const ProtectedFiles& files)
{
    if (files.contains(path)) { return true; }
    for (const auto& protected_path : files) {
        std::error_code io;
        if (fs::equivalent(path, protected_path, io)) { return true; }
        if (io && io != std::errc::no_such_file_or_directory && io != std::errc::not_a_directory) {
            return error(ErrorCode::InvalidPath, "Cannot compare the old file to a protected resource path.");
        }
    }
    return false;
}

Diagnostic cleanup_issue(Severity severity, std::string message)
{ return {ErrorCode::SolutionCleanupFailure, severity, {}, std::move(message)}; }

enum class CleanupOutcome { Unchanged, Deferred, Removed };

Result<CleanupOutcome> cleanup_io(std::string_view action, std::string_view path, DWORD code)
{
    if (missing_file(code)) { return CleanupOutcome::Unchanged; }
    if (file_in_use(code)) { return CleanupOutcome::Deferred; }
    return file_error(ErrorCode::SolutionCleanupFailure, action, path, code);
}

Result<CleanupOutcome> remove_verified_version(const ManagedResource& managed, const ResourcePaths& paths)
{
    auto located = absolute_path(managed.path, paths, true);
    if (auto* invalid = std::get_if<Diagnostic>(&located)) {
        return cleanup_issue(Severity::Error, std::move(invalid->message));
    }
    const auto& declared = std::get<fs::path>(located);
    auto component = paths.directory;
    const auto relative = declared.lexically_relative(paths.directory);
    if (relative.empty()) {
        return cleanup_issue(Severity::Error, "Could not check confinement of old managed file: " + managed.path.path);
    }
    for (const auto& name : relative) {
        component /= name;
        const auto attributes = GetFileAttributesW(component.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return cleanup_io("Could not inspect old managed file", managed.path.path, GetLastError());
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return cleanup_issue(Severity::Warning, "Old managed path traverses a reparse point: " + managed.path.path);
        }
    }
    // A single handle verifies content and disposes of the verified file.
    // Existing invocation pins prevent this handle from obtaining DELETE access.
    NativeFile file(CreateFileW(declared.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        return cleanup_io("Could not open old managed file for cleanup", managed.path.path, GetLastError());
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &info, sizeof(info))) {
        return cleanup_io("Could not inspect opened managed file", managed.path.path, GetLastError());
    }
    if (GetFileType(file.get()) != FILE_TYPE_DISK
        || (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return cleanup_issue(Severity::Warning, "Old managed path must identify a regular file: " + managed.path.path);
    }
    auto bytes = read_bytes(file.get(), managed.path.path);
    if (auto* failure = std::get_if<FileReadFailure>(&bytes)) {
        if (file_in_use(failure->native_error)) { return CleanupOutcome::Deferred; }
        return cleanup_issue(Severity::Error, std::move(failure->diagnostic.message));
    }
    auto checked = check_digest(std::get<std::vector<std::byte>>(bytes), managed.sha256);
    if (const auto* invalid = std::get_if<Diagnostic>(&checked)) {
        return cleanup_issue(Severity::Error, "Could not verify obsolete managed content for " + managed.path.path
            + ": " + invalid->message);
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
        return cleanup_io("Could not delete verified old managed file", managed.path.path, GetLastError());
    }
    return CleanupOutcome::Removed;
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

struct FileResourceLease::FilePin {
    explicit FilePin(const fs::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr))
    {}
    NativeFile handle;
};

FileResourceLease::FileResourceLease(core::ResourceReference reference, fs::path path,
    std::shared_ptr<const std::vector<std::byte>> bytes, bool managed)
    : ResourceLease(std::move(reference)), path_(std::move(path)), bytes_(std::move(bytes))
{
    if (managed) {
        // A loaded snapshot can still supply frozen bytes when the source file
        // has already disappeared. Only an acquired lease pins the disk file.
        auto pin = std::make_shared<const FilePin>(path_);
        if (pin->handle.valid()) { pin_ = std::move(pin); }
    }
}

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
        auto paths = resource_paths(solution_directory, roots);
        if (auto* invalid = std::get_if<Diagnostic>(&paths)) { return fail(std::move(*invalid)); }
        auto storage = std::make_shared<Storage>(Storage{solution.id, std::get<ResourcePaths>(std::move(paths)), {}, {}});
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
            new FileResourceLease(binding.resource, std::move(captured.path), std::move(captured.bytes), file->managed));
    } catch (...) {
        return fail(error(ErrorCode::ExecutionBoundaryFailure, "Could not read file resource for invocation."));
    }
}

Check detail::verify_managed_resource(const ManagedResource& resource, const fs::path& directory)
{
    auto paths = resource_paths(directory, {});
    if (auto* invalid = std::get_if<Diagnostic>(&paths)) { return std::move(*invalid); }
    const FileDeclaration file{resource.path, resource.sha256, resource.revision, true, true};
    auto verified = read_file(file, std::get<ResourcePaths>(paths));
    if (auto* invalid = std::get_if<Diagnostic>(&verified)) { return std::move(*invalid); }
    return std::monostate{};
}

SolutionCleanupReport detail::cleanup_managed_versions(const Solution& previous, const Solution& current,
    const fs::path& directory, std::span<const NamedResourceRoot> roots)
{
    SolutionCleanupReport report;
    const auto record = [&](Diagnostic issue, std::optional<ResourceId> resource) {
        if (issue.severity == Severity::Warning) { ++report.deferred; }
        issue.location.solution = previous.id;
        issue.location.resource = std::move(resource);
        report.diagnostics.push_back(std::move(issue));
    };
    auto resolved = resource_paths(directory, roots);
    if (auto* invalid = std::get_if<Diagnostic>(&resolved)) {
        record(cleanup_issue(Severity::Error, std::move(invalid->message)), std::nullopt);
        return report;
    }
    const auto& paths = std::get<ResourcePaths>(resolved);
    const auto retained_managed = protected_managed_files(current, paths);
    ProtectedFiles retained_external{paths.directory / solution_manifest_name, paths.directory / package_lock_name};
    std::optional<Check> external_check;
    const auto remove = [&](const ManagedResource& managed) -> Result<CleanupOutcome> {
        auto candidate = referenced_path(managed.path, paths, true);
        if (auto* invalid = std::get_if<Diagnostic>(&candidate)) {
            return cleanup_issue(Severity::Error, std::move(invalid->message));
        }
        const auto& path = std::get<std::optional<fs::path>>(candidate);
        if (!path) { return CleanupOutcome::Unchanged; }
        const auto deferred = [&](const Diagnostic& cause) {
            return cleanup_issue(Severity::Warning, "Could not establish that the old managed file is unreferenced: "
                + managed.path.path + ". " + cause.message);
        };
        if (const auto* invalid = std::get_if<Diagnostic>(&retained_managed)) { return deferred(*invalid); }
        auto still_managed = aliases_protected_file(*path, std::get<ProtectedFiles>(retained_managed));
        if (const auto* invalid = std::get_if<Diagnostic>(&still_managed)) { return deferred(*invalid); }
        if (std::get<bool>(still_managed)) { return CleanupOutcome::Unchanged; }
        if (!external_check) {
            external_check = protect_external_files(retained_external, previous, paths);
            if (std::holds_alternative<std::monostate>(*external_check)) {
                external_check = protect_external_files(retained_external, current, paths);
            }
        }
        if (const auto* invalid = std::get_if<Diagnostic>(&*external_check)) { return deferred(*invalid); }
        auto external = aliases_protected_file(*path, retained_external);
        if (const auto* invalid = std::get_if<Diagnostic>(&external)) { return deferred(*invalid); }
        if (std::get<bool>(external)) {
            return cleanup_issue(Severity::Warning, "Old managed file is shared with an external resource or package metadata: "
                + managed.path.path);
        }
        return remove_verified_version(managed, paths);
    };
    for (const auto& resource : previous.resources) {
        const auto* managed = std::get_if<ManagedResource>(&resource.location);
        if (!managed) { continue; }
        auto outcome = remove(*managed);
        if (auto* failure = std::get_if<Diagnostic>(&outcome)) { record(std::move(*failure), resource.id); }
        else if (std::get<CleanupOutcome>(outcome) == CleanupOutcome::Removed) { ++report.removed; }
        else if (std::get<CleanupOutcome>(outcome) == CleanupOutcome::Deferred) { ++report.deferred; }
    }
    return report;
}

} // namespace visionpro::infrastructure
