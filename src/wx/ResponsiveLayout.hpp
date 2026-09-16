#pragma once

#include "LayoutPolicy.hpp"

#include <wx/checkbox.h>
#include <wx/dcclient.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <vector>

namespace neotpc::layout {

// wxStaticText::Wrap() edits its label and doesn't split long words. Keep the
// source separately so narrowing AND widening are reversible, including paths.
class WrappedLabel final : public wxStaticText {
public:
    WrappedLabel(wxWindow* parent, wxWindowID id, const wxString& text)
        : wxStaticText(parent, id, wxEmptyString, wxDefaultPosition,
                       wxDefaultSize, wxST_NO_AUTORESIZE) {
        SetMinSize(wxSize(1, -1));
        SetLabel(text);
    }

    void SetLabel(const wxString& text) override {
        if (!dirty_ && original_ == text) return;
        original_ = text;
        dirty_ = true;
        rewrap(width_);
        // Content changes may alter height without changing the panel width.
        // This is intentionally not called by rewrap()/sizer measurement.
        PostSizeEventToParent();
    }

    const wxString& unwrappedText() const { return original_; }

    bool InformFirstDirection(int direction, int size, int) override {
        return direction == wxHORIZONTAL && size > 0 && rewrap(size);
    }

private:
    bool rewrap(int width) {
        if (!dirty_ && width_ == width && font_ == GetFont()) return false;
        width_ = width;
        font_ = GetFont();
        dirty_ = false;
        wxString rendered;
        if (width <= 0) {
            rendered = original_;
        } else {
            wxClientDC dc(this);
            dc.SetFont(GetFont());
            rendered = wrapText(original_, width, [&](const wxString& line) {
                wxArrayInt measured;
                if (!dc.GetPartialTextExtents(line, measured) || measured.size() != line.length()) {
                    measured.clear();
                    for (std::size_t n = 1; n <= line.length(); ++n)
                        measured.Add(dc.GetTextExtent(line.substr(0, n)).x);
                }
                std::vector<int> extents;
                extents.reserve(line.length());
                for (std::size_t n = 0; n < line.length(); ++n) extents.push_back(measured[n]);
                return extents;
            });
        }
        // These are prose/path labels, not mnemonic-bearing field captions.
        rendered.Replace("&", "&&");
        wxStaticText::SetLabel(rendered);
        // NO_AUTORESIZE prevents geometry changes during a sizer calculation;
        // the new height must nevertheless participate in the next calculation.
        InvalidateBestSize();
        return true;
    }

    wxString original_;
    wxFont font_;
    int width_ = -1;
    bool dirty_ = true;
};

// Account for group-box insets during width-first measurement as well as actual
// layout. Otherwise a wrapping row can be measured wider than it is drawn.
class GroupSizer final : public wxStaticBoxSizer {
public:
    GroupSizer(wxWindow* parent, const wxString& title)
        : wxStaticBoxSizer(wxVERTICAL, parent, title) {}

    bool InformFirstDirection(int direction, int size, int availableOtherDir) override {
        if (direction == wxHORIZONTAL && size > 0) {
            int top = 0, other = 0;
            GetStaticBox()->GetBordersForSizer(&top, &other);
            size = std::max(1, size - 2 * other);
        }
        return wxBoxSizer::InformFirstDirection(direction, size, availableOtherDir);
    }
};

// A field is one horizontal row when it fits. The input moves beneath its label
// when it doesn't; both stay in the same sizer and preserve their tab order.
inline wxWrapSizer* fieldRow(wxWindow* parent, wxStaticText* caption, wxWindow* control) {
    auto* row = new wxWrapSizer(wxHORIZONTAL);
    const int gap = parent->FromDIP(6);
    row->Add(caption, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, gap);
    row->Add(control, 1, wxEXPAND | wxBOTTOM, gap);
    return row;
}

inline wxWrapSizer* fieldRow(wxWindow* parent, const wxString& label, wxWindow* control) {
    return fieldRow(parent, new wxStaticText(parent, wxID_ANY, label), control);
}

// Native checkbox for keyboard/accessibility, separately wrapping caption for
// portable multiline layout (native multiline checkbox support varies by port).
class WrappedCheckBox final : public wxPanel {
public:
    WrappedCheckBox(wxWindow* parent, const wxString& caption)
        : wxPanel(parent), gap_(FromDIP(6)) {
        SetMinSize(wxSize(1, -1));
        box_ = new wxCheckBox(this, wxID_ANY, wxEmptyString);
        box_->SetName(caption);
        label_ = new WrappedLabel(this, wxID_ANY, caption);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(box_, 0, wxRIGHT, gap_);
        row->Add(label_, 1, wxEXPAND);
        SetSizer(row);
        label_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!IsEnabled()) return;
            box_->SetFocus();
            box_->SetValue(!box_->GetValue());
            wxCommandEvent event(wxEVT_CHECKBOX, box_->GetId());
            event.SetEventObject(box_);
            event.SetInt(box_->GetValue());
            box_->GetEventHandler()->ProcessEvent(event);
        });
        // Native checkbox events bubble through this panel unchanged.
    }

    void SetLabel(const wxString& caption) override {
        if (box_) box_->SetName(caption);
        if (label_) label_->SetLabel(caption);
        InvalidateBestSize();
    }

    bool GetValue() const { return box_->GetValue(); }
    void SetValue(bool value) { box_->SetValue(value); }

    bool InformFirstDirection(int direction, int size, int availableOtherDir) override {
        if (direction != wxHORIZONTAL || size <= 0) return false;
        const int textWidth = std::max(1, size - box_->GetEffectiveMinSize().x - gap_);
        const bool changed = label_->InformFirstDirection(direction, textWidth, availableOtherDir);
        if (changed) InvalidateBestSize();
        return changed;
    }

private:
    wxCheckBox* box_ = nullptr;
    WrappedLabel* label_ = nullptr;
    int gap_ = 0;
};

// Inspector pages scroll DOWN as wrapping increases their height. In particular,
// FitInside() must never retain an old, wider virtual width after sash dragging.
class ScrolledPage final : public wxScrolledWindow {
public:
    explicit ScrolledPage(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxVSCROLL | wxTAB_TRAVERSAL | wxBORDER_NONE) {
        SetMinSize(wxSize(1, 1));
        SetScrollRate(0, FromDIP(12));
    }

    wxSize GetBestVirtualSize() const override {
        const auto client = GetClientSize();
        const auto minimum = GetSizer() ? GetSizer()->GetMinSize() : client;
        return wxSize(std::max(1, client.x), std::max(client.y, minimum.y));
    }

    bool Layout() override {
        if (reflowing_ || !GetSizer() || GetClientSize().x <= 0)
            return wxScrolledWindow::Layout();
        reflowing_ = true;
        // A vertical scrollbar changes client width; expanded nested panels can
        // change best height after receiving that width. Resolve both without
        // recursively queuing size events, resizing the frame, or resetting the
        // user's scroll position.
        for (int pass = 0; pass < 4; ++pass) {
            wxScrolledWindow::Layout();
            layoutDescendants(this);
            const wxSize wanted = GetBestVirtualSize();
            const auto client = GetClientSize();
            if (GetVirtualSize() == wanted) break;
            SetVirtualSize(wanted);
            if (GetClientSize() != client) continue;
        }
        wxScrolledWindow::Layout();
        reflowing_ = false;
        return true;
    }

private:
    static void layoutDescendants(wxWindow* parent) {
        for (auto* child : parent->GetChildren()) {
            if (!child->IsShown()) continue;
            if (child->GetSizer()) child->Layout();
            layoutDescendants(child);
            // Includes wxCollapsiblePane, whose native best-size cache otherwise
            // may still describe its previously wider content pane.
            child->InvalidateBestSize();
        }
    }
    bool reflowing_ = false;
};

} // namespace neotpc::layout
