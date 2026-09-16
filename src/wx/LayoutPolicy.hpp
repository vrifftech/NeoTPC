#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace neotpc::layout {

// Positions follow the supplied text/metrics indexing, not encoded byte sizes.
// The wx adapter measures cumulative pixel extents using the current label font.
struct LineBreak {
    std::size_t end = 0;
    std::size_t next = 0;
};

inline LineBreak chooseLineBreak(const std::vector<int>& extents,
                                const std::vector<std::size_t>& spaces,
                                int width) {
    if (extents.empty()) return {};
    const auto fit = static_cast<std::size_t>(std::upper_bound(
        extents.begin(), extents.end(), std::max(1, width)) - extents.begin());
    if (fit == extents.size()) return {fit, fit};

    // Prefer a word boundary. For a path, identifier or other unbroken token,
    // break between characters instead of letting it widen the inspector.
    const auto boundary = std::upper_bound(spaces.begin(), spaces.end(), fit);
    if (boundary != spaces.begin() && *(boundary - 1) > 0) {
        const auto end = *(boundary - 1);
        auto next = end + 1;
        for (auto it = boundary; it != spaces.end() && *it == next; ++it) ++next;
        return {end, next};
    }
    // Always make progress, even if a single glyph is wider than the viewport.
    const auto end = std::max<std::size_t>(1, fit);
    return {end, end};
}

// Keep paragraph wrapping and line breaking in one implementation.
// Text can be wxString or std::basic_string; measure returns cumulative pixel
// extents in the same character indexing scheme as Text.
template<class Text, class Measure>
Text wrapText(const Text& original, int width, Measure measure) {
    if (width <= 0) return original;
    Text rendered;
    std::size_t begin = 0;
    for (;;) {
        const auto newline = original.find('\n', begin);
        Text rest = original.substr(begin, newline == Text::npos ? Text::npos : newline - begin);
        while (!rest.empty()) {
            auto extents = measure(rest);
            if (extents.size() != rest.length())
                throw std::invalid_argument("Text metrics must match character count");
            std::vector<std::size_t> spaces;
            int previous = 0;
            for (std::size_t n = 0; n < rest.length(); ++n) {
                previous = std::max(previous, extents[n]);
                extents[n] = previous;
                if (rest[n] == ' ' || rest[n] == '\t') spaces.push_back(n);
            }
            const auto split = chooseLineBreak(extents, spaces, width);
            rendered += rest.substr(0, split.end);
            rest = rest.substr(split.next);
            if (!rest.empty()) rendered += '\n';
        }
        if (newline == Text::npos) break;
        rendered += '\n';
        begin = newline + 1;
    }
    return rendered;
}

} // namespace neotpc::layout
