#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neotpc::texture {

std::string asciiLower(std::string value);
// Converts native Windows UTF-16 paths to UTF-8 instead of routing them
// through the active ANSI code page. POSIX native path bytes are preserved.
std::string pathToUtf8(const std::filesystem::path& path);
std::string genericPathToUtf8(const std::filesystem::path& path);
std::string extensionLower(const std::filesystem::path& path);
// Whole-file reads are capped by the current parser memory budget.
std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path);
std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path, std::uintmax_t maxBytes);
void writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);
// Normalizes common filesystem conflict suffixes such as "(2)" and
// "- copy" so same-folder texture variants can be grouped for comparison.
std::string canonicalTextureConflictStem(const std::filesystem::path& path);
std::vector<std::filesystem::path> findConflictingTexturePaths(const std::filesystem::path& currentPath);

} // namespace neotpc::texture
