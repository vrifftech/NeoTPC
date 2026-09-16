#pragma once

#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"
#include "texture/Image.hpp"
#include "texture/Txi.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace neotpc::workflow {

struct CommonTxiField { const char* key; const char* label; };
inline constexpr std::array<CommonTxiField, 6> commonTxiFields{{
    {"proceduretype", "Procedure type"}, {"numx", "Columns (numx)"},
    {"numy", "Rows (numy)"}, {"fps", "Frames per second"},
    {"envmaptexture", "Environment map"}, {"bumpmaptexture", "Bump map"}
}};

inline std::string trimTxiField(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string commonTxiValueIssue(std::string_view key, const std::string& value) {
    if (value.empty()) return {}; // Removing a field is explicit and valid.
    if (value.find_first_of("\r\n") != std::string::npos)
        return "Enter one value on one line, or leave blank to remove.";
    if (key == "numx" || key == "numy") {
        unsigned int number = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number == 0 || number > 32767)
            return "Enter a decimal integer from 1 to 32767, or leave blank to remove.";
    } else if (key == "fps") {
        // TXI uses decimal points, regardless of the UI/system locale. Do not
        // accept locale commas then write text the texture parser can't read.
        std::istringstream in(value); in.imbue(std::locale::classic());
        double number = 0;
        if (!(in >> number) || !std::isfinite(number) || number <= 0)
            return "Enter a positive, finite FPS with a decimal point (for example 8 or 7.5), or leave blank.";
        in >> std::ws;
        if (!in.eof()) return "Enter one FPS number using a decimal point, not a comma.";
    } else if (key == "proceduretype") {
        const auto directive = texture::findTxiDirective(std::string(key));
        if (directive && std::find(directive->allowedValues.begin(), directive->allowedValues.end(), value)
                == directive->allowedValues.end())
            return "Choose an exact lowercase procedure type from the dictionary, or leave blank to remove.";
    }
    for (const auto& issue : texture::validateTxiText(std::string(key) + " " + value + "\n"))
        if (issue.severity == texture::TxiIssueSeverity::Error) return issue.message;
    return {};
}

struct CommonTxiEditResult {
    std::string text;
    int invalidField = -1;
    std::string error;
};

// Validate every changed field before constructing a result. Unchanged unknown
// values, comments, lists and duplicates survive; the caller commits one undo
// group only after an accepted dialog. No partial edits on failure/cancellation.
inline CommonTxiEditResult editCommonTxiValues(const std::string& original,
                                               const std::array<std::string, 6>& values) {
    std::array<std::string, 6> normalized;
    std::array<bool, 6> changed{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        normalized[i] = trimTxiField(values[i]);
        changed[i] = normalized[i] != texture::getTxiValue(original, commonTxiFields[i].key).value_or("");
        if (!changed[i]) continue;
        const auto error = commonTxiValueIssue(commonTxiFields[i].key, normalized[i]);
        if (!error.empty()) return {original, static_cast<int>(i), error};
    }
    std::string candidate = original;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (!changed[i]) continue;
        candidate = normalized[i].empty()
            ? texture::removeTxiValue(candidate, commonTxiFields[i].key)
            : texture::setTxiValue(candidate, commonTxiFields[i].key, normalized[i]);
    }
    return {std::move(candidate), -1, {}};
}

inline std::filesystem::path txiExportDestination(std::filesystem::path output) {
    if (output.empty() || output.filename().empty() || output.filename() == "." || output.filename() == "..")
        throw texture::TextureError("Choose a TXI filename.");
    if (output.extension().empty()) output.replace_extension(".txi");
    if (texture::extensionLower(output) != "txi")
        throw texture::TextureError("Export TXI requires a .txi filename. Use Export / Convert to write an image.");
    return output;
}

} // namespace neotpc::workflow
