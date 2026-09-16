#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

namespace neotpc::layout {

struct GridColumnPolicy {
    int minimum = 1;  // Measured pixels, including cell padding.
    int weight = 1;
    bool optional = false;
};

// Fit the available width exactly. At narrow widths hide secondary columns;
// the selected-row details keep that information available. Even at large font
// scales, visible columns shrink and wrap instead of increasing virtual width.
inline std::vector<int> gridColumnWidths(const std::vector<GridColumnPolicy>& columns,
                                         int available) {
    std::vector<int> widths(columns.size(), 0);
    if (columns.empty() || available <= 0) return widths;
    const auto minimum = std::accumulate(columns.begin(), columns.end(), 0LL,
        [](long long total, const auto& col) { return total + std::max(1, col.minimum); });
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < columns.size(); ++i)
        if (minimum <= available || !columns[i].optional) visible.push_back(i);
    if (visible.empty()) visible.push_back(0);
    // Degenerate subpixel windows cannot display every column. Never overrun.
    if (visible.size() > static_cast<std::size_t>(available)) visible.resize(available);
    long long bases = 0, weights = 0;
    for (auto i : visible) {
        bases += std::max(1, columns[i].minimum);
        weights += std::max(1, columns[i].weight);
    }
    int left = available;
    for (std::size_t n = 0; n < visible.size(); ++n) {
        const auto i = visible[n];
        const int remainingColumns = static_cast<int>(visible.size() - n - 1);
        long long width = bases <= available
            ? std::max(1, columns[i].minimum) +
              (static_cast<long long>(available) - bases) * std::max(1, columns[i].weight) / weights
            : static_cast<long long>(available) * std::max(1, columns[i].weight) / weights;
        widths[i] = n + 1 == visible.size() ? left
            : std::clamp(static_cast<int>(width), 1, left - remainingColumns);
        left -= widths[i];
    }
    return widths;
}

} // namespace neotpc::layout
