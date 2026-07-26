#include "TxiEditor.hpp"

#include "NeoWxUi.hpp"
#include "texture/Txi.hpp"

#include <wx/font.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace neotpc {
namespace {

std::string directiveFromLine(std::string line) {
    std::size_t first = 0;
    while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
    if (first == line.size() || line[first] == '#' || line[first] == ';') return {};

    std::size_t end = first;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
    std::string key = line.substr(first, end - first);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (key == "decal1") key = "decal";
    const auto directive = texture::findTxiDirective(key);
    if (!directive || directive->valueKind == texture::TxiDirectiveValueKind::ValueToken) return {};
    return directive->name;
}

wxString completionList(const std::vector<std::string>& suggestions) {
    wxString out;
    for (std::size_t index = 0; index < suggestions.size(); ++index) {
        if (index != 0) out += '\n';
        out += wxui::toWx(suggestions[index]);
    }
    return out;
}

} // namespace

TxiEditor::TxiEditor(wxWindow* parent, wxWindowID id)
    : wxStyledTextCtrl(parent, id) {
    SetCodePage(wxSTC_CP_UTF8);
    SetEOLMode(wxSTC_EOL_LF);
    SetWrapMode(wxSTC_WRAP_NONE);
    SetUseTabs(false);
    SetTabWidth(4);
    SetIndent(4);
    SetScrollWidthTracking(true);
    SetEndAtLastLine(false);

    SetMarginType(0, wxSTC_MARGIN_NUMBER);
    updateLineNumberMargin();

    AutoCompSetSeparator('\n');
    AutoCompSetIgnoreCase(true);
    AutoCompSetChooseSingle(false);
    AutoCompSetAutoHide(true);
    AutoCompSetDropRestOfWord(true);
    AutoCompSetCancelAtStart(true);
    AutoCompSetFillUps(" \t");
    AutoCompStops("\r\n;#");
    AutoCompSetMaxHeight(12);
    AutoCompSetMaxWidth(48);

    Bind(wxEVT_STC_CHARADDED, &TxiEditor::onCharAdded, this);
    Bind(wxEVT_KEY_DOWN, &TxiEditor::onKeyDown, this);
    Bind(wxEVT_STC_AUTOCOMP_SELECTION_CHANGE,
         &TxiEditor::onAutocompleteSelectionChanged, this);
    Bind(wxEVT_STC_AUTOCOMP_COMPLETED, &TxiEditor::onAutocompleteCompleted, this);
    Bind(wxEVT_STC_UPDATEUI, &TxiEditor::onUpdateUi, this);
}

void TxiEditor::setValue(const wxString& value) {
    if (AutoCompActive()) AutoCompCancel();
    SetText(value);
    EmptyUndoBuffer();
    SetSavePoint();
    GotoPos(0);
    updateLineNumberMargin();
    updateHintForCaret();
}

wxString TxiEditor::value() const {
    return GetText();
}

void TxiEditor::applyTheme(bool darkMode) {
    const wxui::ThemePalette palette = wxui::themePalette(darkMode);
    const int pointSize = std::max(9, GetFont().GetPointSize());
    const wxFont editorFont(wxFontInfo(pointSize).Family(wxFONTFAMILY_TELETYPE));

    StyleSetFont(wxSTC_STYLE_DEFAULT, editorFont);
    StyleSetForeground(wxSTC_STYLE_DEFAULT, palette.text);
    StyleSetBackground(wxSTC_STYLE_DEFAULT, palette.field);
    StyleClearAll();

    StyleSetFont(wxSTC_STYLE_LINENUMBER, editorFont);
    StyleSetForeground(wxSTC_STYLE_LINENUMBER, palette.mutedText);
    StyleSetBackground(wxSTC_STYLE_LINENUMBER, palette.panel);
    lastLineCount_ = 0;
    updateLineNumberMargin();

    SetCaretForeground(palette.text);
    SetCaretLineVisible(true);
    SetCaretLineBackground(palette.fieldAlt);
    SetCaretLineBackAlpha(darkMode ? 72 : 40);
    SetSelForeground(true, palette.selectionText);
    SetSelBackground(true, palette.selection);
    CallTipSetForeground(palette.text);
    CallTipSetBackground(palette.button);
    SetBackgroundColour(palette.field);
    SetForegroundColour(palette.text);
    Refresh(false);
}

void TxiEditor::goToOneBasedLine(std::size_t lineNumber) {
    if (lineNumber == 0) return;
    const int zeroBased = static_cast<int>(lineNumber - 1);
    const int position = PositionFromLine(zeroBased);
    if (position < 0) return;
    GotoPos(position);
    EnsureCaretVisible();
    SetFocus();
}

void TxiEditor::setHintHandler(HintHandler handler) {
    hintHandler_ = std::move(handler);
    updateHintForCaret();
}

void TxiEditor::onCharAdded(wxStyledTextEvent& event) {
    const int key = event.GetKey();
    if (key == '\r' || key == '\n' || key == '#' || key == ';') {
        if (AutoCompActive()) AutoCompCancel();
        updateHintForCaret();
    } else {
        updateAutocomplete(false);
    }
    event.Skip();
}

void TxiEditor::onKeyDown(wxKeyEvent& event) {
    if ((event.ControlDown() || event.MetaDown()) && event.GetKeyCode() == WXK_SPACE) {
        updateAutocomplete(true);
        return;
    }
    event.Skip();
}

void TxiEditor::onAutocompleteSelectionChanged(wxStyledTextEvent& event) {
    if (completingValue_) {
        setHintForValue(completionDirective_, wxui::toStd(event.GetString()));
    } else {
        setHintForDirective(wxui::toStd(event.GetString()));
    }
    event.Skip();
}

void TxiEditor::onAutocompleteCompleted(wxStyledTextEvent& event) {
    if (completingValue_) {
        setHintForValue(completionDirective_, wxui::toStd(event.GetString()));
    } else {
        CallAfter(&TxiEditor::offerValuesAfterDirectiveCompletion);
    }
    event.Skip();
}

void TxiEditor::onUpdateUi(wxStyledTextEvent& event) {
    updateLineNumberMargin();
    if (!AutoCompActive()) updateHintForCaret();
    event.Skip();
}

void TxiEditor::updateAutocomplete(bool explicitRequest) {
    const auto result = texture::txiAutocomplete(currentLineBeforeCaret(), explicitRequest);
    if (result.suggestions.empty()) {
        if (AutoCompActive()) AutoCompCancel();
        completingValue_ = false;
        completionDirective_.clear();
        updateHintForCaret();
        return;
    }

    completingValue_ = result.kind == texture::TxiAutocompleteKind::Value;
    completionDirective_ = result.directive;
    AutoCompShow(static_cast<int>(result.replacementLength), completionList(result.suggestions));

    if (completingValue_) {
        if (result.suggestions.size() == 1) {
            setHintForValue(completionDirective_, result.suggestions.front());
        } else {
            setHintForDirective(completionDirective_);
        }
    } else if (result.suggestions.size() == 1) {
        setHintForDirective(result.suggestions.front());
    }
}

void TxiEditor::updateHintForCaret() {
    const std::string directive = currentDirective();
    if (directive.empty()) {
        lastHintDirective_.clear();
        lastHintValue_.clear();
        setHint("Type a TXI directive or press Ctrl+Space for suggestions.");
        return;
    }
    setHintForDirective(directive);
}

void TxiEditor::setHintForDirective(const std::string& directive) {
    if (directive.empty()) return;
    if (directive == lastHintDirective_ && lastHintValue_.empty()) return;
    lastHintDirective_ = directive;
    lastHintValue_.clear();
    const std::string hint = texture::txiDirectiveHint(directive);
    if (hint.empty()) return;
    setHint(wxui::toWx(hint));
}

void TxiEditor::setHintForValue(const std::string& directive, const std::string& value) {
    if (directive.empty() || value.empty()) return;
    if (directive == lastHintDirective_ && value == lastHintValue_) return;
    lastHintDirective_ = directive;
    lastHintValue_ = value;
    const std::string hint = texture::txiValueHint(directive, value);
    if (hint.empty()) {
        setHintForDirective(directive);
        return;
    }
    setHint(wxui::toWx(hint));
}

void TxiEditor::setHint(const wxString& text) {
    if (hintHandler_) hintHandler_(text);
}

void TxiEditor::updateLineNumberMargin() {
    const int lineCount = std::max(1, GetLineCount());
    if (lineCount == lastLineCount_) return;
    lastLineCount_ = lineCount;
    const int digits = std::max(3, static_cast<int>(std::to_string(lineCount).size()));
    wxString sample = "_";
    for (int index = 0; index < digits; ++index) sample += '9';
    SetMarginWidth(0, TextWidth(wxSTC_STYLE_LINENUMBER, sample));
}

void TxiEditor::offerValuesAfterDirectiveCompletion() {
    const int caret = GetCurrentPos();
    if (caret <= 0) return;

    const int previous = GetCharAt(caret - 1);
    if (previous != ' ' && previous != '\t') {
        AddText(" ");
    }
    updateAutocomplete(false);
}

std::string TxiEditor::currentLineBeforeCaret() {
    const int caret = GetCurrentPos();
    const int line = LineFromPosition(caret);
    const int start = PositionFromLine(line);
    return wxui::toStd(GetTextRange(start, caret));
}

std::string TxiEditor::currentDirective() {
    return directiveFromLine(currentLineBeforeCaret());
}

} // namespace neotpc
