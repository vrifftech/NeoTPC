#include "texture/FileUtil.hpp"
#include "texture/ParserLimits.hpp"

#include <algorithm>
#include <array>
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

#if defined(_WIN32)
std::string widePathToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("Windows path is too long to encode as UTF-8");
    }

    const int inputLength = static_cast<int>(value.size());
    const int outputLength = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), inputLength, nullptr, 0, nullptr, nullptr);
    if (outputLength <= 0) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to encode Windows path as UTF-8");
    }

    std::string output(static_cast<std::size_t>(outputLength), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), inputLength, output.data(), outputLength, nullptr, nullptr);
    if (converted != outputLength) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to encode Windows path as UTF-8");
    }
    return output;
}
#endif

std::string systemErrorText(const std::string& action, const std::filesystem::path& path) {
#if defined(_WIN32)
    const DWORD error = GetLastError();
    return action + ": " + pathToUtf8(path) + " (Windows error " + std::to_string(error) + ")";
#else
    return action + ": " + pathToUtf8(path) + ": " + std::strerror(errno);
#endif
}

bool isAsciiSpace(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

void trimAsciiSpaces(std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), isAsciiSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isAsciiSpace).base();
    value = first < last ? std::string(first, last) : std::string{};
}

bool removeTrailingDuplicateNumber(std::string& value) {
    trimAsciiSpaces(value);
    if (value.size() < 3 || value.back() != ')') return false;
    const auto open = value.find_last_of('(');
    if (open == std::string::npos || open + 2 >= value.size()) return false;
    if (!std::all_of(value.begin() + static_cast<std::ptrdiff_t>(open + 1), value.end() - 1,
                     [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
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
    return genericPathToUtf8(absolute.lexically_normal());
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

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    return widePathToUtf8(path.native());
#else
    return path.native();
#endif
}

std::string genericPathToUtf8(const std::filesystem::path& path) {
    auto value = pathToUtf8(path);
#if defined(_WIN32)
    std::replace(value.begin(), value.end(), '\\', '/');
#endif
    return value;
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch + ('a' - 'A'));
        return static_cast<char>(ch);
    });
    return value;
}

std::string extensionLower(const std::filesystem::path& path) {
    auto ext = pathToUtf8(path.extension());
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
    if (ec) throw std::runtime_error("Unable to inspect file: " + pathToUtf8(path) + ": " + ec.message());
    if (size > maxBytes) {
        throw std::runtime_error("Refusing to read file larger than limit (" + std::to_string(size) + " > " +
                                 std::to_string(maxBytes) + "): " + pathToUtf8(path));
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("File is too large for this process: " + pathToUtf8(path));
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("File is too large for one bounded stream read: " + pathToUtf8(path));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Unable to open file: " + pathToUtf8(path));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in && !in.eof()) throw std::runtime_error("Unable to read file: " + pathToUtf8(path));
    if (static_cast<std::size_t>(in.gcount()) != bytes.size()) {
        throw std::runtime_error("File changed while it was being read: " + pathToUtf8(path));
    }
    char extra = 0;
    if (in.read(&extra, 1)) throw std::runtime_error("File grew beyond its inspected size while reading: " + pathToUtf8(path));
    return bytes;
}

void writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Unable to open file for writing: " + pathToUtf8(path));
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
        throw std::runtime_error("Unable to write file: " + pathToUtf8(path));
    }
    out.flush();
    if (!out) throw std::runtime_error("Unable to flush file: " + pathToUtf8(path));
    out.close();
    if (!out) throw std::runtime_error("Unable to close file: " + pathToUtf8(path));
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
    const std::string original = asciiLower(pathToUtf8(path.stem()));
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
        throw std::runtime_error("Unable to scan conflicting images in " + pathToUtf8(directory) + ": " + iterationError.message());
    }

    std::sort(matches.begin(), matches.end(), [&](const auto& left, const auto& right) {
        const bool leftCurrent = pathsReferToSameFile(left, currentPath);
        const bool rightCurrent = pathsReferToSameFile(right, currentPath);
        if (leftCurrent != rightCurrent) return leftCurrent;
        const auto leftName = asciiLower(pathToUtf8(left.filename()));
        const auto rightName = asciiLower(pathToUtf8(right.filename()));
        if (leftName != rightName) return leftName < rightName;
        return pathToUtf8(left.filename()) < pathToUtf8(right.filename());
    });
    return matches;
}

} // namespace neotpc::texture
