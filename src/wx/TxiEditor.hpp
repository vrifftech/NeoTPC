#pragma once

#ifdef __EMSCRIPTEN__
#include <wx/textctrl.h>
#else
#include <wx/stc/stc.h>
#endif

#include <cstddef>
#include <functional>
#include <string>

namespace neotpc {

#ifdef __EMSCRIPTEN__
class TxiEditor final : public wxTextCtrl {
#else
class TxiEditor final : public wxStyledTextCtrl {
#endif
public:
    using HintHandler = std::function<void(const wxString&)>;

    explicit TxiEditor(wxWindow* parent, wxWindowID id = wxID_ANY);

    // Refreshing the current document keeps its caret, selection and viewport.
    // A new document explicitly requests a reset.
    void setValue(const wxString& value, bool preserveView = true);
    wxString value() const;
    void applyTheme(bool darkMode);
    void goToOneBasedLine(std::size_t lineNumber);
    void setHintHandler(HintHandler handler);

private:
    enum class PendingCompletionKind {
        None,
        Directive,
        Value,
    };

#ifdef __EMSCRIPTEN__
    void onText(wxCommandEvent& event);
#else
    void onCharAdded(wxStyledTextEvent& event);
    void onAutocompleteSelectionChanged(wxStyledTextEvent& event);
    void onAutocompleteCompleted(wxStyledTextEvent& event);
    void onUpdateUi(wxStyledTextEvent& event);
#endif
    void onKeyDown(wxKeyEvent& event);

    void updateCompletion(bool explicitRequest);
    bool acceptPendingCompletion();
    void clearPendingCompletion();
    void showGhostSuggestion(const std::string& suggestion);
    void showTypeSignature(const std::string& signature);
    void updateHintForCaret();
    void setHintForDirective(const std::string& directive);
    void setHintForValue(const std::string& directive, const std::string& value);
    void setHint(const wxString& text);
#ifndef __EMSCRIPTEN__
    void updateLineNumberMargin();
    void offerValuesAfterDirectiveCompletion();
#endif
    std::string currentLineBeforeCaret();
    std::string currentDirective();
    std::string currentValue();

    HintHandler hintHandler_;
    PendingCompletionKind pendingCompletionKind_ = PendingCompletionKind::None;
    std::string pendingCompletion_;
    std::size_t pendingReplacementLength_ = 0;
    std::string completionDirective_;
    bool applyingCompletion_ = false;
#ifndef __EMSCRIPTEN__
    int lastLineCount_ = 0;
    int lastCaretPosition_ = -1;
    bool completingValue_ = false;
#endif
};

} // namespace neotpc
