#pragma once

#include "GridLayoutPolicy.hpp"
#include "LayoutPolicy.hpp"

#include <wx/dcclient.h>
#include <wx/grid.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neotpc::layout {

struct ReferenceColumn {
    wxString title;
    int minimumCharacters = 10;
    int weight = 1;
    bool optional = false;
};

// A genuinely tabular, read-only view. Keep canonical cells separate from their
// displayed soft line breaks, so copy/search and narrow -> wide resize are lossless.
class WrappedReadOnlyGrid final : public wxGrid {
public:
    using Rows = std::vector<std::vector<wxString>>;
    WrappedReadOnlyGrid(wxWindow* parent, std::vector<ReferenceColumn> columns)
        : wxGrid(parent, wxID_ANY), columns_(std::move(columns)) {
        CreateGrid(0, static_cast<int>(columns_.size()));
        SetMinSize(FromDIP(wxSize(1, 140)));
        EnableEditing(false);
        DisableDragColSize();
        DisableDragRowSize();
        DisableDragGridSize();
        SetRowLabelSize(0);
        SetColMinimalAcceptableWidth(1);
        SetDefaultCellAlignment(wxALIGN_LEFT, wxALIGN_TOP);
        SetDefaultCellOverflow(false);
        SetColLabelAlignment(wxALIGN_LEFT, wxALIGN_CENTER);
        SetSelectionMode(wxGrid::wxGridSelectRows);
        SetMargins(0, 0);
        for (int col = 0; col < GetNumberCols(); ++col)
            SetColLabelValue(col, columns_[static_cast<std::size_t>(col)].title);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { event.Skip(); queueReflow(); });
        GetGridWindow()->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { event.Skip(); queueReflow(); });
    }

    bool setRows(Rows rows) {
        if (rows == original_) return false;
        for (const auto& row : rows)
            if (row.size() != columns_.size()) throw std::invalid_argument("Wrong reference-grid column count");
        wxGridUpdateLocker lock(this);
        ClearSelection();
        const int count = static_cast<int>(rows.size());
        if (GetNumberRows() > count) DeleteRows(count, GetNumberRows() - count);
        else if (GetNumberRows() < count) AppendRows(count - GetNumberRows());
        original_ = std::move(rows);
        dirty_ = true;
        reflow();
        return true;
    }

    const Rows& originalRows() const { return original_; }

    bool Layout() override {
        const bool result = wxGrid::Layout();
        reflow();
        return result;
    }

    void reflow() {
        if (reflowing_ || !GetGridWindow()) return;
        const int available = std::max(1, GetGridWindow()->GetClientSize().x - 1);
        const auto font = GetDefaultCellFont();
        bool heightsUnchanged = rowHeights_.size() == original_.size();
        for (int row = 0; heightsUnchanged && row < GetNumberRows(); ++row)
            heightsUnchanged = GetRowSize(row) == rowHeights_[row];
        if (!dirty_ && width_ == available && font_ == font && heightsUnchanged &&
                GetColLabelSize() == labelHeight_) return;
        reflowing_ = true;
        wxGridUpdateLocker lock(this);
        width_ = available;
        font_ = font;
        dirty_ = false;
        wxClientDC dc(GetGridWindow());
        dc.SetFont(font);
        const int padding = FromDIP(4);
        const int em = std::max(1, dc.GetCharWidth());
        std::vector<GridColumnPolicy> policies;
        for (const auto& col : columns_)
            policies.push_back({em * col.minimumCharacters + 2 * padding, col.weight, col.optional});
        const auto widths = gridColumnWidths(policies, available);
        for (int col = 0; col < GetNumberCols(); ++col) {
            if (GetColSize(col) != widths[col]) SetColSize(col, widths[col]);
        }
        // Native string cells draw explicit line breaks. Using the existing line
        // policy also breaks long directive names, unlike word-only renderers.
        rowHeights_.resize(original_.size());
        for (int row = 0; row < GetNumberRows(); ++row) {
            int height = FromDIP(26);
            for (int col = 0; col < GetNumberCols(); ++col) {
                const auto& source = original_[row][col];
                const auto text = widths[col] == 0 ? source : wrapText(source,
                    std::max(1, widths[col] - 2 * padding), [&](const wxString& line) {
                        wxArrayInt measured;
                        std::vector<int> extents;
                        extents.reserve(line.length());
                        if (dc.GetPartialTextExtents(line, measured) && measured.size() == line.length()) {
                            for (std::size_t n = 0; n < line.length(); ++n) extents.push_back(measured[n]);
                        } else {
                            for (std::size_t n = 1; n <= line.length(); ++n)
                                extents.push_back(dc.GetTextExtent(line.substr(0, n)).x);
                        }
                        return extents;
                    });
                if (GetCellValue(row, col) != text) SetCellValue(row, col, text);
                if (widths[col] != 0) {
                    wxCoord textWidth = 0, textHeight = 0;
                    dc.GetMultiLineTextExtent(text, &textWidth, &textHeight);
                    height = std::max(height, static_cast<int>(textHeight) + 2 * padding);
                }
            }
            rowHeights_[row] = height;
            if (GetRowSize(row) != height) SetRowSize(row, height);
        }
        labelHeight_ = std::max(FromDIP(28), GetLabelFont().GetPixelSize().y + FromDIP(12));
        SetColLabelSize(labelHeight_);
        // A keyboard cursor may have been in an optional column before resize.
        const int cursorCol = GetGridCursorCol();
        if (GetGridCursorRow() >= 0 && cursorCol >= 0 && widths[cursorCol] == 0)
            SetGridCursor(GetGridCursorRow(), 0);
        reflowing_ = false;
        ForceRefresh();
    }

private:
    void queueReflow() {
        if (queued_) return;
        queued_ = true;
        CallAfter([this] { queued_ = false; reflow(); });
    }
    std::vector<ReferenceColumn> columns_;
    Rows original_;
    wxFont font_;
    std::vector<int> rowHeights_;
    int labelHeight_ = -1;
    int width_ = -1;
    bool dirty_ = true;
    bool reflowing_ = false;
    bool queued_ = false;
};

} // namespace neotpc::layout
