// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture/FileUtil.hpp"
#include "texture/ParserLimits.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace neotpc::texture {
namespace {

std::string systemErrorText(const std::string& action, const std::filesystem::path& path) {
#if defined(_WIN32)
    return action + ": " + path.string() + " (Windows error " + std::to_string(GetLastError()) + ")";
#else
    return action + ": " + path.string() + ": " + std::strerror(errno);
#endif
}

void trimAsciiSpaces(std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    value = first < last ? std::string(first, last) : std::string{};
}

bool removeTrailingDuplicateNumber(std::string& value) {
    trimAsciiSpaces(value);
    if (value.size() < 3 || value.back() != ')') return false;
    const auto open = value.find_last_of('(');
    if (open == std::string::npos || open + 2 >= value.size()) return false;
    if (!std::all_of(value.begin() + static_cast<std::ptrdiff_t>(open + 1), value.end() - 1,
                     [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return false;
    }
    value.erase(open);
    trimAsciiSpaces(value);
    return true;
}

bool removeTrailingCopyMarker(std::string& value) {
    trimAsciiSpaces(value);
    static const std::array<const char*, 5> suffixes{{" - copy", "- copy", "-copy", "_copy", " copy"}};
    for (const char* suffix : suffixes) {
        const std::string marker = suffix;
        if (value.size() >= marker.size() &&
            value.compare(value.size() - marker.size(), marker.size(), marker) == 0) {
            value.erase(value.size() - marker.size());
            trimAsciiSpaces(value);
            return true;
        }
    }
    if (value.size() >= 6 && value.compare(value.size() - 6, 6, "(copy)") == 0) {
        value.erase(value.size() - 6);
        trimAsciiSpaces(value);
        return true;
    }
    return false;
}

std::string normalizedPathKey(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) absolute = path;
    return absolute.lexically_normal().generic_string();
}

bool pathsReferToSameFile(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code ec;
    const bool equivalent = std::filesystem::equivalent(left, right, ec);
    if (!ec) return equivalent;
#if defined(_WIN32)
    return asciiLower(normalizedPathKey(left)) == asciiLower(normalizedPathKey(right));
#else
    return normalizedPathKey(left) == normalizedPathKey(right);
#endif
}

} // namespace

static void flushFileToDisk(const std::filesystem::path& path);

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string extensionLower(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    return asciiLower(ext);
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path) {
    return readFileBytes(path, parser::maxInMemoryFileBytes());
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path, std::uintmax_t maxBytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("Unable to inspect file: " + path.string() + ": " + ec.message());
    if (size > maxBytes) {
        throw std::runtime_error("Refusing to read file larger than limit (" + std::to_string(size) + " > " +
                                 std::to_string(maxBytes) + "): " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("File is too large for this process: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("File is too large for one bounded stream read: " + path.string());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Unable to open file: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in && !in.eof()) throw std::runtime_error("Unable to read file: " + path.string());
    if (static_cast<std::size_t>(in.gcount()) != bytes.size()) {
        throw std::runtime_error("File changed while it was being read: " + path.string());
    }
    char extra = 0;
    if (in.read(&extra, 1)) throw std::runtime_error("File grew beyond its inspected size while reading: " + path.string());
    return bytes;
}

void writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Unable to open file for writing: " + path.string());
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
        throw std::runtime_error("Unable to write file: " + path.string());
    }
    out.flush();
    if (!out) throw std::runtime_error("Unable to flush file: " + path.string());
    out.close();
    if (!out) throw std::runtime_error("Unable to close file: " + path.string());
    flushFileToDisk(path);
}

static void flushFileToDisk(const std::filesystem::path& path) {
#if defined(_WIN32)
    const DWORD originalAttributes = GetFileAttributesW(path.c_str());
    if (originalAttributes == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error(systemErrorText("Unable to inspect file for durable flush", path));
    }
    const bool restoreReadOnly = (originalAttributes & FILE_ATTRIBUTE_READONLY) != 0u;
    DWORD writableAttributes = originalAttributes & ~FILE_ATTRIBUTE_READONLY;
    if (writableAttributes == 0u) writableAttributes = FILE_ATTRIBUTE_NORMAL;
    if (restoreReadOnly && !SetFileAttributesW(path.c_str(), writableAttributes)) {
        throw std::runtime_error(systemErrorText("Unable to temporarily make file writable for durable flush", path));
    }
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD openError = GetLastError();
        if (restoreReadOnly) SetFileAttributesW(path.c_str(), originalAttributes);
        SetLastError(openError);
        throw std::runtime_error(systemErrorText("Unable to open file for durable flush", path));
    }
    if (!FlushFileBuffers(handle)) {
        const DWORD flushError = GetLastError();
        const auto message = systemErrorText("Unable to durably flush file", path);
        CloseHandle(handle);
        if (restoreReadOnly) SetFileAttributesW(path.c_str(), originalAttributes);
        SetLastError(flushError);
        throw std::runtime_error(message);
    }
    CloseHandle(handle);
    if (restoreReadOnly && !SetFileAttributesW(path.c_str(), originalAttributes)) {
        throw std::runtime_error(systemErrorText("Unable to restore read-only file attributes after durable flush", path));
    }
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) throw std::runtime_error(systemErrorText("Unable to open file for durable flush", path));
    if (::fsync(descriptor) != 0) {
        const auto message = systemErrorText("Unable to durably flush file", path);
        ::close(descriptor);
        throw std::runtime_error(message);
    }
    ::close(descriptor);
#endif
}

static bool extensionIn(const std::filesystem::path& path, const std::vector<std::string>& extensions) {
    const auto ext = extensionLower(path);
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

std::string canonicalTextureConflictStem(const std::filesystem::path& path) {
    const std::string original = asciiLower(path.stem().string());
    std::string canonical = original;
    trimAsciiSpaces(canonical);
    bool changed = true;
    while (changed && !canonical.empty()) {
        changed = false;
        if (removeTrailingDuplicateNumber(canonical)) changed = true;
        if (removeTrailingCopyMarker(canonical)) changed = true;
    }
    return canonical.empty() ? original : canonical;
}

static bool isComparableTextureImagePath(const std::filesystem::path& path) {
    static const std::vector<std::string> extensions = {
        "tpc", "txb", "tga", "dds", "png", "jpg", "jpeg", "jpe", "bmp"
    };
    return extensionIn(path, extensions);
}

std::vector<std::filesystem::path> findConflictingTexturePaths(const std::filesystem::path& currentPath) {
    if (currentPath.empty()) throw std::runtime_error("Cannot search for conflicting images without a current file");
    const auto targetStem = canonicalTextureConflictStem(currentPath);
    if (targetStem.empty()) throw std::runtime_error("Current filename does not have a comparable base name");
    auto directory = currentPath.parent_path();
    if (directory.empty()) directory = std::filesystem::current_path();

    std::vector<std::filesystem::path> matches;
    std::error_code currentError;
    if (std::filesystem::is_regular_file(currentPath, currentError) && !currentError) matches.push_back(currentPath);

    std::error_code iterationError;
    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, iterationError);
    const std::filesystem::directory_iterator end;
    while (!iterationError && it != end) {
        std::error_code typeError;
        const auto candidate = it->path();
        if (it->is_regular_file(typeError) && !typeError && isComparableTextureImagePath(candidate) &&
            canonicalTextureConflictStem(candidate) == targetStem && !pathsReferToSameFile(candidate, currentPath)) {
            matches.push_back(candidate);
        }
        it.increment(iterationError);
    }
    if (iterationError) {
        throw std::runtime_error("Unable to scan conflicting images in " + directory.string() + ": " + iterationError.message());
    }

    std::sort(matches.begin(), matches.end(), [&](const auto& left, const auto& right) {
        const bool leftCurrent = pathsReferToSameFile(left, currentPath);
        const bool rightCurrent = pathsReferToSameFile(right, currentPath);
        if (leftCurrent != rightCurrent) return leftCurrent;
        const auto leftName = asciiLower(left.filename().string());
        const auto rightName = asciiLower(right.filename().string());
        if (leftName != rightName) return leftName < rightName;
        return left.filename().string() < right.filename().string();
    });
    return matches;
}

} // namespace neotpc::texture
