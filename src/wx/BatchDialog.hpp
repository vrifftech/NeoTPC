#pragma once

#include <wx/dialog.h>

#if defined(__EMSCRIPTEN__)
#include <neoshared/wasm_dialog_compat.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxDirPickerCtrl;
class wxGauge;
class wxTextCtrl;

namespace neotpc {

class EncodingOptionsPanel;

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
    BatchDialog(wxWindow* parent, const wxString& initialDirectory, bool darkMode);
    ~BatchDialog() override;

private:
    void onConvert();

#if defined(__EMSCRIPTEN__)
    void requestBrowserInputDirectory();
    void installBrowserInput(std::uint32_t sessionId,
                             std::string displayName,
                             std::vector<BrowserRetainedInputFile> files);
    void releaseBrowserInput() noexcept;
    void startBrowserConversion();
    void continueBrowserConversion();
    void handleBrowserSourceRead(std::uint64_t generation,
                                 std::vector<std::uint8_t> bytes,
                                 std::string error);
    void handleBrowserSidecarRead(std::uint64_t generation,
                                  std::vector<std::uint8_t> bytes,
                                  std::string error);
    void encodeBrowserBatchItem(std::uint64_t generation);
    void publishNextBrowserOutput(std::uint64_t generation);
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
    wxCheckBox* recursive_ = nullptr;
    wxCheckBox* overwrite_ = nullptr;
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
#endif
};

} // namespace neotpc
