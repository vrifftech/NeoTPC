#pragma once

#include "texture/Txi.hpp"
#include <wx/panel.h>
#include <functional>
#include <vector>

class wxTextCtrl;
class wxButton;

namespace neotpc {
namespace layout { class WrappedReadOnlyGrid; }

class TxiDiagnosticsPanel final : public wxPanel {
public:
    explicit TxiDiagnosticsPanel(wxWindow* parent);
    void setIssues(std::vector<texture::TxiValidationIssue> issues);
    void setJumpHandler(std::function<void(std::size_t)> handler);
    void setLookupHandler(std::function<void(const std::string&)> handler);
    bool Layout() override;
private:
    void select(int row);
    void jump();
    layout::WrappedReadOnlyGrid* grid_ = nullptr;
    wxTextCtrl* details_ = nullptr;
    wxButton* jump_ = nullptr;
    wxButton* lookup_ = nullptr;
    std::vector<texture::TxiValidationIssue> issues_;
    std::function<void(std::size_t)> jumpHandler_;
    std::function<void(const std::string&)> lookupHandler_;
    int selected_ = -1;
    bool rebuilding_ = false;
};

} // namespace neotpc
