#include "TxiDiagnosticsPanel.hpp"

#include "NeoWxUi.hpp"
#include "WrappedReadOnlyGrid.hpp"

#include <wx/button.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>
#include <algorithm>
#include <utility>

namespace neotpc {
namespace {
bool sameIssue(const texture::TxiValidationIssue& a, const texture::TxiValidationIssue& b) {
    return a.lineNumber == b.lineNumber && a.severity == b.severity && a.key == b.key && a.message == b.message;
}
}

TxiDiagnosticsPanel::TxiDiagnosticsPanel(wxWindow* parent) : wxPanel(parent) {
    SetMinSize(wxSize(1, -1));
    auto* root = new wxBoxSizer(wxVERTICAL);
    grid_ = new layout::WrappedReadOnlyGrid(this, {
        {"Severity", 8, 22, false}, {"Line", 4, 12, false}, {"Issue", 20, 66, false}});
    grid_->SetMinSize(FromDIP(wxSize(1, 110)));
    grid_->SetName("TXI validation issues");
    grid_->SetToolTip("Select an issue for its complete message; double-click to go to its line.");
    wxui::configureStableGridRendering(*grid_);
    root->Add(grid_, 1, wxEXPAND | wxBOTTOM, FromDIP(6));
    details_ = new wxTextCtrl(this, wxID_ANY, "No validation issues.", wxDefaultPosition, wxDefaultSize,
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    details_->SetName("Selected TXI issue details");
    details_->SetMinSize(FromDIP(wxSize(1, 90)));
    root->Add(details_, 1, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* buttons = new wxWrapSizer(wxHORIZONTAL);
    jump_ = new wxButton(this, wxID_ANY, "Go to line");
    lookup_ = new wxButton(this, wxID_ANY, "Look up key");
    buttons->Add(jump_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
    buttons->Add(lookup_, 0, wxBOTTOM, FromDIP(6));
    root->Add(buttons, 0, wxEXPAND);
    SetSizer(root);
    grid_->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& event) {
        if (!rebuilding_) select(event.GetRow());
        event.Skip();
    });
    grid_->Bind(wxEVT_GRID_CELL_LEFT_DCLICK, [this](wxGridEvent& event) { select(event.GetRow()); jump(); });
    jump_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { jump(); });
    lookup_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (selected_ < 0 || !lookupHandler_) return;
        if (const auto directive = texture::findTxiDirective(issues_[selected_].key))
            lookupHandler_(directive->name);
    });
    select(-1);
}

bool TxiDiagnosticsPanel::Layout() {
    const bool result = wxPanel::Layout();
    if (grid_) grid_->reflow();
    return result;
}

void TxiDiagnosticsPanel::setIssues(std::vector<texture::TxiValidationIssue> issues) {
    if (issues.size() == issues_.size() && std::equal(issues.begin(), issues.end(), issues_.begin(), sameIssue)) return;
    int selected = issues.empty() ? -1 : 0;
    if (selected_ >= 0 && static_cast<std::size_t>(selected_) < issues_.size()) {
        for (std::size_t i = 0; i < issues.size(); ++i)
            if (sameIssue(issues[i], issues_[selected_])) { selected = static_cast<int>(i); break; }
    }
    rebuilding_ = true;
    issues_ = std::move(issues);
    layout::WrappedReadOnlyGrid::Rows rows;
    for (const auto& issue : issues_) {
        rows.push_back({wxui::toWx(texture::txiIssueSeverityToString(issue.severity)),
            issue.lineNumber ? wxString::Format("%llu", static_cast<unsigned long long>(issue.lineNumber)) : wxString("All"),
            wxui::toWx(issue.key.empty() ? issue.message : issue.key + ": " + issue.message)});
    }
    grid_->setRows(std::move(rows));
    if (selected >= 0) { grid_->SetGridCursor(selected, 0); grid_->SelectRow(selected); }
    rebuilding_ = false;
    select(selected);
    Layout();
}
void TxiDiagnosticsPanel::setJumpHandler(std::function<void(std::size_t)> handler) {
    jumpHandler_ = std::move(handler); select(selected_);
}
void TxiDiagnosticsPanel::setLookupHandler(std::function<void(const std::string&)> handler) {
    lookupHandler_ = std::move(handler); select(selected_);
}
void TxiDiagnosticsPanel::select(int row) {
    selected_ = row >= 0 && static_cast<std::size_t>(row) < issues_.size() ? row : -1;
    if (selected_ < 0) {
        details_->ChangeValue("No validation issues.");
        jump_->Disable(); lookup_->Disable(); return;
    }
    const auto& issue = issues_[selected_];
    const auto line = issue.lineNumber ? "Line " + std::to_string(issue.lineNumber) : "Document-wide check";
    const auto text = wxui::toWx(texture::txiIssueSeverityToString(issue.severity) + " | " + line +
        (issue.key.empty() ? "" : "\nKey: " + issue.key) + "\n\n" + issue.message);
    if (details_->GetValue() != text) {
        details_->ChangeValue(text);
        details_->SetInsertionPoint(0);
    }
    jump_->Enable(issue.lineNumber > 0 && static_cast<bool>(jumpHandler_));
    lookup_->Enable(texture::findTxiDirective(issue.key).has_value() && static_cast<bool>(lookupHandler_));
}
void TxiDiagnosticsPanel::jump() {
    if (selected_ >= 0 && issues_[selected_].lineNumber > 0 && jumpHandler_)
        jumpHandler_(issues_[selected_].lineNumber);
}

} // namespace neotpc
