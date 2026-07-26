#pragma once

#include <wx/stc/stc.h>

#include <cstddef>
#include <functional>
#include <string>

namespace neotpc {

class TxiEditor final : public wxStyledTextCtrl {
public:
    using HintHandler = std::function<void(const wxString&)>;

    explicit TxiEditor(wxWindow* parent, wxWindowID id = wxID_ANY);

    void setValue(const wxString& value);
    wxString value() const;
    void applyTheme(bool darkMode);
    void goToOneBasedLine(std::size_t lineNumber);
    void setHintHandler(HintHandler handler);

private:
    void onCharAdded(wxStyledTextEvent& event);
    void onKeyDown(wxKeyEvent& event);
    void onAutocompleteSelectionChanged(wxStyledTextEvent& event);
    void onAutocompleteCompleted(wxStyledTextEvent& event);
    void onUpdateUi(wxStyledTextEvent& event);

    void updateAutocomplete(bool explicitRequest);
    void updateHintForCaret();
    void setHintForDirective(const std::string& directive);
    void setHintForValue(const std::string& directive, const std::string& value);
    void setHint(const wxString& text);
    void updateLineNumberMargin();
    void offerValuesAfterDirectiveCompletion();
    std::string currentLineBeforeCaret();
    std::string currentDirective();

    HintHandler hintHandler_;
    std::string completionDirective_;
    std::string lastHintDirective_;
    std::string lastHintValue_;
    int lastLineCount_ = 0;
    bool completingValue_ = false;
};

} // namespace neotpc
