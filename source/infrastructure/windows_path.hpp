#pragma once

#include <filesystem>
#include <string_view>

namespace visionpro::infrastructure::detail {

// Extended-length names disable Win32's slash and dot-segment normalization.
// Keep ordinary spellings in domain/configuration values and use this only at I/O.
inline std::filesystem::path windows_access_path(const std::filesystem::path& path)
{
    if (!path.is_absolute() || path.native().size() < 248) { return path; }
    auto normalized = path.lexically_normal().make_preferred().native();
    if (normalized.starts_with(LR"(\\?\)") || normalized.starts_with(LR"(\\.\)")) { return normalized; }
    if (normalized.starts_with(LR"(\\)")) { return LR"(\\?\UNC\)" + normalized.substr(2); }
    return LR"(\\?\)" + normalized;
}

inline std::filesystem::path windows_display_path(const std::filesystem::path& path)
{
    const auto& native = path.native();
    if (native.starts_with(LR"(\\?\UNC\)")) { return LR"(\\)" + native.substr(8); }
    if (native.starts_with(LR"(\\?\)") && native.size() >= 7 && native[5] == L':') { return native.substr(4); }
    return path;
}

} // namespace visionpro::infrastructure::detail
