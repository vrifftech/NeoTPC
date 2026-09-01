#include "BatchDialog.hpp"

#include "BrowserWorkload.hpp"
#include "EncodingOptionsPanel.hpp"
#include "PathUtils.hpp"
#include "texture/BatchConverter.hpp"
#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"
#include "texture/ParserLimits.hpp"
#include "NeoWxUi.hpp"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filepicker.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(__EMSCRIPTEN__)
#include "NeoBrowserFiles.hpp"
#include <neoshared/wasm_dialog_compat.h>
#endif

namespace fs = std::filesystem;

namespace neotpc {
namespace {

enum : int {
    ID_CONVERT = wxID_HIGHEST + 470,
    ID_SELECT_BROWSER_INPUT,
};

#if defined(__EMSCRIPTEN__)

constexpr std::size_t kBrowserBatchMaxItems = 256;
constexpr std::size_t kBrowserBatchMaxRetainedFiles = 512;
constexpr std::size_t kBrowserBatchMaxPathDepth = 24;
constexpr std::size_t kBrowserBatchMaxTextureBytes = 64u * 1024u * 1024u;
constexpr std::size_t kBrowserBatchMaxTxiBytes = 1u * 1024u * 1024u;
constexpr std::uint64_t kBrowserBatchMaxAggregateBytes = UINT64_C(1024) * 1024 * 1024;
constexpr std::uint64_t kBrowserBatchMaxDecodedBytes = UINT64_C(64) * 1024 * 1024;
constexpr std::uintmax_t kBrowserBatchMaxOutputBytes = UINT64_C(96) * 1024 * 1024;

const char* browserBatchAccept() {
    return ".tpc,.txb,.tga,.dds,.png,.jpg,.jpeg,.jpe,.bmp,.txi";
}

bool isPixelExtension(const std::string& extension) {
    return extension == "tpc" || extension == "txb" || extension == "tga" ||
           extension == "dds" || extension == "png" || extension == "jpg" ||
           extension == "jpeg" || extension == "jpe" || extension == "bmp";
}

bool outputUsesSidecar(const std::string& extension) {
    return extension == "tga" || extension == "dds" || extension == "png" ||
           extension == "jpg" || extension == "bmp";
}

std::string normalizeOutputExtension(std::string extension) {
    extension = neotpc::texture::asciiLower(std::move(extension));
    while (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    if (extension == "jpeg" || extension == "jpe") return "jpg";
    return extension;
}

std::optional<fs::path> normalizeBrowserRelativePath(const std::string& value,
                                                     std::string& error) {
    std::string portable = value;
    std::replace(portable.begin(), portable.end(), '\\', '/');
    while (!portable.empty() && portable.front() == '/') portable.erase(portable.begin());
    if (portable.empty()) {
        error = "A selected browser file has an empty relative path.";
        return std::nullopt;
    }

    fs::path result;
    std::size_t depth = 0;
    std::size_t begin = 0;
    while (begin <= portable.size()) {
        const std::size_t slash = portable.find('/', begin);
        const std::size_t end = slash == std::string::npos ? portable.size() : slash;
        const std::string component = portable.substr(begin, end - begin);
        if (!component.empty() && component != ".") {
            if (component == "..") {
                error = "A selected browser path contains a parent-directory component.";
                return std::nullopt;
            }
            if (component.find(':') != std::string::npos ||
                component.find('\0') != std::string::npos) {
                error = "A selected browser path contains an invalid component.";
                return std::nullopt;
            }
            result /= fs::path(component);
            if (++depth > kBrowserBatchMaxPathDepth) {
                error = "The selected browser directory exceeds the NeoTPC path-depth limit.";
                return std::nullopt;
            }
        }
        if (slash == std::string::npos) break;
        begin = slash + 1;
    }
    if (result.empty() || result.is_absolute() || result.has_root_name()) {
        error = "A selected browser file has an unsafe relative path.";
        return std::nullopt;
    }
    return result.lexically_normal();
}

std::string browserPathKey(const fs::path& path) {
    return neotpc::texture::asciiLower(neotpc::texture::genericPathToUtf8(path.lexically_normal()));
}

std::string browserStemKey(const fs::path& path) {
    return browserPathKey(path.parent_path() / path.stem());
}

std::string formatByteCount(std::uint64_t bytes) {
    constexpr double kMiB = 1024.0 * 1024.0;
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(bytes >= 10u * 1024u * 1024u ? 0 : 1);
    out << (static_cast<double>(bytes) / kMiB) << " MiB";
    return out.str();
}

fs::path collisionPath(const fs::path& initial,
                       const fs::path& source,
                       std::set<std::string>& claimed) {
    if (claimed.insert(browserPathKey(initial)).second) return initial;

    const std::string sourceExtension = normalizeOutputExtension(
        neotpc::texture::extensionLower(source));
    const fs::path parent = initial.parent_path();
    const fs::path outputExtension = initial.extension();
    fs::path collisionStem = initial.stem();
    collisionStem += fs::path("." + sourceExtension).native();

    fs::path candidateName = collisionStem;
    candidateName += outputExtension.native();
    fs::path candidate = parent / candidateName;
    unsigned suffix = 2;
    while (!claimed.insert(browserPathKey(candidate)).second) {
        candidateName = collisionStem;
        candidateName += fs::path("-" + std::to_string(suffix++)).native();
        candidateName += outputExtension.native();
        candidate = parent / candidateName;
    }
    return candidate;
}

struct BrowserBatchItem {
    BrowserRetainedInputFile source;
    std::optional<BrowserRetainedInputFile> sidecar;
    fs::path relativePath;
    fs::path outputRelative;
};

#endif

} // namespace

#if defined(__EMSCRIPTEN__)
struct BrowserBatchState {
    std::uint64_t generation = 0;
    std::vector<BrowserBatchItem> items;
    std::size_t index = 0;
    neotpc::texture::BatchOptions options;
    fs::path outputRoot;
    neotpc::texture::BatchReport report;
    bool cancelRequested = false;
    std::uint32_t activeReadRequest = 0;
    std::vector<std::uint8_t> sourceBytes;
    std::string sidecarTxi;
    std::vector<fs::path> outputs;
    std::size_t publishIndex = 0;
    bool publishInProgress = false;
};
#endif

BatchDialog::BatchDialog(wxWindow* parent, const wxString& initialDirectory, bool darkMode)
    : wxDialog(parent, wxID_ANY, "Batch Convert Textures", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* intro = new wxStaticText(this, wxID_ANY,
        "Convert a folder tree while preserving relative paths. Matching TXI sidecars are read automatically; "
        "TPC output embeds TXI and other image outputs write sidecars.");
    intro->Wrap(FromDIP(700));
    root->Add(intro, 0, wxEXPAND | wxALL, FromDIP(10));

    auto* paths = new wxFlexGridSizer(2, FromDIP(7), FromDIP(10));
    paths->AddGrowableCol(1, 1);
    paths->Add(new wxStaticText(this, wxID_ANY, "Input folder"), 0, wxALIGN_CENTER_VERTICAL);
#if defined(__EMSCRIPTEN__)
    auto* inputRow = new wxPanel(this, wxID_ANY);
    auto* inputSizer = new wxBoxSizer(wxHORIZONTAL);
    inputDirectory_ = new wxTextCtrl(inputRow, wxID_ANY, "No browser folder selected",
                                     wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    inputBrowse_ = new wxButton(inputRow, ID_SELECT_BROWSER_INPUT, "Browse...");
    inputSizer->Add(inputDirectory_, 1, wxEXPAND | wxRIGHT, FromDIP(6));
    inputSizer->Add(inputBrowse_, 0, wxEXPAND);
    inputRow->SetSizer(inputSizer);
    paths->Add(inputRow, 1, wxEXPAND);
#else
    inputDirectory_ = new wxDirPickerCtrl(this, wxID_ANY, initialDirectory, "Choose input folder",
                                          wxDefaultPosition, wxDefaultSize,
                                          wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
    paths->Add(inputDirectory_, 1, wxEXPAND);
#endif
    paths->Add(new wxStaticText(this, wxID_ANY, "Output folder"), 0, wxALIGN_CENTER_VERTICAL);
    wxString output;
#if !defined(__EMSCRIPTEN__)
    output = initialDirectory;
    if (!output.IsEmpty()) {
        output += wxFileName::GetPathSeparator();
        output += "converted";
    }
#endif
    outputDirectory_ = new wxDirPickerCtrl(this, wxID_ANY, output, "Choose output folder",
                                           wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
    paths->Add(outputDirectory_, 1, wxEXPAND);
    paths->Add(new wxStaticText(this, wxID_ANY, "Output format"), 0, wxALIGN_CENTER_VERTICAL);
    format_ = new wxChoice(this, wxID_ANY);
    for (const char* value : {"tpc", "tga", "dds", "png", "jpg", "bmp", "txi"}) format_->Append(value);
    format_->SetSelection(0);
    paths->Add(format_, 1, wxEXPAND);
    root->Add(paths, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto* flags = new wxBoxSizer(wxHORIZONTAL);
    recursive_ = new wxCheckBox(this, wxID_ANY, "Include subfolders");
    recursive_->SetValue(true);
    overwrite_ = new wxCheckBox(this, wxID_ANY, "Overwrite existing outputs");
    flags->Add(recursive_, 0, wxRIGHT, FromDIP(16));
    flags->Add(overwrite_);
    root->Add(flags, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    options_ = new EncodingOptionsPanel(this);
    root->Add(options_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

#if defined(__EMSCRIPTEN__)
    browserProgress_ = new wxGauge(this, wxID_ANY, 1, wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL);
    browserProgress_->Hide();
    root->Add(browserProgress_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
#endif

    report_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(700, 190)),
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    report_->SetHint("The conversion report will appear here.");
    root->Add(report_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    convertButton_ = new wxButton(this, ID_CONVERT, "Convert");
    closeButton_ = new wxButton(this, wxID_CLOSE, "Close");
    buttons->Add(convertButton_, 0, wxRIGHT, FromDIP(8));
    buttons->Add(closeButton_);
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    SetSizer(root);
    wxui::configureResponsiveWindow(*this, wxSize(780, 760), wxSize(560, 420));
    CentreOnParent();
    wxui::constrainWindowToDisplay(*this);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onConvert(); }, ID_CONVERT);
#if defined(__EMSCRIPTEN__)
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestBrowserInputDirectory(); }, ID_SELECT_BROWSER_INPUT);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (browserBatch_) {
            cancelBrowserConversion();
        } else {
            EndModal(wxID_CLOSE);
        }
    }, wxID_CLOSE);
#else
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
#endif
    wxui::applyTheme(this, darkMode);
}

BatchDialog::~BatchDialog() {
#if defined(__EMSCRIPTEN__)
    ++browserSelectionGeneration_;
    if (browserBatch_) {
        if (browserBatch_->activeReadRequest != 0) {
            browser::cancelRetainedFileRead(browserBatch_->activeReadRequest);
        }
        cleanupBrowserOutputs();
        browserBatch_.reset();
    }
    releaseBrowserInput();
    if (outputDirectory_ != nullptr && !outputDirectory_->GetPath().empty()) {
        neoshared::wasm::ReleaseDirectory(outputDirectory_->GetPath());
    }
#endif
}

void BatchDialog::onConvert() {
#if defined(__EMSCRIPTEN__)
    if (browserBatch_) {
        cancelBrowserConversion();
        return;
    }
    startBrowserConversion();
#else
    const auto input = wxpath::fromWx(inputDirectory_->GetPath());
    const auto output = wxpath::fromWx(outputDirectory_->GetPath());
    if (input.empty() || output.empty()) {
        wxMessageBox("Choose both input and output folders.", "Batch conversion",
                     wxOK | wxICON_WARNING, this);
        return;
    }

    neotpc::texture::BatchOptions options;
    options.outputExtension = wxui::toStd(format_->GetStringSelection());
    options.recursive = recursive_->GetValue();
    options.overwrite = overwrite_->GetValue();
    options.saveOptions = options_->options();

    wxProgressDialog progress("NeoTPC batch conversion", "Scanning textures...", 100, this,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_ESTIMATED_TIME |
                              wxPD_REMAINING_TIME | wxPD_AUTO_HIDE);
    try {
        const auto result = neotpc::texture::batchConvertTextures(
            input, output, options,
            [&progress](std::size_t current, std::size_t total, const std::filesystem::path& source) {
                const int percent = total == 0 ? 0 : std::clamp(static_cast<int>((current * 100) / total), 0, 100);
                return progress.Update(percent,
                    wxString::Format("Converting %llu of %llu\n%s",
                                     static_cast<unsigned long long>(current),
                                     static_cast<unsigned long long>(total),
                                     wxpath::toWx(source).c_str()));
            });
        report_->ChangeValue(wxui::toWx(result.summary()));
        if (result.failed != 0) {
            wxMessageBox(wxString::Format("Conversion finished with %llu failure(s). See the report for details.",
                                          static_cast<unsigned long long>(result.failed)),
                         "Batch conversion", wxOK | wxICON_WARNING, this);
        } else if (!result.cancelled) {
            wxMessageBox(wxString::Format("Converted %llu texture(s); skipped %llu.",
                                          static_cast<unsigned long long>(result.converted),
                                          static_cast<unsigned long long>(result.skipped)),
                         "Batch conversion", wxOK | wxICON_INFORMATION, this);
        }
    } catch (const std::exception& error) {
        wxui::showError(this, error);
    }
#endif
}

#if defined(__EMSCRIPTEN__)

void BatchDialog::requestBrowserInputDirectory() {
    if (browserBatch_) return;
    const std::uint64_t request = ++browserSelectionGeneration_;
    inputBrowse_->Disable();
    inputDirectory_->ChangeValue("Waiting for browser folder selection...");
    wxWeakRef<BatchDialog> weak(this);
    neobrowser::requestRetainedDirectory(
        "Choose a texture input folder", browserBatchAccept(),
        [weak, request](neobrowser::RetainedFileSetResult result) mutable {
            if (!weak) {
                if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                return;
            }
            auto* dialog = weak.get();
            if (dialog->browserSelectionGeneration_ != request) {
                if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                return;
            }
            dialog->inputBrowse_->Enable();
            if (!result.error.empty()) {
                if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                dialog->inputDirectory_->ChangeValue(
                    dialog->browserInputSessionId_ == 0 ? "No browser folder selected" :
                    wxui::toWx(dialog->browserInputDisplayName_));
                wxMessageBox(wxui::toWx(result.error), "Input Folder Selection Failed",
                             wxOK | wxICON_ERROR, dialog);
                return;
            }
            if (result.cancelled()) {
                dialog->inputDirectory_->ChangeValue(
                    dialog->browserInputSessionId_ == 0 ? "No browser folder selected" :
                    wxui::toWx(dialog->browserInputDisplayName_));
                return;
            }

            std::vector<BrowserRetainedInputFile> files;
            files.reserve(result.files.size());
            for (auto& file : result.files) {
                files.push_back(BrowserRetainedInputFile{
                    file.fileId, std::move(file.relativePath), file.size});
            }
            dialog->installBrowserInput(
                result.sessionId, std::move(result.displayName), std::move(files));
        });
}

void BatchDialog::installBrowserInput(std::uint32_t sessionId,
                                      std::string displayName,
                                      std::vector<BrowserRetainedInputFile> files) {
    if (sessionId == 0) return;
    if (files.empty()) {
        neobrowser::releaseRetainedFileSet(sessionId);
        wxMessageBox("The selected directory contains no supported texture or TXI files.",
                     "No Textures Found", wxOK | wxICON_INFORMATION, this);
        return;
    }
    if (files.size() > kBrowserBatchMaxRetainedFiles) {
        neobrowser::releaseRetainedFileSet(sessionId);
        wxMessageBox(
            wxString::Format("The selected directory contains %llu supported files. The browser batch limit is %llu.",
                             static_cast<unsigned long long>(files.size()),
                             static_cast<unsigned long long>(kBrowserBatchMaxRetainedFiles)),
            "Batch Selection Too Large", wxOK | wxICON_ERROR, this);
        return;
    }

    std::uint64_t totalBytes = 0;
    std::unordered_set<std::string> paths;
    std::unordered_set<std::uint32_t> fileIds;
    std::vector<std::uint32_t> retainedIds;
    retainedIds.reserve(files.size());
    for (const auto& file : files) {
        if (file.fileId == 0 || !fileIds.insert(file.fileId).second) {
            neobrowser::releaseRetainedFileSet(sessionId);
            wxMessageBox("The selected directory returned an invalid or duplicate browser-file identifier.",
                         "Invalid Batch Input", wxOK | wxICON_ERROR, this);
            return;
        }
        std::string pathError;
        const auto relative = normalizeBrowserRelativePath(file.relativePath, pathError);
        if (!relative) {
            neobrowser::releaseRetainedFileSet(sessionId);
            wxMessageBox(wxui::toWx(pathError), "Unsafe Batch Input",
                         wxOK | wxICON_ERROR, this);
            return;
        }
        const std::string extension = neotpc::texture::extensionLower(*relative);
        const std::uint64_t fileLimit = extension == "txi"
            ? static_cast<std::uint64_t>(kBrowserBatchMaxTxiBytes)
            : static_cast<std::uint64_t>(kBrowserBatchMaxTextureBytes);
        if (file.size > fileLimit) {
            neobrowser::releaseRetainedFileSet(sessionId);
            wxMessageBox(
                wxString::Format("%s exceeds the per-file browser limit.",
                                 wxpath::toWx(relative->filename()).c_str()),
                "Batch Selection Too Large", wxOK | wxICON_ERROR, this);
            return;
        }
        if (file.size > kBrowserBatchMaxAggregateBytes - totalBytes) {
            neobrowser::releaseRetainedFileSet(sessionId);
            wxMessageBox("The selected directory exceeds the 1 GiB browser batch input limit.",
                         "Batch Selection Too Large", wxOK | wxICON_ERROR, this);
            return;
        }
        totalBytes += file.size;
        if (!paths.insert(browserPathKey(*relative)).second) {
            neobrowser::releaseRetainedFileSet(sessionId);
            wxMessageBox("The selected directory contains duplicate case-insensitive paths.",
                         "Ambiguous Batch Input", wxOK | wxICON_ERROR, this);
            return;
        }
        retainedIds.push_back(file.fileId);
    }

    releaseBrowserInput();
    browserInputSessionId_ = sessionId;
    browserInputDisplayName_ = displayName.empty() ? "Selected browser folder" : std::move(displayName);
    browserInputFiles_ = std::move(files);
    neobrowser::retainOnlyRetainedFiles(browserInputSessionId_, retainedIds);
    inputDirectory_->ChangeValue(
        wxString::Format("%s — %llu file(s), %s",
                         wxui::toWx(browserInputDisplayName_).c_str(),
                         static_cast<unsigned long long>(browserInputFiles_.size()),
                         wxui::toWx(formatByteCount(totalBytes)).c_str()));
}

void BatchDialog::releaseBrowserInput() noexcept {
    if (browserInputSessionId_ != 0) {
        neobrowser::releaseRetainedFileSet(browserInputSessionId_);
    }
    browserInputSessionId_ = 0;
    browserInputDisplayName_.clear();
    browserInputFiles_.clear();
}

void BatchDialog::startBrowserConversion() {
    if (browserInputSessionId_ == 0 || browserInputFiles_.empty()) {
        wxMessageBox("Choose an input folder with the browser Browse button first.",
                     "Batch conversion", wxOK | wxICON_WARNING, this);
        return;
    }
    if (!neobrowser::retainedDirectoryWriteSupported()) {
        wxMessageBox(
            "Browser batch conversion requires writable-directory support so generated files can be committed one at a time. "
            "Use a Chromium-based browser with the File System Access API, or convert files individually.",
            "Writable Directory Required", wxOK | wxICON_ERROR, this);
        return;
    }

    const fs::path outputRoot = wxpath::fromWx(outputDirectory_->GetPath());
    if (outputRoot.empty()) {
        wxMessageBox("Choose an output folder.", "Batch conversion",
                     wxOK | wxICON_WARNING, this);
        return;
    }
    neoshared::wasm::RegisterOutputDirectory(outputDirectory_->GetPath());

    const std::string outputExtension = normalizeOutputExtension(
        wxui::toStd(format_->GetStringSelection()));
    if (outputExtension != "tpc" && outputExtension != "tga" &&
        outputExtension != "dds" && outputExtension != "png" &&
        outputExtension != "jpg" && outputExtension != "bmp" &&
        outputExtension != "txi") {
        wxMessageBox("Choose a supported batch output format.", "Batch conversion",
                     wxOK | wxICON_ERROR, this);
        return;
    }

    struct IndexedFile {
        BrowserRetainedInputFile file;
        fs::path relative;
    };
    std::vector<IndexedFile> indexed;
    indexed.reserve(browserInputFiles_.size());
    std::unordered_map<std::string, std::size_t> byPath;
    std::unordered_set<std::string> pixelStems;
    for (const auto& file : browserInputFiles_) {
        std::string pathError;
        const auto relative = normalizeBrowserRelativePath(file.relativePath, pathError);
        if (!relative) {
            wxMessageBox(wxui::toWx(pathError), "Unsafe Batch Input",
                         wxOK | wxICON_ERROR, this);
            return;
        }
        if (!recursive_->GetValue() && !relative->parent_path().empty()) continue;
        const std::string extension = neotpc::texture::extensionLower(*relative);
        if (!isPixelExtension(extension) && extension != "txi") continue;
        const std::size_t index = indexed.size();
        indexed.push_back(IndexedFile{file, *relative});
        byPath.emplace(browserPathKey(*relative), index);
        if (isPixelExtension(extension)) pixelStems.insert(browserStemKey(*relative));
    }

    std::vector<BrowserBatchItem> items;
    std::set<std::string> claimedOutputs;
    std::unordered_set<std::uint32_t> usedIds;
    std::uint64_t workloadBytes = 0;
    for (const auto& entry : indexed) {
        const std::string extension = neotpc::texture::extensionLower(entry.relative);
        if (extension == "txi" && pixelStems.find(browserStemKey(entry.relative)) != pixelStems.end()) {
            continue;
        }

        BrowserBatchItem item;
        item.source = entry.file;
        item.relativePath = entry.relative;
        if (isPixelExtension(extension)) {
            fs::path sidecarPath = entry.relative;
            sidecarPath.replace_extension(".txi");
            const auto sidecar = byPath.find(browserPathKey(sidecarPath));
            if (sidecar != byPath.end()) item.sidecar = indexed[sidecar->second].file;
        }

        item.outputRelative = entry.relative;
        item.outputRelative.replace_extension("." + outputExtension);
        item.outputRelative = collisionPath(item.outputRelative, item.relativePath, claimedOutputs);
        items.push_back(std::move(item));
        if (items.size() > kBrowserBatchMaxItems) {
            wxMessageBox(
                wxString::Format("The batch contains more than %llu convertible items. Split it into smaller folders.",
                                 static_cast<unsigned long long>(kBrowserBatchMaxItems)),
                "Batch Workload Too Large", wxOK | wxICON_ERROR, this);
            return;
        }
    }

    for (const auto& item : items) {
        if (usedIds.insert(item.source.fileId).second) {
            if (item.source.size > kBrowserBatchMaxAggregateBytes - workloadBytes) {
                wxMessageBox("The selected conversion workload exceeds the 1 GiB browser limit.",
                             "Batch Workload Too Large", wxOK | wxICON_ERROR, this);
                return;
            }
            workloadBytes += item.source.size;
        }
        if (item.sidecar && usedIds.insert(item.sidecar->fileId).second) {
            if (item.sidecar->size > kBrowserBatchMaxAggregateBytes - workloadBytes) {
                wxMessageBox("The selected conversion workload exceeds the 1 GiB browser limit.",
                             "Batch Workload Too Large", wxOK | wxICON_ERROR, this);
                return;
            }
            workloadBytes += item.sidecar->size;
        }
    }

    if (items.empty()) {
        wxMessageBox("The selected directory contains no convertible texture files.",
                     "Batch conversion", wxOK | wxICON_INFORMATION, this);
        return;
    }

    auto state = std::make_unique<BrowserBatchState>();
    state->generation = ++browserConversionGeneration_;
    state->items = std::move(items);
    state->options.outputExtension = outputExtension;
    state->options.recursive = recursive_->GetValue();
    state->options.overwrite = overwrite_->GetValue();
    state->options.saveOptions = options_->options();
    state->outputRoot = outputRoot;
    state->report.discovered = state->items.size();
    browserBatch_ = std::move(state);

    browserProgress_->SetRange(static_cast<int>(browserBatch_->items.size()));
    browserProgress_->SetValue(0);
    browserProgress_->Show();
    Layout();
    report_->ChangeValue(
        wxString::Format("Prepared %llu texture(s), %s retained input.\nConversion uses one bounded file at a time.\n",
                         static_cast<unsigned long long>(browserBatch_->items.size()),
                         wxui::toWx(formatByteCount(workloadBytes)).c_str()));
    setBrowserControlsBusy(true);
    continueBrowserConversion();
}

void BatchDialog::continueBrowserConversion() {
    if (!browserBatch_) return;
    if (browserBatch_->cancelRequested || browserBatch_->index >= browserBatch_->items.size()) {
        finishBrowserConversion();
        return;
    }

    const auto& item = browserBatch_->items[browserBatch_->index];
    browserProgress_->SetValue(static_cast<int>(browserBatch_->index));
    report_->AppendText(
        wxString::Format("[%llu/%llu] Reading %s\n",
                         static_cast<unsigned long long>(browserBatch_->index + 1),
                         static_cast<unsigned long long>(browserBatch_->items.size()),
                         wxpath::toWx(item.relativePath).c_str()));

    const std::uint64_t generation = browserBatch_->generation;
    wxWeakRef<BatchDialog> weak(this);
    browserBatch_->activeReadRequest = browser::requestRetainedFileBytes(
        browserInputSessionId_, item.source.fileId, item.source.size,
        kBrowserBatchMaxTextureBytes,
        [weak, generation](browser::RetainedReadResult result) mutable {
            if (!weak) return;
            weak->handleBrowserSourceRead(
                generation, std::move(result.bytes), std::move(result.error));
        });
}

void BatchDialog::handleBrowserSourceRead(std::uint64_t generation,
                                          std::vector<std::uint8_t> bytes,
                                          std::string error) {
    if (!browserBatch_ || browserBatch_->generation != generation) return;
    browserBatch_->activeReadRequest = 0;
    if (browserBatch_->cancelRequested) {
        finishBrowserConversion();
        return;
    }
    if (!error.empty()) {
        completeBrowserBatchItem(false, std::move(error));
        return;
    }
    browserBatch_->sourceBytes = std::move(bytes);

    const auto& item = browserBatch_->items[browserBatch_->index];
    if (!item.sidecar) {
        encodeBrowserBatchItem(generation);
        return;
    }

    wxWeakRef<BatchDialog> weak(this);
    browserBatch_->activeReadRequest = browser::requestRetainedFileBytes(
        browserInputSessionId_, item.sidecar->fileId, item.sidecar->size,
        kBrowserBatchMaxTxiBytes,
        [weak, generation](browser::RetainedReadResult result) mutable {
            if (!weak) return;
            weak->handleBrowserSidecarRead(
                generation, std::move(result.bytes), std::move(result.error));
        });
}

void BatchDialog::handleBrowserSidecarRead(std::uint64_t generation,
                                           std::vector<std::uint8_t> bytes,
                                           std::string error) {
    if (!browserBatch_ || browserBatch_->generation != generation) return;
    browserBatch_->activeReadRequest = 0;
    if (browserBatch_->cancelRequested) {
        finishBrowserConversion();
        return;
    }
    if (!error.empty()) {
        completeBrowserBatchItem(false, std::move(error));
        return;
    }
    browserBatch_->sidecarTxi.assign(bytes.begin(), bytes.end());
    encodeBrowserBatchItem(generation);
}

void BatchDialog::encodeBrowserBatchItem(std::uint64_t generation) {
    if (!browserBatch_ || browserBatch_->generation != generation) return;
    const auto item = browserBatch_->items[browserBatch_->index];
    try {
        neotpc::texture::parser::ScopedResourceLimits limits(
            kBrowserBatchMaxTextureBytes, kBrowserBatchMaxDecodedBytes);
        auto texture = neotpc::texture::loadTextureBytes(
            browserBatch_->sourceBytes, item.relativePath,
            std::move(browserBatch_->sidecarTxi));
        std::vector<std::uint8_t>().swap(browserBatch_->sourceBytes);
        browserBatch_->sidecarTxi.clear();

        if (browserBatch_->options.outputExtension != "txi" && !texture.hasPixels()) {
            throw neotpc::texture::TextureError("TXI-only input has no pixels to encode");
        }

        const fs::path output = browserBatch_->outputRoot / item.outputRelative;
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
        if (ec) throw neotpc::texture::TextureError("Unable to create browser output directory: " + ec.message());

        neotpc::texture::saveTexture(texture, output, browserBatch_->options.saveOptions);
        browserBatch_->outputs.clear();
        browserBatch_->outputs.push_back(output);
        if (outputUsesSidecar(browserBatch_->options.outputExtension) && !texture.txi.empty()) {
            fs::path sidecar = output;
            sidecar.replace_extension(".txi");
            browserBatch_->outputs.push_back(std::move(sidecar));
        }

        std::uintmax_t generatedBytes = 0;
        for (const auto& generated : browserBatch_->outputs) {
            browser::cancelScheduledPublish(generated);
            const std::uintmax_t size = fs::file_size(generated, ec);
            if (ec) throw neotpc::texture::TextureError("Unable to inspect generated browser output: " + ec.message());
            if (size > kBrowserBatchMaxOutputBytes ||
                generatedBytes > kBrowserBatchMaxOutputBytes - size) {
                throw neotpc::texture::TextureError(
                    "Generated output exceeds the 96 MiB per-item browser limit");
            }
            generatedBytes += size;
        }
        browserBatch_->publishIndex = 0;
        publishNextBrowserOutput(generation);
    } catch (const std::exception& error) {
        cleanupBrowserOutputs();
        completeBrowserBatchItem(false, error.what());
    }
}

void BatchDialog::publishNextBrowserOutput(std::uint64_t generation) {
    if (!browserBatch_ || browserBatch_->generation != generation) return;
    if (browserBatch_->cancelRequested) {
        cleanupBrowserOutputs();
        finishBrowserConversion();
        return;
    }
    if (browserBatch_->publishIndex >= browserBatch_->outputs.size()) {
        completeBrowserBatchItem(true, "OK");
        return;
    }

    const fs::path path = browserBatch_->outputs[browserBatch_->publishIndex];
    fs::path relative = path.lexically_relative(browserBatch_->outputRoot);
    if (relative.empty()) relative = path.filename();
    browserBatch_->publishInProgress = true;
    wxWeakRef<BatchDialog> weak(this);
    neobrowser::requestDownloadFile(
        path, neotpc::texture::genericPathToUtf8(relative),
        [weak, generation, path](neobrowser::DownloadResult result) mutable {
            std::error_code ignored;
            fs::remove(path, ignored);
            if (!weak) return;
            auto* dialog = weak.get();
            if (!dialog->browserBatch_ || dialog->browserBatch_->generation != generation) return;
            dialog->browserBatch_->publishInProgress = false;
            if (!result.error.empty() || result.cancelled()) {
                dialog->browserBatch_->cancelRequested = true;
                dialog->completeBrowserBatchItem(
                    false,
                    result.error.empty() ? "Browser output publication was cancelled" : std::move(result.error));
                return;
            }
            if (result.ready()) {
                dialog->browserBatch_->cancelRequested = true;
                dialog->completeBrowserBatchItem(
                    false,
                    "The selected output directory could not be written. A replacement download was prepared and the batch was stopped to avoid accumulating downloads.");
                return;
            }
            ++dialog->browserBatch_->publishIndex;
            dialog->publishNextBrowserOutput(generation);
        });
}

void BatchDialog::completeBrowserBatchItem(bool converted, std::string message) {
    if (!browserBatch_ || browserBatch_->index >= browserBatch_->items.size()) return;
    const auto& item = browserBatch_->items[browserBatch_->index];
    neotpc::texture::BatchItemResult result;
    result.input = item.relativePath;
    result.output = item.outputRelative;
    result.status = converted
        ? neotpc::texture::BatchItemStatus::Converted
        : neotpc::texture::BatchItemStatus::Failed;
    result.message = std::move(message);
    if (converted) {
        ++browserBatch_->report.converted;
        report_->AppendText("  saved\n");
    } else {
        ++browserBatch_->report.failed;
        report_->AppendText(wxString("  failed: ") + wxui::toWx(result.message) + "\n");
    }
    browserBatch_->report.items.push_back(std::move(result));
    cleanupBrowserOutputs();
    browserBatch_->sourceBytes.clear();
    browserBatch_->sidecarTxi.clear();
    ++browserBatch_->index;
    browserProgress_->SetValue(static_cast<int>(browserBatch_->index));

    const std::uint64_t generation = browserBatch_->generation;
    wxWeakRef<BatchDialog> weak(this);
    wxTheApp->CallAfter([weak, generation]() {
        if (!weak || !weak->browserBatch_ || weak->browserBatch_->generation != generation) return;
        weak->continueBrowserConversion();
    });
}

void BatchDialog::finishBrowserConversion() {
    if (!browserBatch_) return;
    if (browserBatch_->publishInProgress) {
        browserBatch_->cancelRequested = true;
        return;
    }
    if (browserBatch_->activeReadRequest != 0) {
        browser::cancelRetainedFileRead(browserBatch_->activeReadRequest);
        browserBatch_->activeReadRequest = 0;
    }
    cleanupBrowserOutputs();
    browserBatch_->report.cancelled = browserBatch_->cancelRequested;
    const auto report = std::move(browserBatch_->report);
    browserBatch_.reset();
    setBrowserControlsBusy(false);
    browserProgress_->Hide();
    Layout();
    report_->ChangeValue(wxui::toWx(report.summary()));

    if (report.cancelled) {
        wxMessageBox("Batch conversion stopped. Completed output files were already committed.",
                     "Batch conversion", wxOK | wxICON_WARNING, this);
    } else if (report.failed != 0) {
        wxMessageBox(
            wxString::Format("Conversion finished with %llu failure(s). See the report for details.",
                             static_cast<unsigned long long>(report.failed)),
            "Batch conversion", wxOK | wxICON_WARNING, this);
    } else {
        wxMessageBox(
            wxString::Format("Converted %llu texture(s).",
                             static_cast<unsigned long long>(report.converted)),
            "Batch conversion", wxOK | wxICON_INFORMATION, this);
    }
}

void BatchDialog::cancelBrowserConversion() {
    if (!browserBatch_) return;
    browserBatch_->cancelRequested = true;
    convertButton_->Disable();
    report_->AppendText("Cancellation requested.\n");
    if (browserBatch_->publishInProgress) return;
    if (browserBatch_->activeReadRequest != 0) {
        browser::cancelRetainedFileRead(browserBatch_->activeReadRequest);
        browserBatch_->activeReadRequest = 0;
    }
    finishBrowserConversion();
}

void BatchDialog::cleanupBrowserOutputs() noexcept {
    if (!browserBatch_) return;
    for (std::size_t index = 0; index < browserBatch_->outputs.size(); ++index) {
        const auto& path = browserBatch_->outputs[index];
        browser::cancelScheduledPublish(path);
        if (browserBatch_->publishInProgress && index == browserBatch_->publishIndex) continue;
        std::error_code ignored;
        fs::remove(path, ignored);
    }
    if (!browserBatch_->publishInProgress) browserBatch_->outputs.clear();
}

void BatchDialog::setBrowserControlsBusy(bool busy) {
    inputBrowse_->Enable(!busy);
    outputDirectory_->Enable(!busy);
    format_->Enable(!busy);
    recursive_->Enable(!busy);
    overwrite_->Enable(!busy);
    options_->Enable(!busy);
    closeButton_->Enable(true);
    convertButton_->Enable(true);
    convertButton_->SetLabel(busy ? "Cancel" : "Convert");
}

#endif

} // namespace neotpc
