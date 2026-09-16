#pragma once

#include <wx/dialog.h>

#if defined(__EMSCRIPTEN__)
#include <neoshared/wasm_dialog_compat.h>
#endif

#include "texture/BatchConverter.hpp"
#include <optional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxDirPickerCtrl;
class wxGauge;
class wxListCtrl;
class wxStaticText;
class wxTextCtrl;

namespace neotpc {
namespace layout { class WrappedCheckBox; class WrappedLabel; }

class EncodingOptionsPanel;

// Both callbacks run on the UI thread. Prepare contributes to the existing
// batch confirmation; completed sees only the outcomes of the finished job.
struct BatchEditorHooks {
    std::function<std::string(const texture::BatchPlan&)> prepare;
    std::function<void(wxWindow*, const texture::BatchReport&)> completed;
};

#if defined(__EMSCRIPTEN__)
struct BrowserRetainedInputFile {
    std::uint32_t fileId = 0;
    std::string relativePath;
    std::uint64_t size = 0;
};

struct BrowserBatchState;
#endif

class BatchDialog final : public wxDialog {
public:
    BatchDialog(wxWindow* parent, const wxString& initialDirectory, bool darkMode,
                BatchEditorHooks editor = {});
    ~BatchDialog() override;

private:
    BatchEditorHooks editor_;
    void onConvert();
    void onScan();
    void invalidatePlan();
    void showRows(const std::vector<texture::BatchItemResult>& rows, bool selectReady);
    void setSelectedRows(bool include);
    void refreshPlanCount();
    void refreshSelectionActions();
    void showSelectedDetails();
    bool acceptsInput(const std::filesystem::path& input) const;
    std::optional<texture::BatchPlan> plan_;
    std::vector<texture::BatchItemResult> displayedRows_;
    std::vector<bool> included_;
    wxListCtrl* items_ = nullptr;
    wxChoice* inputType_ = nullptr;
    wxChoice* matchingFormat_ = nullptr;
    wxButton* scanButton_ = nullptr;
    layout::WrappedLabel* resultSummary_ = nullptr;
    bool outputChosen_ = false;
    bool updatingOutput_ = false;
    bool planReady_ = false;
    bool nativeBusy_ = false;

#if defined(__EMSCRIPTEN__)
    void requestBrowserInputDirectory();
    void installBrowserInput(std::uint32_t sessionId,
                             std::string displayName,
                             std::vector<BrowserRetainedInputFile> files);
    void releaseBrowserInput() noexcept;
    void startBrowserConversion(bool reviewOnly = false);
    void continueBrowserConversion();
    void handleBrowserSourceRead(std::uint64_t generation,
                                 std::vector<std::uint8_t> bytes,
                                 std::string error);
    void handleBrowserSidecarRead(std::uint64_t generation,
                                  std::vector<std::uint8_t> bytes,
                                  std::string error);
    void encodeBrowserBatchItem(std::uint64_t generation);
    void completeBrowserBatchItem(bool converted, std::string message);
    void finishBrowserConversion();
    void cancelBrowserConversion();
    void cleanupBrowserOutputs() noexcept;
    void setBrowserControlsBusy(bool busy);
#endif

#if defined(__EMSCRIPTEN__)
    wxTextCtrl* inputDirectory_ = nullptr;
    wxButton* inputBrowse_ = nullptr;
#else
    wxDirPickerCtrl* inputDirectory_ = nullptr;
#endif
    wxDirPickerCtrl* outputDirectory_ = nullptr;
    wxChoice* format_ = nullptr;
    layout::WrappedCheckBox* recursive_ = nullptr;
    layout::WrappedCheckBox* overwrite_ = nullptr;
    EncodingOptionsPanel* options_ = nullptr;
    wxTextCtrl* report_ = nullptr;
    wxButton* convertButton_ = nullptr;
    wxButton* closeButton_ = nullptr;
#if defined(__EMSCRIPTEN__)
    wxGauge* browserProgress_ = nullptr;
    std::uint32_t browserInputSessionId_ = 0;
    std::string browserInputDisplayName_;
    std::vector<BrowserRetainedInputFile> browserInputFiles_;
    std::uint64_t browserSelectionGeneration_ = 0;
    std::uint64_t browserConversionGeneration_ = 0;
    std::unique_ptr<BrowserBatchState> browserBatch_;
    std::unique_ptr<BrowserBatchState> browserPlan_;
    std::vector<std::filesystem::path> browserExcludedInputs_;
#endif
};

} // namespace neotpc
