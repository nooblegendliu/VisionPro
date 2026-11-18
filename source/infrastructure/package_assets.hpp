#pragma once

#include "windows_path.hpp"
#include "resource_files.hpp"
#include "solution_storage.hpp"

namespace visionpro::infrastructure::detail {
namespace fs = std::filesystem;

// Only directories and files created by this operation are rolled back.
class PackageAssets {
    using Check = core::Result<std::monostate>;
public:
    explicit PackageAssets(core::SolutionId solution) : solution_(solution) {}
    PackageAssets(const PackageAssets&) = delete;
    PackageAssets& operator=(const PackageAssets&) = delete;
    ~PackageAssets()
    {
        if (published_) { return; }
        std::error_code ignored;
        for (auto it = created_.rbegin(); it != created_.rend(); ++it) {
            if (it->owned) { fs::remove(detail::windows_access_path(it->path), ignored); }
        }
    }

    Check create_directory(const fs::path& directory, bool unique)
    {
        if (!unique) {
            if (fs::is_directory(windows_access_path(directory))) { return std::monostate{}; }
            const auto parent = directory.parent_path();
            if (parent.empty() || directory == parent) {
                return error(core::ErrorCode::InvalidPath, "The package directory has no available filesystem root.");
            }
            auto prepared = create_directory(parent, false);
            if (std::holds_alternative<core::Diagnostic>(prepared)) { return prepared; }
        }
        created_.push_back({directory});
        auto& item = created_.back();
        item.owned = fs::create_directory(detail::windows_access_path(directory));
        if (!item.owned && (unique || !fs::is_directory(windows_access_path(directory)))) {
            return error(core::ErrorCode::SolutionAlreadyExists, "The destination asset directory already exists.");
        }
        return std::monostate{};
    }

    Check copy(const fs::path& source, const fs::path& target, std::optional<std::string_view> digest)
    {
        created_.push_back({target});
        auto result = detail::copy_resource_file(source, target, digest);
        created_.back().owned = std::holds_alternative<std::monostate>(result);
        return result;
    }

    Check write(const fs::path& target, std::span<const std::byte> bytes)
    {
        created_.push_back({target});
        auto result = write_staged_file(target, bytes);
        created_.back().owned = std::holds_alternative<std::monostate>(result);
        return result;
    }
    void confirm_publication() noexcept { published_ = true; }

private:
    core::Diagnostic error(core::ErrorCode code, std::string message) const
    {
        core::DiagnosticLocation where; where.solution = solution_;
        return {code, core::Severity::Error, where, std::move(message)};
    }
    struct CreatedPath { fs::path path; bool owned{false}; };
    const core::SolutionId solution_;
    std::vector<CreatedPath> created_;
    bool published_{false};
};

} // namespace visionpro::infrastructure::detail
