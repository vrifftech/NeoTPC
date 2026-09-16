#include "TxiDictionaryPanel.hpp"

#include "NeoWxUi.hpp"
#include "ResponsiveLayout.hpp"
#include "WrappedReadOnlyGrid.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/msgdlg.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>

namespace neotpc {

TxiDictionaryPanel::TxiDictionaryPanel(wxWindow* parent) : wxPanel(parent) {
    SetMinSize(wxSize(1, 1));
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* help = new layout::WrappedLabel(this, wxID_ANY,
        "Read-only TXI dictionary. Select a row for the complete value rules and notes. Click a heading to sort.");
    root->Add(help, 0, wxEXPAND | wxALL, FromDIP(8));
    search_ = new wxTextCtrl(this, wxID_ANY);
    search_->SetName("Search TXI dictionary");
    search_->SetHint("Search names, values, defaults or notes...");
    search_->SetMinSize(wxSize(1, -1));
    root->Add(search_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    category_ = new wxChoice(this, wxID_ANY);
    category_->Append("All categories");
    for (const auto& value : txidictionary::categories()) category_->Append(wxui::toWx(value));
    category_->SetSelection(0);
    category_->SetName("TXI dictionary category");
    category_->SetMinSize(FromDIP(wxSize(140, -1)));
    auto* filters = new wxWrapSizer(wxHORIZONTAL);
    filters->Add(category_, 1, wxEXPAND | wxRIGHT | wxBOTTOM, FromDIP(6));
    auto* clear = new wxButton(this, wxID_ANY, "Clear filters");
    filters->Add(clear, 0, wxBOTTOM, FromDIP(6));
    root->Add(filters, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    count_ = new layout::WrappedLabel(this, wxID_ANY, wxEmptyString);
    root->Add(count_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    grid_ = new layout::WrappedReadOnlyGrid(this, {
        {"Directive", 14, 36, false}, {"Type", 12, 16, true},
        {"Default", 10, 14, true}, {"Meaning", 22, 64, false}});
    grid_->SetName("TXI dictionary table");
    grid_->SetToolTip("Select a directive for full details. Ctrl+C copies its reference, not an insertion template.");
    wxui::configureStableGridRendering(*grid_);
    root->Add(grid_, 3, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    root->Add(new wxStaticText(this, wxID_ANY, "Selected directive"), 0,
              wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    details_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    details_->SetName("Full TXI directive details");
    details_->SetMinSize(FromDIP(wxSize(1, 140)));
    root->Add(details_, 2, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    auto* actions = new wxWrapSizer(wxHORIZONTAL);
    copyName_ = new wxButton(this, wxID_ANY, "Copy name");
    copyDetails_ = new wxButton(this, wxID_ANY, "Copy reference");
    actions->Add(copyName_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
    actions->Add(copyDetails_, 0, wxBOTTOM, FromDIP(6));
    root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    SetSizer(root);

    search_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refreshRows(selectedDirective()); });
    category_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refreshRows(selectedDirective()); });
    clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const auto selected = selectedDirective();
        search_->ChangeValue(wxEmptyString); category_->SetSelection(0);
        refreshRows(selected); search_->SetFocus();
    });
    search_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_DOWN && !entries_.empty()) {
            grid_->SetFocus(); grid_->SetGridCursor(std::max(0, selected_), 0);
            grid_->MakeCellVisible(std::max(0, selected_), 0);
        } else event.Skip();
    });
    grid_->Bind(wxEVT_GRID_SELECT_CELL, [this](wxGridEvent& event) {
        if (!rebuilding_) showDetails(event.GetRow());
        event.Skip();
    });
    grid_->Bind(wxEVT_GRID_LABEL_LEFT_CLICK, [this](wxGridEvent& event) {
        if (event.GetCol() < 0) { event.Skip(); return; }
        const auto selected = selectedDirective();
        if (sortColumn_ == event.GetCol()) ascending_ = !ascending_;
        else { sortColumn_ = event.GetCol(); ascending_ = true; }
        refreshRows(selected);
    });
    grid_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        if (event.CmdDown() && !event.AltDown() && (event.GetKeyCode() == 'C')) copySelection(false);
        else event.Skip();
    });
    copyName_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { copySelection(true); });
    copyDetails_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { copySelection(false); });
    refreshRows();
}

bool TxiDictionaryPanel::Layout() {
    const bool result = wxPanel::Layout();
    if (grid_) grid_->reflow();
    return result;
}

std::string TxiDictionaryPanel::query() const { return wxui::toStd(search_->GetValue()); }
std::string TxiDictionaryPanel::category() const {
    return category_->GetSelection() <= 0 ? std::string{} : wxui::toStd(category_->GetStringSelection());
}
void TxiDictionaryPanel::setQuery(const std::string& queryText, const std::string& group) {
    const auto selected = selectedDirective();
    search_->ChangeValue(wxui::toWx(queryText));
    const int index = group.empty() ? 0 : category_->FindString(wxui::toWx(group));
    category_->SetSelection(index == wxNOT_FOUND ? 0 : index);
    refreshRows(selected);
}
void TxiDictionaryPanel::focusSearch() { search_->SetFocus(); search_->SelectAll(); }
std::string TxiDictionaryPanel::selectedDirective() const {
    return selected_ >= 0 && static_cast<std::size_t>(selected_) < entries_.size()
        ? entries_[selected_]->name : std::string{};
}
void TxiDictionaryPanel::restoreSelection(const std::string& name) { refreshRows(name); }
void TxiDictionaryPanel::selectDirective(const std::string& name) {
    const auto canonical = texture::findTxiDirective(name);
    if (!canonical) return;
    // An explicit lookup must not be hidden by a prior category/search filter.
    search_->ChangeValue(wxEmptyString); category_->SetSelection(0);
    refreshRows(canonical->name);
}
void TxiDictionaryPanel::refreshRows(const std::string& preferred) {
    rebuilding_ = true;
    entries_ = txidictionary::find(query(), category(), sortColumn_, ascending_);
    layout::WrappedReadOnlyGrid::Rows rows;
    int row = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = *entries_[i];
        rows.push_back({wxui::toWx(entry.name), wxui::toWx(texture::txiDirectiveValueKindToString(entry.valueKind)),
                        wxui::toWx(entry.defaultValue), wxui::toWx(entry.description)});
        if (entry.name == preferred) row = static_cast<int>(i);
    }
    grid_->setRows(std::move(rows));
    grid_->SetSortingColumn(sortColumn_, ascending_);
    if (!entries_.empty()) {
        grid_->SetGridCursor(row, 0); grid_->SelectRow(row); grid_->MakeCellVisible(row, 0);
    }
    rebuilding_ = false;
    showDetails(entries_.empty() ? -1 : row);
    const char* headings[] = {"Directive", "Type", "Default", "Meaning"};
    count_->SetLabel(entries_.empty() ? wxString("No matching directives. Clear filters to see the full dictionary.")
        : wxString::Format("%llu of %llu directives. ",
            static_cast<unsigned long long>(entries_.size()),
            static_cast<unsigned long long>(texture::txiDirectiveCatalog().size())) +
          "Sorted by " + wxui::toWx(headings[sortColumn_]) + (ascending_ ? " (ascending)." : " (descending)."));
    Layout();
}
void TxiDictionaryPanel::showDetails(int row) {
    selected_ = row >= 0 && static_cast<std::size_t>(row) < entries_.size() ? row : -1;
    const auto text = selected_ < 0 ? wxString("No directive selected.")
        : wxui::toWx(txidictionary::details(*entries_[selected_]));
    if (details_->GetValue() != text) {
        details_->ChangeValue(text);
        details_->SetInsertionPoint(0);
    }
    copyName_->Enable(selected_ >= 0); copyDetails_->Enable(selected_ >= 0);
}
void TxiDictionaryPanel::copySelection(bool nameOnly) {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= entries_.size()) return;
    const auto& item = *entries_[selected_];
    const auto text = wxui::toWx(nameOnly ? item.name : txidictionary::details(item));
    bool copied = false;
    if (wxTheClipboard && wxTheClipboard->Open()) {
        copied = wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
    if (!copied) wxMessageBox("The clipboard is unavailable. You can select and copy text in the details pane.",
                              "Copy TXI reference", wxOK | wxICON_INFORMATION, this);
}

} // namespace neotpc
