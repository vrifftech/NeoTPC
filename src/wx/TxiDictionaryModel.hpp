#pragma once

#include "texture/Txi.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace neotpc::txidictionary {

// Operates on the existing shared catalog. Reference text is never an edit or
// an insertion template: defaults such as "empty" and "none" are explanatory.
inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline std::string searchText(const texture::TxiDirectiveInfo& item) {
    std::string text = item.name + "\n" + item.category + "\n" +
        texture::txiDirectiveValueKindToString(item.valueKind) + "\n" +
        item.valueHint + "\n" + item.defaultValue + "\n" + item.description + "\n" + item.notes;
    for (const auto& value : item.allowedValues) text += "\n" + value;
    if (item.name == "decal") text += "\ndecal1"; // Catalog's documented legacy alias.
    return lower(std::move(text));
}

inline std::vector<std::string> categories() {
    std::set<std::string> unique;
    for (const auto& item : texture::txiDirectiveCatalog()) unique.insert(item.category);
    return {unique.begin(), unique.end()};
}

inline std::string sortValue(const texture::TxiDirectiveInfo& item, int column) {
    switch (column) {
    case 1: return texture::txiDirectiveValueKindToString(item.valueKind);
    case 2: return item.defaultValue;
    case 3: return item.description;
    default: return item.name;
    }
}

inline std::vector<const texture::TxiDirectiveInfo*> find(
    const std::string& query = {}, const std::string& category = {},
    int sortColumn = 0, bool ascending = true) {
    std::istringstream words(lower(query));
    std::vector<std::string> terms;
    for (std::string word; words >> word;) terms.push_back(std::move(word));
    std::vector<const texture::TxiDirectiveInfo*> result;
    for (const auto& item : texture::txiDirectiveCatalog()) {
        if (!category.empty() && item.category != category) continue;
        const auto text = searchText(item);
        if (std::all_of(terms.begin(), terms.end(), [&](const auto& word) {
                return text.find(word) != std::string::npos;
            })) result.push_back(&item);
    }
    std::sort(result.begin(), result.end(), [&](const auto* a, const auto* b) {
        const auto left = lower(sortValue(*a, sortColumn));
        const auto right = lower(sortValue(*b, sortColumn));
        // A name tie-break makes filtered and reversed views deterministic.
        const bool less = left == right ? a->name < b->name : left < right;
        const bool greater = left == right ? a->name > b->name : left > right;
        return ascending ? less : greater;
    });
    return result;
}

inline std::string details(const texture::TxiDirectiveInfo& item) {
    std::string out = item.name + "\n\nCategory: " + item.category +
        "\nType: " + texture::txiDirectiveValueKindToString(item.valueKind) +
        "\nValue / range: " + item.valueHint +
        "\nDocumented default: " + (item.defaultValue.empty() ? "Not specified" : item.defaultValue);
    if (!item.allowedValues.empty()) {
        out += "\nAllowed values: ";
        for (std::size_t i = 0; i < item.allowedValues.size(); ++i) {
            if (i) out += ", ";
            out += item.allowedValues[i];
        }
    }
    out += "\n\n" + item.description;
    if (!item.notes.empty()) out += "\n\nNotes\n" + item.notes;
    out += "\n\nDefaults describe catalog behavior; they are not automatically inserted into your TXI.";
    return out;
}

} // namespace neotpc::txidictionary
