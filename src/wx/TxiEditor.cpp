#include "TxiEditor.hpp"

#include "NeoWxUi.hpp"
#include "texture/Txi.hpp"

#include <wx/font.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace neotpc {
namespace {

std::string trimLeft(std::string value) {
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
    value.erase(0, first);
    return value;
}

std::string trim(std::string value) {
    value = trimLeft(std::move(value));
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    return value;
}

bool equalIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::string directiveFromLine(std::string line) {
    line = trimLeft(std::move(line));
    if (line.empty() || line.front() == '#' || line.front() == ';') return {};

    std::size_t end = 0;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
    std::string key = line.substr(0, end);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (key == "decal1") key = "decal";
    const auto directive = texture::findTxiDirective(key);
    return directive ? directive->name : std::string{};
}

std::string valueFromLine(std::string line) {
    line = trimLeft(std::move(line));
    if (line.empty() || line.front() == '#' || line.front() == ';') return {};
    std::size_t end = 0;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
    if (end == line.size()) return {};
    return trim(line.substr(end));
}

bool completionDelimiter(char ch, bool directive) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') return true;
    if (directive) return false;
    return ch == ',' || ch == '(' || ch == ')' || ch == '[' || ch == ']';
}

#ifndef __EMSCRIPTEN__
constexpr int kGhostAnnotationStyle = 240;

wxString completionList(const std::vector<std::string>& suggestions) {
    wxString out;
    for (std::size_t index = 0; index < suggestions.size(); ++index) {
        if (index != 0) out += '\n';
        out += wxui::toWx(suggestions[index]);
    }
    return out;
}
#else
std::string compactSuggestionSummary(const std::vector<std::string>& suggestions) {
    if (suggestions.empty()) return {};
    std::string out = std::to_string(suggestions.size()) + " matches: ";
    const std::size_t shown = std::min<std::size_t>(suggestions.size(), 6);
    for (std::size_t index = 0; index < shown; ++index) {
        if (index != 0) out += ", ";
        out += suggestions[index];
    }
    if (shown < suggestions.size()) out += ", ...";
    return out;
}
#endif

} // namespace

#ifdef __EMSCRIPTEN__

TxiEditor::TxiEditor(wxWindow* parent, wxWindowID id)
    : wxTextCtrl(parent, id, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                 wxTE_MULTILINE | wxTE_DONTWRAP) {
    Bind(wxEVT_TEXT, &TxiEditor::onText, this);
    Bind(wxEVT_KEY_DOWN, &TxiEditor::onKeyDown, this);
}

void TxiEditor::setValue(const wxString& value, bool preserveView) {
    if (preserveView && value == GetValue()) return;
    long begin = 0, end = 0;
    GetSelection(&begin, &end);
    const int vertical = GetScrollPos(wxVERTICAL), horizontal = GetScrollPos(wxHORIZONTAL);
    applyingCompletion_ = true;
    ChangeValue(value);
    if (preserveView) {
        const long length = GetLastPosition();
        SetSelection(std::clamp(begin, 0L, length), std::clamp(end, 0L, length));
        SetScrollPos(wxVERTICAL, vertical); SetScrollPos(wxHORIZONTAL, horizontal);
    } else SetInsertionPoint(0);
    applyingCompletion_ = false;
    clearPendingCompletion();
    updateHintForCaret();
}

wxString TxiEditor::value() const {
    return GetValue();
}

void TxiEditor::applyTheme(bool darkMode) {
    const wxui::ThemePalette palette = wxui::themePalette(darkMode);
    const int pointSize = std::max(9, GetFont().GetPointSize());
    SetFont(wxFont(wxFontInfo(pointSize).Family(wxFONTFAMILY_TELETYPE)));
    SetBackgroundColour(palette.field);
    SetForegroundColour(palette.text);
    Refresh(false);
}

void TxiEditor::goToOneBasedLine(std::size_t lineNumber) {
    if (lineNumber == 0) return;
    if (lineNumber > static_cast<std::size_t>(GetNumberOfLines())) return;
    const long position = XYToPosition(0, static_cast<long>(lineNumber - 1));
    if (position < 0) return;
    SetInsertionPoint(position);
    ShowPosition(position);
    SetFocus();
    updateCompletion(false);
}

void TxiEditor::setHintHandler(HintHandler handler) {
    hintHandler_ = std::move(handler);
    updateCompletion(false);
}

void TxiEditor::onText(wxCommandEvent& event) {
    if (!applyingCompletion_) updateCompletion(false);
    event.Skip();
}

#else

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
    AutoCompSetMaxWidth(56);

#if wxCHECK_VERSION(3, 3, 0)
    EOLAnnotationSetStyleOffset(kGhostAnnotationStyle);
    EOLAnnotationSetVisible(wxSTC_EOLANNOTATION_STANDARD);
#else
    CallTipUseStyle(0);
#endif

    Bind(wxEVT_STC_CHARADDED, &TxiEditor::onCharAdded, this);
    Bind(wxEVT_KEY_DOWN, &TxiEditor::onKeyDown, this);
    Bind(wxEVT_STC_AUTOCOMP_SELECTION_CHANGE,
         &TxiEditor::onAutocompleteSelectionChanged, this);
    Bind(wxEVT_STC_AUTOCOMP_COMPLETED, &TxiEditor::onAutocompleteCompleted, this);
    Bind(wxEVT_STC_UPDATEUI, &TxiEditor::onUpdateUi, this);
}

void TxiEditor::setValue(const wxString& value, bool preserveView) {
    if (preserveView && value == GetText()) return;
    const int caret = GetCurrentPos(), anchor = GetAnchor();
    const int firstLine = GetFirstVisibleLine(), horizontal = GetXOffset();
    if (AutoCompActive()) AutoCompCancel();
#if wxCHECK_VERSION(3, 3, 0)
    EOLAnnotationClearAll();
#else
    if (CallTipActive()) CallTipCancel();
#endif
    applyingCompletion_ = true;
    SetText(value);
    EmptyUndoBuffer(); // Document history, not Scintilla, owns Undo/Redo.
    SetSavePoint();
    if (preserveView) {
        const int length = GetTextLength();
        SetSelection(std::clamp(anchor, 0, length), std::clamp(caret, 0, length));
        SetFirstVisibleLine(std::clamp(firstLine, 0, std::max(0, GetLineCount() - 1)));
        SetXOffset(horizontal);
    } else GotoPos(0);
    applyingCompletion_ = false;
    clearPendingCompletion();
    lastCaretPosition_ = -1;
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
#if wxCHECK_VERSION(3, 3, 0)
    StyleSetFont(kGhostAnnotationStyle, editorFont);
    StyleSetForeground(kGhostAnnotationStyle, palette.mutedText);
    StyleSetBackground(kGhostAnnotationStyle, palette.field);
    EOLAnnotationSetStyleOffset(kGhostAnnotationStyle);
    EOLAnnotationSetVisible(wxSTC_EOLANNOTATION_STANDARD);
#else
    StyleSetFont(wxSTC_STYLE_CALLTIP, editorFont);
    StyleSetForeground(wxSTC_STYLE_CALLTIP, palette.mutedText);
    StyleSetBackground(wxSTC_STYLE_CALLTIP, palette.field);
#endif
    lastLineCount_ = 0;
    updateLineNumberMargin();

    SetCaretForeground(palette.text);
    SetCaretLineVisible(true);
    SetCaretLineBackground(palette.fieldAlt);
    SetCaretLineBackAlpha(darkMode ? 72 : 40);
    SetSelForeground(true, palette.selectionText);
    SetSelBackground(true, palette.selection);
#if !wxCHECK_VERSION(3, 3, 0)
    CallTipSetForeground(palette.mutedText);
    CallTipSetBackground(palette.field);
#endif
    SetBackgroundColour(palette.field);
    SetForegroundColour(palette.text);
    Refresh(false);
    updateCompletion(false);
}

void TxiEditor::goToOneBasedLine(std::size_t lineNumber) {
    if (lineNumber == 0) return;
    const int zeroBased = static_cast<int>(lineNumber - 1);
    const int position = PositionFromLine(zeroBased);
    if (position < 0) return;
    GotoPos(position);
    EnsureCaretVisible();
    SetFocus();
    updateCompletion(false);
}

void TxiEditor::setHintHandler(HintHandler handler) {
    hintHandler_ = std::move(handler);
    updateCompletion(false);
}

void TxiEditor::onCharAdded(wxStyledTextEvent& event) {
    const int key = event.GetKey();
    if (key == '\r' || key == '\n' || key == '#' || key == ';') {
        if (AutoCompActive()) AutoCompCancel();
        clearPendingCompletion();
        updateHintForCaret();
    } else if (!AutoCompActive()) {
        updateCompletion(false);
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
    const bool completedValue = completingValue_;
    const std::string completedDirective = completionDirective_;
    completingValue_ = false;
    completionDirective_.clear();
    clearPendingCompletion();
    if (completedValue) {
        setHintForValue(completedDirective, wxui::toStd(event.GetString()));
    } else {
        CallAfter(&TxiEditor::offerValuesAfterDirectiveCompletion);
    }
    event.Skip();
}

void TxiEditor::onUpdateUi(wxStyledTextEvent& event) {
    updateLineNumberMargin();
    const int caret = GetCurrentPos();
    if (!AutoCompActive() && !applyingCompletion_ && caret != lastCaretPosition_) {
        updateCompletion(false);
    }
    event.Skip();
}

#endif

void TxiEditor::onKeyDown(wxKeyEvent& event) {
    if (!event.ControlDown() && !event.MetaDown() && !event.AltDown() &&
        !event.ShiftDown() && event.GetKeyCode() == WXK_TAB && acceptPendingCompletion()) {
        return;
    }
    if ((event.ControlDown() || event.MetaDown()) && event.GetKeyCode() == WXK_SPACE) {
        updateCompletion(true);
        return;
    }
#ifndef __EMSCRIPTEN__
    if (event.GetKeyCode() == WXK_BACK || event.GetKeyCode() == WXK_DELETE) {
        event.Skip();
        CallAfter([this]() {
            if (!applyingCompletion_) updateCompletion(false);
        });
        return;
    }
#endif
    event.Skip();
}

void TxiEditor::updateCompletion(bool explicitRequest) {
#ifndef __EMSCRIPTEN__
    lastCaretPosition_ = GetCurrentPos();
#endif
    const std::string line = currentLineBeforeCaret();
    const auto result = texture::txiAutocomplete(line, explicitRequest);

#ifndef __EMSCRIPTEN__
    if (explicitRequest && !result.suggestions.empty()) {
        clearPendingCompletion();
        completingValue_ = result.kind == texture::TxiAutocompleteKind::Value;
        completionDirective_ = result.directive;
        AutoCompShow(static_cast<int>(result.replacementLength),
                     completionList(result.suggestions));
        return;
    }
#endif

    if (result.suggestions.size() == 1) {
        const std::string& suggestion = result.suggestions.front();
        const std::string typed = result.replacementLength <= line.size()
            ? line.substr(line.size() - result.replacementLength)
            : std::string{};
        const bool alreadyComplete = result.kind == texture::TxiAutocompleteKind::Directive
            ? equalIgnoreCase(typed, suggestion)
            : typed == suggestion;
        if (!alreadyComplete) {
            pendingCompletionKind_ = result.kind == texture::TxiAutocompleteKind::Directive
                ? PendingCompletionKind::Directive
                : PendingCompletionKind::Value;
            pendingCompletion_ = suggestion;
            pendingReplacementLength_ = result.replacementLength;
            completionDirective_ = result.directive;
            showGhostSuggestion(suggestion);
            return;
        }
    }

    clearPendingCompletion();
#ifdef __EMSCRIPTEN__
    if (explicitRequest && result.suggestions.size() > 1) {
        setHint(wxui::toWx(compactSuggestionSummary(result.suggestions)));
        return;
    }
#endif
    updateHintForCaret();
}

bool TxiEditor::acceptPendingCompletion() {
    if (pendingCompletionKind_ == PendingCompletionKind::None || pendingCompletion_.empty()) {
        return false;
    }

    const bool directive = pendingCompletionKind_ == PendingCompletionKind::Directive;
    applyingCompletion_ = true;

#ifdef __EMSCRIPTEN__
    const long caret = GetInsertionPoint();
    if (caret < 0 || pendingReplacementLength_ > static_cast<std::size_t>(caret)) {
        applyingCompletion_ = false;
        clearPendingCompletion();
        return false;
    }
    const long start = caret - static_cast<long>(pendingReplacementLength_);
    const wxString fullText = GetValue();
    long end = caret;
    while (end < static_cast<long>(fullText.length())) {
        const wxUniChar ch = fullText[static_cast<std::size_t>(end)];
        const unsigned long value = ch.GetValue();
        if (value > 0x7F || completionDelimiter(static_cast<char>(value), directive)) break;
        ++end;
    }
    SetSelection(start, end);
    WriteText(wxui::toWx(pendingCompletion_));
    long newCaret = start + static_cast<long>(pendingCompletion_.size());
    const wxString updated = GetValue();
    if (directive) {
        if (newCaret >= static_cast<long>(updated.length()) ||
            (updated[static_cast<std::size_t>(newCaret)] != ' ' &&
             updated[static_cast<std::size_t>(newCaret)] != '\t')) {
            SetInsertionPoint(newCaret);
            WriteText(" ");
            ++newCaret;
        } else {
            ++newCaret;
        }
    }
    SetInsertionPoint(newCaret);
#else
    const int caret = GetCurrentPos();
    if (caret < 0 || pendingReplacementLength_ > static_cast<std::size_t>(caret)) {
        applyingCompletion_ = false;
        clearPendingCompletion();
        return false;
    }
    const int start = caret - static_cast<int>(pendingReplacementLength_);
    int end = caret;
    while (end < GetTextLength()) {
        const int ch = GetCharAt(end);
        if (ch < 0 || ch > 0x7F || completionDelimiter(static_cast<char>(ch), directive)) break;
        ++end;
    }
    SetTargetStart(start);
    SetTargetEnd(end);
    ReplaceTarget(wxui::toWx(pendingCompletion_));
    int newCaret = start + static_cast<int>(pendingCompletion_.size());
    if (directive) {
        if (newCaret >= GetTextLength() ||
            (GetCharAt(newCaret) != ' ' && GetCharAt(newCaret) != '\t')) {
            InsertText(newCaret, " ");
            ++newCaret;
        } else {
            ++newCaret;
        }
    }
    GotoPos(newCaret);
    EnsureCaretVisible();
#endif

    applyingCompletion_ = false;
    clearPendingCompletion();
    updateCompletion(false);
    return true;
}

void TxiEditor::clearPendingCompletion() {
    pendingCompletionKind_ = PendingCompletionKind::None;
    pendingCompletion_.clear();
    pendingReplacementLength_ = 0;
    completionDirective_.clear();
#ifndef __EMSCRIPTEN__
#if wxCHECK_VERSION(3, 3, 0)
    EOLAnnotationClearAll();
#else
    if (CallTipActive()) CallTipCancel();
#endif
#endif
}

void TxiEditor::showGhostSuggestion(const std::string& suggestion) {
    setHint(wxui::toWx(suggestion + "    Tab to complete"));
#ifndef __EMSCRIPTEN__
#if wxCHECK_VERSION(3, 3, 0)
    EOLAnnotationClearAll();
    const int caret = GetCurrentPos();
    const int line = LineFromPosition(caret);
    if (caret != GetLineEndPosition(line)) return;

    std::string suffix = suggestion;
    if (pendingReplacementLength_ <= suffix.size()) {
        suffix.erase(0, pendingReplacementLength_);
    }
    if (suffix.empty()) return;
    EOLAnnotationSetText(line, wxui::toWx(suffix + "    Tab"));
    EOLAnnotationSetStyle(line, 0);
#else
    if (CallTipActive()) CallTipCancel();
    CallTipShow(GetCurrentPos(), wxui::toWx(suggestion + "    Tab"));
#endif
#endif
}

void TxiEditor::showTypeSignature(const std::string& signature) {
#ifndef __EMSCRIPTEN__
    if (signature.empty() || AutoCompActive()) return;
#if wxCHECK_VERSION(3, 3, 0)
    EOLAnnotationClearAll();
    const int caret = GetCurrentPos();
    const int line = LineFromPosition(caret);
    if (caret != GetLineEndPosition(line)) return;
    EOLAnnotationSetText(line, wxui::toWx("    " + signature));
    EOLAnnotationSetStyle(line, 0);
#else
    if (CallTipActive()) CallTipCancel();
    CallTipShow(GetCurrentPos(), wxui::toWx(signature));
#endif
#else
    (void)signature;
#endif
}

void TxiEditor::updateHintForCaret() {
    const std::string directive = currentDirective();
    if (directive.empty()) {
        setHint("Start typing a TXI directive. Tab accepts a unique completion; Ctrl+Space lists matches.");
        return;
    }
    const std::string value = currentValue();
    if (!value.empty()) {
        setHintForValue(directive, value);
    } else {
        setHintForDirective(directive);
    }
}

void TxiEditor::setHintForDirective(const std::string& directive) {
    if (directive.empty()) return;
    const std::string hint = texture::txiDirectiveHint(directive);
    if (!hint.empty()) setHint(wxui::toWx(hint));
    showTypeSignature(texture::txiDirectiveSignature(directive));
}

void TxiEditor::setHintForValue(const std::string& directive, const std::string& value) {
    if (directive.empty() || value.empty()) return;
    const std::string hint = texture::txiValueHint(directive, value);
    if (!hint.empty()) setHint(wxui::toWx(hint));
#ifndef __EMSCRIPTEN__
#if wxCHECK_VERSION(3, 3, 0)
    EOLAnnotationClearAll();
#else
    if (CallTipActive()) CallTipCancel();
#endif
#endif
}

void TxiEditor::setHint(const wxString& text) {
    if (hintHandler_) hintHandler_(text);
}

#ifndef __EMSCRIPTEN__
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
    if (previous != ' ' && previous != '\t') AddText(" ");
    updateCompletion(false);
}
#endif

std::string TxiEditor::currentLineBeforeCaret() {
#ifdef __EMSCRIPTEN__
    const long caret = GetInsertionPoint();
    std::string before = wxui::toStd(GetRange(0, caret));
    const std::size_t start = before.find_last_of("\r\n");
    if (start != std::string::npos) before.erase(0, start + 1);
    return before;
#else
    const int caret = GetCurrentPos();
    const int line = LineFromPosition(caret);
    const int start = PositionFromLine(line);
    return wxui::toStd(GetTextRange(start, caret));
#endif
}

std::string TxiEditor::currentDirective() {
    return directiveFromLine(currentLineBeforeCaret());
}

std::string TxiEditor::currentValue() {
    return valueFromLine(currentLineBeforeCaret());
}

} // namespace neotpc
