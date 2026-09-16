#pragma once

#include "TxiDictionaryModel.hpp"
#include <wx/panel.h>
#include <string>
#include <vector>

class wxTextCtrl;
class wxChoice;
class wxButton;

namespace neotpc {
namespace layout { class WrappedReadOnlyGrid; class WrappedLabel; }

class TxiDictionaryPanel final : public wxPanel {
public:
    explicit TxiDictionaryPanel(wxWindow* parent);
    bool Layout() override;
    void setQuery(const std::string& query, const std::string& category = {});
    std::string query() const;
    std::string category() const;
    void restoreSelection(const std::string& name);
    void selectDirective(const std::string& name);
    std::string selectedDirective() const;
    void focusSearch();

private:
    void refreshRows(const std::string& preferred = {});
    void showDetails(int row);
    void copySelection(bool nameOnly);
    wxTextCtrl* search_ = nullptr;
    wxChoice* category_ = nullptr;
    layout::WrappedReadOnlyGrid* grid_ = nullptr;
    layout::WrappedLabel* count_ = nullptr;
    wxTextCtrl* details_ = nullptr;
    wxButton* copyName_ = nullptr;
    wxButton* copyDetails_ = nullptr;
    std::vector<const texture::TxiDirectiveInfo*> entries_;
    int selected_ = -1;
    int sortColumn_ = 0;
    bool ascending_ = true;
    bool rebuilding_ = false;
};

} // namespace neotpc
