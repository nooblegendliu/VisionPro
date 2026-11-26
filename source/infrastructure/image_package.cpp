#include "image_archive_internal.hpp"
#include <visionpro/core/id_factory.hpp>
#include "windows_path.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <windows.h>
#include <compressapi.h>

namespace visionpro::infrastructure::archive_detail
{
std::string uuid()
{
    return std::get<core::MessageId>(core::IdFactory::generate<core::MessageId>()).to_string();
}
std::string utf8(const fs::path& p)
{
    auto s = p.u8string();
    return {s.begin(), s.end()};
}
fs::path path_from(std::string_view s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}
bool safe_name(std::string_view name)
{
    if (name.empty() || name.size() > 1024 || name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("/\\:\"<>|?*") != name.npos ||
        std::ranges::any_of(name, [](unsigned char c) { return c < 32; })) {
        return false;
    }
    auto stem = std::string(name.substr(0, name.find('.')));
    std::ranges::transform(stem, stem.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return stem != "CON" && stem != "NUL" && stem != "PRN" && stem != "AUX" &&
           !(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem.back() >= '0' &&
             stem.back() <= '9');
}
fs::path unique_path(const fs::path& desired)
{
    if (!fs::exists(desired)) {
        return desired;
    }
    for (unsigned i = 2; i != 0; ++i) {
        const auto next = desired.parent_path() /
                          (desired.stem().wstring() + L"-" + std::to_wstring(i) + desired.extension().wstring());
        if (!fs::exists(next)) {
            return next;
        }
    }
    throw std::runtime_error("Image file namespace exhausted");
}
ArchiveCheck atomic_file(const fs::path& path, std::span<const std::byte> bytes, bool replace)
{
    auto temp = path.parent_path() / (".visionpro-stage-" + uuid());
    auto written = detail::write_staged_file(temp, bytes);
    if (std::holds_alternative<core::Diagnostic>(written)) {
        return written;
    }
    auto published = detail::publish_file(temp, path, replace);
    if (std::holds_alternative<core::Diagnostic>(published)) {
        std::error_code ec;
        fs::remove(temp, ec);
    }
    return published;
}
namespace
{
constexpr std::array<char, 8> magic{'V', 'P', 'I', 'M', 'G', 'D', 'A', '1'};
constexpr std::uint64_t max_entry_size = 512ULL * 1024 * 1024;
void put(std::ostream& stream, std::uint64_t value, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        stream.put(static_cast<char>(value & 255));
        value >>= 8;
    }
}
std::uint64_t get(std::istream& stream, unsigned n)
{
    std::uint64_t value{};
    for (unsigned i = 0; i < n; ++i) {
        const auto c = stream.get();
        if (c == EOF) {
            throw std::runtime_error("Truncated package header");
        }
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(c)) << (i * 8);
    }
    return value;
}
std::string string_field(std::istream& s, std::size_t n)
{
    if (!n || n > 1024) {
        throw std::runtime_error("Invalid package text length");
    }
    std::string str(n, '\0');
    if (!s.read(str.data(), static_cast<std::streamsize>(n))) {
        throw std::runtime_error("Truncated package text");
    }
    return str;
}
struct Codec {
    COMPRESSOR_HANDLE compressor{};
    DECOMPRESSOR_HANDLE decompressor{};
    ~Codec()
    {
        if (compressor)
            CloseCompressor(compressor);
        if (decompressor)
            CloseDecompressor(decompressor);
    }
};
struct Entry {
    ImagePackageEntry info;
    std::uint64_t offset{}, compressed{};
    std::string digest;
};
struct PackageReader {
    std::ifstream stream;
    std::vector<Entry> entries;
    explicit PackageReader(const fs::path& file) : stream(file, std::ios::binary)
    {
        const auto total = fs::file_size(file);
        std::array<char, 8> tag{};
        if (!stream.read(tag.data(), tag.size()) || tag != magic) {
            throw std::runtime_error("Unsupported image package format");
        }
        const auto count = get(stream, 4);
        if (!count || count > 65536) {
            throw std::runtime_error("Invalid package count");
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto name_size = get(stream, 2), origin_size = get(stream, 2);
            const auto size = get(stream, 8), packed = get(stream, 8);
            if (!size || size > max_entry_size || !packed || packed > size) {
                throw std::runtime_error("Invalid package entry size");
            }
            auto name = string_field(stream, static_cast<std::size_t>(name_size));
            if (!safe_name(name)) {
                throw std::runtime_error("Invalid package file name");
            }
            auto origin = string_field(stream, static_cast<std::size_t>(origin_size));
            auto digest = string_field(stream, 64);
            const auto offset = static_cast<std::uint64_t>(stream.tellg());
            if (offset > total || packed > total - offset) {
                throw std::runtime_error("Truncated package payload");
            }
            entries.push_back({{std::move(name), std::move(origin), size}, offset, packed, std::move(digest)});
            stream.seekg(static_cast<std::streamoff>(packed), std::ios::cur);
        }
        if (static_cast<std::uint64_t>(stream.tellg()) != total) {
            throw std::runtime_error("Unexpected trailing package data");
        }
    }
    Bytes read(std::size_t index)
    {
        if (index >= entries.size()) {
            throw std::runtime_error("Package image index out of range");
        }
        const auto& e = entries[index];
        stream.seekg(static_cast<std::streamoff>(e.offset));
        Bytes data(static_cast<std::size_t>(e.compressed));
        if (!stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
            throw std::runtime_error("Could not read package image");
        }
        if (e.compressed < e.info.size) {
            Codec codec;
            Bytes raw(static_cast<std::size_t>(e.info.size));
            SIZE_T produced{};
            if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &codec.decompressor) ||
                !Decompress(codec.decompressor, data.data(), data.size(), raw.data(), raw.size(), &produced) ||
                produced != raw.size()) {
                throw std::runtime_error("Invalid compressed package image");
            }
            data = std::move(raw);
        }
        if (std::get<std::string>(detail::sha256(std::as_bytes(std::span(data)))) != e.digest) {
            throw std::runtime_error("Image package checksum mismatch");
        }
        return data;
    }
};
} // namespace
ArchiveCheck write_package(const fs::path& target, std::span<const PackageImage> images)
{
    if (images.empty() || images.size() > 65536) {
        return issue("Invalid package image count.");
    }
    auto temp = target.parent_path() / (".visionpro-pack-" + uuid());
    struct Cleanup {
        fs::path file;
        ~Cleanup()
        {
            std::error_code ec;
            fs::remove(file, ec);
        }
    } cleanup{temp};
    try {
        Codec codec;
        if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &codec.compressor)) {
            return issue("Could not initialize image compression.");
        }
        std::ofstream out(temp, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out.write(magic.data(), magic.size());
        put(out, images.size(), 4);
        for (const auto& image : images) {
            if (!safe_name(image.name) || image.origin.empty() || image.origin.size() > 1024 ||
                image.bytes.empty() || image.bytes.size() > max_entry_size) {
                return issue("Invalid archived image name, origin or size.");
            }
            Bytes compressed(image.bytes.size());
            SIZE_T length{};
            const bool smaller = Compress(codec.compressor, image.bytes.data(), image.bytes.size(),
                                          compressed.data(), compressed.size(), &length) &&
                                 length < image.bytes.size();
            const auto payload = smaller ? std::span(compressed).first(length) : std::span(image.bytes);
            const auto digest = detail::sha256(std::as_bytes(std::span(image.bytes)));
            if (auto* d = std::get_if<core::Diagnostic>(&digest)) {
                return *d;
            }
            put(out, image.name.size(), 2);
            put(out, image.origin.size(), 2);
            put(out, image.bytes.size(), 8);
            put(out, payload.size(), 8);
            out << image.name << image.origin << std::get<std::string>(digest);
            out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        }
        out.close();
        return detail::publish_file(temp, target, false);
    } catch (const std::exception&) {
        return issue("Could not write the image package.");
    }
}
} // namespace visionpro::infrastructure::archive_detail

namespace visionpro::infrastructure
{
using namespace archive_detail;
core::Result<std::vector<ImagePackageEntry>> list_image_package(const fs::path& file)
{
    try {
        PackageReader reader(detail::windows_access_path(file));
        std::vector<ImagePackageEntry> items;
        for (auto& entry : reader.entries) {
            items.push_back(std::move(entry.info));
        }
        return items;
    } catch (const std::exception& e) {
        return issue(e.what());
    }
}
core::Result<Bytes> read_image_package_entry(const fs::path& file, std::size_t index)
{
    try {
        return PackageReader(detail::windows_access_path(file)).read(index);
    } catch (const std::exception& e) {
        return issue(e.what());
    }
}
core::Result<std::vector<fs::path>> extract_image_packages(const fs::path& source, bool same_name_folder)
{
    try {
        if (!source.is_absolute()) {
            return issue("Select an absolute package or directory path.");
        }
        std::vector<fs::path> packages, files;
        const auto selected = detail::windows_access_path(source, true);
        if (fs::is_directory(selected)) {
            for (const auto& entry : fs::directory_iterator(selected)) {
                if (entry.is_regular_file() && entry.path().extension() == ".dat") {
                    packages.push_back(entry.path());
                }
            }
            std::ranges::sort(packages);
        } else {
            packages.push_back(selected);
        }
        if (packages.empty()) {
            return issue("No .dat image packages found.");
        }
        for (const auto& file : packages) {
            PackageReader reader(file);
            const auto dir = same_name_folder ? file.parent_path() / file.stem() : file.parent_path();
            fs::create_directories(dir);
            if (same_name_folder && ((GetFileAttributesW(dir.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                !fs::equivalent(fs::canonical(dir).parent_path(), fs::canonical(file.parent_path())))) {
                return issue("Invalid extraction destination.");
            }
            for (std::size_t i = 0; i < reader.entries.size(); ++i) {
                auto data = reader.read(i);
                auto target = unique_path(dir / path_from(reader.entries[i].info.name));
                auto written = atomic_file(target, std::as_bytes(std::span(data)));
                if (auto* d = std::get_if<core::Diagnostic>(&written)) {
                    return *d;
                }
                files.push_back(detail::windows_display_path(target));
            }
        }
        return files;
    } catch (const std::exception& e) {
        return issue(e.what());
    }
}
} // namespace visionpro::infrastructure
