#include "BatchDialog.hpp"

#include "BrowserWorkload.hpp"
#include "EncodingOptionsPanel.hpp"
#include "TextureTask.hpp"
#include "PathUtils.hpp"
#include "ResponsiveLayout.hpp"
#include "texture/BatchConverter.hpp"
#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"
#include "texture/ParserLimits.hpp"
#include "NeoWxUi.hpp"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/collpane.h>
#include <wx/listctrl.h>
#include <wx/utils.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filepicker.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>

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
#include <emscripten/emscripten.h>
#endif

namespace fs = std::filesystem;

namespace neotpc {
namespace {

enum : int {
    ID_CONVERT = wxID_HIGHEST + 470,
    ID_SELECT_BROWSER_INPUT, ID_SCAN, ID_INCLUDE_ROWS, ID_EXCLUDE_ROWS, ID_SHOW_OUTPUT, ID_ROW_DETAILS,
};

#if defined(__EMSCRIPTEN__)

constexpr std::size_t kBrowserBatchMaxItems = 256;
constexpr std::size_t kBrowserBatchMaxRetainedFiles = 512;
constexpr std::size_t kBrowserBatchMaxPathDepth = 24;
constexpr std::size_t kBrowserBatchMaxTextureBytes = 64u * 1024u * 1024u;
constexpr std::size_t kBrowserBatchMaxTxiBytes = 1u * 1024u * 1024u;
constexpr std::uint64_t kBrowserBatchMaxAggregateBytes = UINT64_C(1024) * 1024 * 1024;
constexpr std::uint64_t kBrowserBatchMaxDecodedBytes = UINT64_C(64) * 1024 * 1024;

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
    bool encodingInProgress = false;
};
#endif

BatchDialog::BatchDialog(wxWindow* parent, const wxString& initialDirectory, bool darkMode, BatchEditorHooks editor)
    : wxDialog(parent, wxID_ANY, "Batch Convert Textures", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), editor_(std::move(editor)) {
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* form = new layout::ScrolledPage(this);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* intro = new layout::WrappedLabel(form, wxID_ANY,
        "Convert a folder tree while preserving relative paths. Matching TXI sidecars are read automatically; "
        "TPC output embeds TXI and other image outputs write sidecars.");
    root->Add(intro, 0, wxEXPAND | wxALL, FromDIP(10));

    auto* paths = new wxBoxSizer(wxVERTICAL);
    auto addPathField = [form, paths](const wxString& caption, wxWindow* control) {
        control->SetMinSize(form->FromDIP(wxSize(180, -1)));
        paths->Add(layout::fieldRow(form, caption, control), 0, wxEXPAND);
    };
#if defined(__EMSCRIPTEN__)
    auto* inputRow = new wxPanel(form, wxID_ANY);
    auto* inputSizer = new wxWrapSizer(wxHORIZONTAL);
    inputDirectory_ = new wxTextCtrl(inputRow, wxID_ANY, "No browser folder selected",
                                     wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    inputBrowse_ = new wxButton(inputRow, ID_SELECT_BROWSER_INPUT, "Browse...");
    inputDirectory_->SetMinSize(FromDIP(wxSize(180, -1)));
    inputSizer->Add(inputDirectory_, 1, wxEXPAND | wxRIGHT | wxBOTTOM, FromDIP(6));
    inputSizer->Add(inputBrowse_, 0, wxEXPAND);
    inputRow->SetSizer(inputSizer);
    addPathField("Input folder", inputRow);
#else
    inputDirectory_ = new wxDirPickerCtrl(form, wxID_ANY, initialDirectory, "Choose input folder",
                                          wxDefaultPosition, wxDefaultSize,
                                          wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
    addPathField("Input folder", inputDirectory_);
#endif
    wxString output;
#if !defined(__EMSCRIPTEN__)
    output = initialDirectory;
    if (!output.IsEmpty()) {
        output += wxFileName::GetPathSeparator();
        output += "converted";
    }
#endif
    outputDirectory_ = new wxDirPickerCtrl(form, wxID_ANY, output, "Choose output folder",
                                           wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
    addPathField("Output folder", outputDirectory_);
    format_ = new wxChoice(form, wxID_ANY);
    for (const char* value : {"tpc", "tga", "dds", "png", "jpg", "bmp", "txi"}) format_->Append(value);
    format_->SetSelection(0);
    addPathField("Output format", format_);
    inputType_ = new wxChoice(form, wxID_ANY);
    for(const char* value:{"All supported","tpc","txb","tga","dds","png","jpg","bmp","txi"}) inputType_->Append(value);
    inputType_->SetSelection(0);
    addPathField("Input type", inputType_);
    matchingFormat_ = new wxChoice(form, wxID_ANY);
    matchingFormat_->Append("Preserve original file (no recompression)");
    matchingFormat_->Append("Re-encode using selected settings");
    matchingFormat_->SetSelection(0);
    matchingFormat_->SetToolTip("Preserve copies a matching container/dialect exactly, including its mip levels and TXI. Encoding controls then apply only to format/dialect conversions. Choose Re-encode to apply them to all selected inputs.");
    matchingFormat_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { invalidatePlan(); });
    addPathField("Matching inputs", matchingFormat_);
    root->Add(paths, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    auto* policyNote = new layout::WrappedLabel(form, wxID_ANY,
        "Preserve copies matching containers/dialects exactly. Encoding controls apply only to conversions; choose Re-encode to rebuild matching files.");
    root->Add(policyNote, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto* flags = new wxBoxSizer(wxVERTICAL);
    recursive_ = new layout::WrappedCheckBox(form, "Include subfolders");
    recursive_->SetValue(true);
    overwrite_ = new layout::WrappedCheckBox(form, "Overwrite existing outputs");
    flags->Add(recursive_, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    flags->Add(overwrite_, 0, wxEXPAND);
#if defined(__EMSCRIPTEN__)
    overwrite_->SetValue(false);overwrite_->Disable();
    overwrite_->SetLabel("Browser: new files in an empty output folder only");
#endif
    root->Add(flags, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    options_ = new EncodingOptionsPanel(form);
    format_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        invalidatePlan();
        options_->setTarget(neotpc::texture::kindForExtension(std::filesystem::path("output." + wxui::toStd(format_->GetStringSelection()))));
    });
    options_->setChangeHandler([this]{invalidatePlan();});
    root->Add(options_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    inputType_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){invalidatePlan();});
    recursive_->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){invalidatePlan();});
    overwrite_->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent&){invalidatePlan();});
    outputDirectory_->Bind(wxEVT_DIRPICKER_CHANGED,[this](wxFileDirPickerEvent&){if(!updatingOutput_)outputChosen_=true;invalidatePlan();});
    if(auto* text=outputDirectory_->GetTextCtrl())text->Bind(wxEVT_TEXT,[this](wxCommandEvent& e){if(!updatingOutput_)outputChosen_=true;invalidatePlan();e.Skip();});
#ifndef __EMSCRIPTEN__
    auto inputChanged=[this] {
        if(!outputChosen_) {
            updatingOutput_=true;
            outputDirectory_->SetPath(wxpath::toWx(wxpath::fromWx(inputDirectory_->GetPath()) / "converted"));
            updatingOutput_=false;
        }
        invalidatePlan();
    };
    inputDirectory_->Bind(wxEVT_DIRPICKER_CHANGED,[inputChanged](wxFileDirPickerEvent&){inputChanged();});
    if(auto* text=inputDirectory_->GetTextCtrl())text->Bind(wxEVT_TEXT,[inputChanged](wxCommandEvent& e){inputChanged();e.Skip();});
#endif

    form->SetSizer(root); form->Layout();
    outer->Add(form,1,wxEXPAND);root=outer;

#if defined(__EMSCRIPTEN__)
    browserProgress_ = new wxGauge(this, wxID_ANY, 1, wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL);
    browserProgress_->Hide();
    root->Add(browserProgress_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
#endif

    items_=new wxListCtrl(this,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(720,200)),wxLC_REPORT);
    wxui::setColumns(*items_,{{"Include",65},{"Source",230},{"Output",230},{"Status",200}});
    items_->SetName("Reviewed batch destinations");
    items_->SetMinSize(FromDIP(wxSize(1, 160)));
    root->Add(items_,1,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(10));
    auto* selection=new wxWrapSizer(wxHORIZONTAL);
    selection->Add(new wxButton(this,ID_INCLUDE_ROWS,"Include selected"),0,wxRIGHT | wxBOTTOM,FromDIP(6));
    selection->Add(new wxButton(this,ID_EXCLUDE_ROWS,"Exclude selected"),0,wxRIGHT | wxBOTTOM,FromDIP(6));
    selection->Add(new wxButton(this, ID_ROW_DETAILS, "Row details..."), 0, wxRIGHT | wxBOTTOM, FromDIP(6));
    auto* openOutput=new wxButton(this,ID_SHOW_OUTPUT,"Open output folder");
    selection->Add(openOutput,0,wxBOTTOM,FromDIP(6));root->Add(selection,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(10));
#if defined(__EMSCRIPTEN__)
    openOutput->Hide();
#else
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const auto out = wxpath::fromWx(outputDirectory_->GetPath());
        std::error_code ec;
        if (!fs::is_directory(out, ec)) {
            wxMessageBox("The output folder does not exist yet. Scan only reviews destinations; conversion creates the folder.",
                "Output folder", wxOK | wxICON_INFORMATION, this);
        } else if (!wxLaunchDefaultApplication(wxpath::toWx(out))) {
            wxMessageBox(wxString("Could not open the output folder:\n\n") + wxpath::toWx(out),
                "Output folder", wxOK | wxICON_WARNING, this);
        }
    }, ID_SHOW_OUTPUT);
#endif
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { showSelectedDetails(); }, ID_ROW_DETAILS);
    items_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { showSelectedDetails(); });
    items_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { refreshSelectionActions(); });
    items_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { refreshSelectionActions(); });
    items_->SetToolTip("Select rows to include or exclude them. Double-click a row for full paths and the complete status message.");
    Bind(wxEVT_BUTTON,[this](wxCommandEvent&){setSelectedRows(true);},ID_INCLUDE_ROWS);
    Bind(wxEVT_BUTTON,[this](wxCommandEvent&){setSelectedRows(false);},ID_EXCLUDE_ROWS);
    resultSummary_ = new layout::WrappedLabel(this, wxID_ANY,
        "Scan to review destinations. Nothing is written until Convert.");
    root->Add(resultSummary_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    auto* details=new wxCollapsiblePane(this,wxID_ANY,"Technical details",wxDefaultPosition,wxDefaultSize,wxCP_DEFAULT_STYLE|wxCP_NO_TLW_RESIZE);
    auto* detailSizer=new wxBoxSizer(wxVERTICAL);
    report_ = new wxTextCtrl(details->GetPane(), wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(640, 100)),
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
    report_->SetMinSize(FromDIP(wxSize(1, 100)));
    detailSizer->Add(report_,1,wxEXPAND);details->GetPane()->SetSizer(detailSizer);
    root->Add(details,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(10));
    details->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,[this](wxCollapsiblePaneEvent&){Layout();});

    auto* buttons = new wxWrapSizer(wxHORIZONTAL);
    scanButton_ = new wxButton(this, ID_SCAN, "Scan / Review");
    buttons->Add(scanButton_,0,wxRIGHT | wxBOTTOM,FromDIP(8));
    convertButton_ = new wxButton(this, ID_CONVERT, "Convert 0 ready files");
    convertButton_->Disable();
    Bind(wxEVT_BUTTON,[this](wxCommandEvent&){onScan();},ID_SCAN);
    closeButton_ = new wxButton(this, wxID_CLOSE, "Close");
    buttons->Add(convertButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(8));
    buttons->Add(closeButton_, 0, wxBOTTOM, FromDIP(8));
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    SetSizer(root);
    SetEscapeId(wxID_CLOSE);
    refreshSelectionActions();
    wxui::configureResponsiveWindow(*this, wxSize(780, 760), wxSize(560, 420));
    CentreOnParent();
    wxui::constrainWindowToDisplay(*this);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onConvert(); }, ID_CONVERT);
#if defined(__EMSCRIPTEN__)
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestBrowserInputDirectory(); }, ID_SELECT_BROWSER_INPUT);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if(nativeBusy_)return;
        if (browserBatch_) {
            cancelBrowserConversion();
        } else {
            EndModal(wxID_CLOSE);
        }
    }, wxID_CLOSE);
    Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& event){
        if(nativeBusy_ || browserBatch_){if(browserBatch_)cancelBrowserConversion();if(event.CanVeto())event.Veto();return;}
        event.Skip();
    });
#else
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if(!nativeBusy_) EndModal(wxID_CLOSE); }, wxID_CLOSE);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        if(nativeBusy_) { if(event.CanVeto()) event.Veto(); return; }
        event.Skip();
    });
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

bool BatchDialog::acceptsInput(const fs::path& input) const {
    if(inputType_->GetSelection()==0)return true;
    auto ext=texture::extensionLower(input);if(ext=="jpeg"||ext=="jpe")ext="jpg";
    return ext==wxui::toStd(inputType_->GetStringSelection());
}
void BatchDialog::invalidatePlan() {
    if(nativeBusy_)return;
#if defined(__EMSCRIPTEN__)
    if(browserBatch_)return;
    browserPlan_.reset(); browserExcludedInputs_.clear();
#endif
    plan_.reset();planReady_=false;
    // A reviewed plan belongs to exactly one set of settings. Never show stale
    // destinations as though they still describe the next conversion.
    displayedRows_.clear(); included_.clear();
    if (items_) items_->DeleteAllItems();
    if (report_) report_->ChangeValue(wxEmptyString);
    refreshSelectionActions();
    if(convertButton_) {convertButton_->SetLabel("Convert 0 ready files");convertButton_->Disable();}
    if(resultSummary_) {
        resultSummary_->SetLabel("Settings changed. Scan to review destinations before converting.");
        Layout();
    }
}
void BatchDialog::showRows(const std::vector<texture::BatchItemResult>& rows,bool selectReady) {
    displayedRows_=rows;included_.clear();items_->DeleteAllItems();
    for(const auto& row:rows) {
        const bool include=selectReady && row.status==texture::BatchItemStatus::Ready && acceptsInput(row.input);
        included_.push_back(include);
        const char* state=row.status==texture::BatchItemStatus::Ready?"Ready":row.status==texture::BatchItemStatus::Conflict?"Conflict":row.status==texture::BatchItemStatus::Converted?"Created":row.status==texture::BatchItemStatus::Failed?"Failed":"Skipped";
#if defined(__EMSCRIPTEN__)
        if (row.status == texture::BatchItemStatus::Ready) state = "Pending input check";
#endif
        wxui::appendRow(*items_,{include?"Yes":"No",texture::pathToUtf8(row.input),texture::pathToUtf8(row.output),std::string(state)+(row.message.empty()?"":" — "+row.message)});
    }
    refreshPlanCount();
    refreshSelectionActions();
}
void BatchDialog::refreshPlanCount() {
    const auto count=static_cast<unsigned long long>(std::count(included_.begin(),included_.end(),true));
    convertButton_->SetLabel(wxString::Format("Convert %llu ready files",count));
    convertButton_->Enable(planReady_ && count>0);
    resultSummary_->SetLabel(wxString::Format("%llu selected; %llu scanned. Conflicts are held; excluded rows are not converted.",count,static_cast<unsigned long long>(displayedRows_.size())));
    Layout();
}
void BatchDialog::refreshSelectionActions() {
    const bool selected = items_ && items_->GetSelectedItemCount() > 0;
    bool reviewing = planReady_;
#if defined(__EMSCRIPTEN__)
    reviewing = reviewing && !browserBatch_;
#endif
    for (int id : {ID_INCLUDE_ROWS, ID_EXCLUDE_ROWS})
        if (auto* button = FindWindow(id)) button->Enable(reviewing && selected);
    if (auto* button = FindWindow(ID_ROW_DETAILS)) button->Enable(selected);
}

void BatchDialog::showSelectedDetails() {
    if (nativeBusy_) return;
#if defined(__EMSCRIPTEN__)
    if (browserBatch_) return;
#endif
    const long index = items_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (index < 0 || static_cast<std::size_t>(index) >= displayedRows_.size()) return;
    const auto& row = displayedRows_[static_cast<std::size_t>(index)];
    const std::string message = "Source:\n" + texture::pathToUtf8(row.input) +
        "\n\nOutput:\n" + texture::pathToUtf8(row.output) + "\n\n" +
        wxui::toStd(items_->GetItemText(index, 3));
    wxMessageBox(wxui::toWx(message), "Batch row details", wxOK | wxICON_INFORMATION, this);
}

void BatchDialog::setSelectedRows(bool include) {
    if (nativeBusy_ || !planReady_) return;
#if defined(__EMSCRIPTEN__)
    if (browserBatch_) return;
    auto excluded = browserExcludedInputs_;
#else
    if (!plan_) return;
    auto excluded = plan_->excludedInputs;
#endif
    std::vector<fs::path> selected;
    for (long row = items_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); row != -1;
         row = items_->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) {
        if (static_cast<std::size_t>(row) >= displayedRows_.size()) continue;
        const auto input = displayedRows_[row].input;
        selected.push_back(input);
        // Held rows can be excluded, releasing their destinations (not their sources).
        if (include) {
            if (!acceptsInput(input)) continue;
            excluded.erase(std::remove(excluded.begin(), excluded.end(), input), excluded.end());
        } else if (std::find(excluded.begin(), excluded.end(), input) == excluded.end()) excluded.push_back(input);
    }
    if (selected.empty()) return;
#if defined(__EMSCRIPTEN__)
    browserExcludedInputs_ = std::move(excluded);
    browserPlan_.reset(); planReady_ = false; convertButton_->Disable();
    startBrowserConversion(true);
#else
    try {
        TextureBusyGuard busy(nativeBusy_);
        const auto old = *plan_;
        auto revised = runTextureTask(this, "Review selected destinations", [&](TextureTaskProgress&) {
            return texture::replanTextureBatch(old, excluded);
        });
        if (revised.items.size() != old.items.size()) throw texture::TextureError("The input folder changed. Scan it again.");
        for (std::size_t i = 0; i < old.items.size(); ++i)
            if (old.items[i].input != revised.items[i].input) throw texture::TextureError("The input folder changed. Scan it again.");
        plan_ = std::move(revised); report_->ChangeValue(wxui::toWx(plan_->summary())); showRows(plan_->items, true);
    } catch (const texture::OperationCancelled&) { invalidatePlan(); resultSummary_->SetLabel("Review cancelled; scan again before converting."); }
      catch (const std::exception& error) { invalidatePlan(); wxui::showError(this, error); }
#endif
    for (std::size_t i = 0; i < displayedRows_.size(); ++i)
        if (std::find(selected.begin(), selected.end(), displayedRows_[i].input) != selected.end())
            items_->SetItemState(static_cast<long>(i), wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
}
void BatchDialog::onScan() {
    if(nativeBusy_)return;
    invalidatePlan();
#if defined(__EMSCRIPTEN__)
    if(browserBatch_)return;
    startBrowserConversion(true);
#else
    const auto input=wxpath::fromWx(inputDirectory_->GetPath()), output=wxpath::fromWx(outputDirectory_->GetPath());
    if(input.empty()||output.empty()){wxMessageBox("Choose input and output folders.","Batch",wxOK|wxICON_INFORMATION,this);return;}
    texture::BatchOptions options;options.outputExtension=wxui::toStd(format_->GetStringSelection());options.recursive=recursive_->GetValue();options.overwrite=overwrite_->GetValue();options.saveOptions=options_->options();
    if (inputType_->GetSelection() != 0) options.inputExtension = wxui::toStd(inputType_->GetStringSelection());
    options.preserveMatchingFormat = matchingFormat_->GetSelection() == 0;
    try {
        TextureBusyGuard busy(nativeBusy_);
        plan_=runTextureTask(this,"Scan batch destinations",[&](TextureTaskProgress&){return texture::planTextureBatch(input,output,options);});
        planReady_=true;report_->ChangeValue(wxui::toWx(plan_->summary()));showRows(plan_->items,true);
    } catch(const texture::OperationCancelled&){invalidatePlan();resultSummary_->SetLabel("Scan cancelled; nothing was written.");}
      catch(const std::exception& e){invalidatePlan();wxui::showError(this,e);}
#endif
}

void BatchDialog::onConvert() {
    if(nativeBusy_)return;
#if defined(__EMSCRIPTEN__)
    if (browserBatch_) {
        cancelBrowserConversion();
        return;
    }
    startBrowserConversion();
#else
    if(!planReady_ || !plan_) {onScan();return;}
    auto selected=*plan_;
    std::string confirmation;
    if (selected.conflicts()) {
        confirmation += "Conflicting rows remain excluded. Convert only the selected, independently safe rows.\n\n";
        selected.options.skipConflicts = true;
    }
    if (selected.options.overwrite)
        confirmation += "Selected outputs and their TXI companions may be replaced. Other batch input dependencies remain protected.\n\n";
    const auto alphaLoss = std::count_if(selected.items.begin(), selected.items.end(), [](const auto& row) {
        return row.status == texture::BatchItemStatus::Ready && row.discardsAlpha;
    });
    if (alphaLoss) confirmation += std::to_string(alphaLoss) +
        " selected JPEG output(s) will discard alpha, including any transparency or mask data.\n\n";
    try {
        if (editor_.prepare) confirmation += editor_.prepare(selected);
    } catch (const std::exception& error) { wxui::showError(this, error); return; }
    if (!confirmation.empty() && !wxui::confirm(this, "Confirm batch conversion", confirmation + "Continue?")) return;
    try {
        TextureBusyGuard busy(nativeBusy_);
        const auto result=runTextureTask(this,"Convert textures",[&](TextureTaskProgress& state) {
            return texture::executeTextureBatch(selected,[&](std::size_t done,std::size_t total,const fs::path& input) {
                state.set(done,total,std::to_string(done)+" / "+std::to_string(total)+" complete\n"+texture::pathToUtf8(input));return !state.cancelled.load();
            });
        });
        // Even a cancelled/partly failed batch can contain committed outputs.
        // Refresh their owner before presenting results; never refresh failures.
        if (editor_.completed) {
            try { editor_.completed(this, result); }
            catch (const std::exception& error) {
                wxMessageBox(wxString("The batch finished, but the open texture could not be refreshed. Its in-memory edits remain available.\n\n") +
                    wxui::toWx(error.what()), "Open texture refresh", wxOK | wxICON_WARNING, this);
            }
        }
        report_->ChangeValue(wxui::toWx(result.summary()));
        auto completed=selected.items;
        for(auto& row:completed) {auto found=std::find_if(result.items.begin(),result.items.end(),[&](const auto& item){return item.input==row.input;});
            if(found!=result.items.end())row=*found;else{row.status=texture::BatchItemStatus::Skipped;row.message="Not run (cancelled)";}}
        showRows(completed,false);
        resultSummary_->SetLabel(wxString::Format("%llu created, %llu skipped, %llu failed, %llu held%s",
            static_cast<unsigned long long>(result.converted),static_cast<unsigned long long>(result.skipped),static_cast<unsigned long long>(result.failed),static_cast<unsigned long long>(result.conflicts),wxString(result.cancelled?" — cancelled":"").c_str()));
        planReady_=false;convertButton_->Disable();refreshSelectionActions();
    } catch(const texture::OperationCancelled&) {invalidatePlan();resultSummary_->SetLabel("Cancelled. Completed outputs remain; the unfinished pair was not committed.");}
      catch(const std::exception& error){invalidatePlan();wxui::showError(this,error);}

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

    invalidatePlan();
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

void BatchDialog::startBrowserConversion(bool reviewOnly) {
    if(!reviewOnly && browserPlan_ && planReady_) {
        auto state=std::make_unique<BrowserBatchState>(*browserPlan_);
        std::vector<BrowserBatchItem> ready;
        for (const auto& item : state->items) {
            auto found = std::find_if(displayedRows_.begin(), displayedRows_.end(), [&](const auto& row) { return row.input == item.relativePath; });
            if (found != displayedRows_.end() && found->status == texture::BatchItemStatus::Ready) ready.push_back(item);
            else {
                const auto row = found == displayedRows_.end() ? texture::BatchItemResult{item.relativePath, item.outputRelative, texture::BatchItemStatus::Skipped, "Excluded in reviewed plan"} : *found;
                state->report.items.push_back(row);
                if (row.status == texture::BatchItemStatus::Conflict) ++state->report.conflicts; else ++state->report.skipped;
            }
        }
        state->items = std::move(ready);
        if (state->report.conflicts && !wxui::confirm(this, "Conflicts held", "Convert only the selected, independently safe rows? Conflicts remain excluded.")) {
            return;
        }
        if(state->items.empty()){invalidatePlan();return;}
        // Browser Scan is index-only. Warn once conservatively before reading
        // any candidate, not once per file after encoding has already begun.
        if (state->options.outputExtension == "jpg" &&
            !wxui::confirm(this, "JPEG output discards alpha",
                "Any converted input with transparency or mask data will lose its alpha channel. Continue with these JPEG outputs?")) return;
        try {TextureBusyGuard busy(nativeBusy_);runTextureTask(this,"Check browser output folder",[&](TextureTaskProgress&){browser::checkEmptyBatchOutput(state->outputRoot);});}
        catch(const std::exception& e){invalidatePlan();wxui::showError(this,e);return;}
        browserPlan_.reset(); browserBatch_=std::move(state);planReady_=false;
        browserProgress_->SetRange(static_cast<int>(browserBatch_->items.size()));browserProgress_->SetValue(0);browserProgress_->Show();
        setBrowserControlsBusy(true);continueBrowserConversion();return;
    }
    if(!reviewOnly){onScan();return;}
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
        if(!byPath.emplace(browserPathKey(*relative), index).second) {
            wxMessageBox("Multiple input paths differ only by case. Select a folder without those ambiguous image/TXI names.","Ambiguous browser inputs",wxOK|wxICON_WARNING,this);return;
        }
        if (isPixelExtension(extension)) pixelStems.insert(browserStemKey(*relative));
    }

    std::vector<BrowserBatchItem> items;

    std::unordered_set<std::uint32_t> usedIds;
    std::uint64_t workloadBytes = 0;
    for (const auto& entry : indexed) {
        const std::string extension = neotpc::texture::extensionLower(entry.relative);
        if (inputType_->GetStringSelection() != "txi" && extension == "txi" && pixelStems.find(browserStemKey(entry.relative)) != pixelStems.end()) {
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

        items.push_back(std::move(item));
        if (items.size() > kBrowserBatchMaxItems) {
            wxMessageBox(
                wxString::Format("The batch contains more than %llu convertible items. Split it into smaller folders.",
                                 static_cast<unsigned long long>(kBrowserBatchMaxItems)),
                "Batch Workload Too Large", wxOK | wxICON_ERROR, this);
            return;
        }
    }

    texture::BatchOptions selectionOptions;
    selectionOptions.outputExtension = outputExtension;
    selectionOptions.saveOptions = options_->options();
    selectionOptions.preserveMatchingFormat = matchingFormat_->GetSelection() == 0;
    if (inputType_->GetSelection() != 0) selectionOptions.inputExtension = wxui::toStd(inputType_->GetStringSelection());
    std::unordered_map<std::string, std::vector<std::size_t>> claims;
    std::vector<texture::BatchItemResult> rows;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const bool selected = texture::batchInputRequested(item.relativePath, selectionOptions, browserExcludedInputs_);
        rows.push_back({item.relativePath, item.outputRelative,
            selected ? texture::BatchItemStatus::Ready : texture::BatchItemStatus::Skipped,
            selected ? (outputExtension == "jpg" ? "Destination clear; JPEG discards any alpha; input checks pending" :
                "Destination clear; input/encoding checks occur before publication") : "Not selected; source remains retained"});
        if (!selected) continue;
        claims[browserPathKey(item.outputRelative)].push_back(i);
        if (outputUsesSidecar(outputExtension)) {
            auto txi = item.outputRelative; txi.replace_extension(".txi"); claims[browserPathKey(txi)].push_back(i);
        }
    }
    for (const auto& claim : claims) if (claim.second.size() > 1) for (auto i : claim.second) {
        rows[i].status = texture::BatchItemStatus::Conflict;
        rows[i].message = "Selected outputs collide; exclude a contender to resolve";
    }
    // The selected host folder must genuinely be empty. Virtual MEMFS existence
    // does not tell us whether a host output would overwrite an unread input.
    try {
        TextureBusyGuard busy(nativeBusy_);
        runTextureTask(this,"Check browser output folder",[&](TextureTaskProgress&){browser::checkEmptyBatchOutput(outputRoot);});
    } catch(const std::exception& error){wxui::showError(this,error);return;}

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
    state->options.overwrite = false;
    state->options.saveOptions = selectionOptions.saveOptions;
    state->options.inputExtension = selectionOptions.inputExtension;
    state->options.preserveMatchingFormat = selectionOptions.preserveMatchingFormat;
    state->outputRoot = outputRoot;
    state->report.discovered = state->items.size();
    browserPlan_ = std::move(state); planReady_ = true; showRows(rows, true);
    report_->ChangeValue(wxui::toWx("Retained input: " + formatByteCount(workloadBytes) +
        ". Destinations are checked; input/encoding validation runs per file before publication.\n"));

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
    if(!browserBatch_ || browserBatch_->generation!=generation)return;
    browserBatch_->encodingInProgress=true;
    const auto item=browserBatch_->items[browserBatch_->index];
    try {
        texture::parser::ScopedResourceLimits limits(kBrowserBatchMaxTextureBytes,kBrowserBatchMaxDecodedBytes);
        auto last=std::chrono::steady_clock::now();
        texture::OperationScope checkpoint([&]{
            if(std::chrono::steady_clock::now()-last>std::chrono::milliseconds(45)) {
                emscripten_sleep(1);last=std::chrono::steady_clock::now();
            }
            if(!browserBatch_ || browserBatch_->cancelRequested)throw texture::OperationCancelled();
        });
        std::optional<std::vector<std::uint8_t>> sidecar;
        if (item.sidecar) sidecar.emplace(browserBatch_->sidecarTxi.begin(), browserBatch_->sidecarTxi.end());
        auto result=texture::encodeBatchTexture(browserBatch_->sourceBytes,sidecar,item.relativePath,item.outputRelative,browserBatch_->options);
        std::vector<std::uint8_t>().swap(browserBatch_->sourceBytes);browserBatch_->sidecarTxi.clear();
        auto encoded=std::move(result.encoded);
        texture::checkOperation();
        browserBatch_->encodingInProgress=false;browserBatch_->publishInProgress=true;
        // Publication is a non-cancellable new-file pair commit. On error the
        // bridge removes newly created members; no automatic download fallback.
        browser::publishNewBatchOutput(browserBatch_->outputRoot,item.outputRelative,encoded);
        browserBatch_->publishInProgress=false;
        completeBrowserBatchItem(true, result.preserved ? "Copied original bytes (including existing mip levels/TXI)" :
            result.discardedAlpha ? "Created JPEG; alpha channel discarded (including mask data)" : "Created image/TXI pair (encoded)");
    } catch(const texture::OperationCancelled&) {
        if(browserBatch_){browserBatch_->encodingInProgress=false;browserBatch_->publishInProgress=false;browserBatch_->cancelRequested=true;finishBrowserConversion();}
    } catch(const std::exception& error) {
        if(browserBatch_){browserBatch_->encodingInProgress=false;browserBatch_->publishInProgress=false;completeBrowserBatchItem(false,error.what());}
    }
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
    if (browserBatch_->publishInProgress || browserBatch_->encodingInProgress) {
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

    showRows(report.items,false);planReady_=false;convertButton_->Disable();refreshSelectionActions();
    resultSummary_->SetLabel(wxString::Format("%llu created, %llu skipped, %llu failed, %llu held",
        static_cast<unsigned long long>(report.converted),static_cast<unsigned long long>(report.skipped),static_cast<unsigned long long>(report.failed),static_cast<unsigned long long>(report.conflicts)));
    if (report.cancelled) {
        resultSummary_->SetLabel(resultSummary_->unwrappedText()+" — cancelled");
        wxMessageBox("Batch conversion stopped. Completed output files were already committed.",
                     "Batch conversion", wxOK | wxICON_WARNING, this);
    } else if (report.failed != 0) {
        wxMessageBox(
            wxString::Format("Conversion finished with %llu failure(s). See the report for details.",
                             static_cast<unsigned long long>(report.failed)),
            "Batch conversion", wxOK | wxICON_WARNING, this);

    }
}

void BatchDialog::cancelBrowserConversion() {
    if (!browserBatch_) return;
    browserBatch_->cancelRequested = true;
    convertButton_->Disable();
    report_->AppendText("Cancellation requested.\n");
    if (browserBatch_->publishInProgress || browserBatch_->encodingInProgress) return;
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
    overwrite_->Enable(false);
    options_->Enable(!busy);
    closeButton_->Enable(true);
    scanButton_->Enable(!busy);
    inputType_->Enable(!busy);
    matchingFormat_->Enable(!busy);
    items_->Enable(!busy);
    convertButton_->Enable(busy);
    convertButton_->SetLabel(busy ? "Cancel" : "Scan again to convert");
    refreshSelectionActions();
}

#endif

} // namespace neotpc
