#include "TextureEditorPanel.hpp"
#include "BatchDialog.hpp"
#if defined(__EMSCRIPTEN__)
#include "BrowserWorkload.hpp"
#endif
#include "EncodingOptionsPanel.hpp"
#include "PathUtils.hpp"
#include "ResponsiveLayout.hpp"
#include "TextureCanvas.hpp"
#include "TextureTask.hpp"
#include "TxiEditor.hpp"
#include "TxiDictionaryPanel.hpp"
#include "TxiDiagnosticsPanel.hpp"
#include "core/TextureDocument.hpp"
#include "core/TextureWorkflow.hpp"
#include "core/TxiWorkflow.hpp"
#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"
#include "texture/ParserLimits.hpp"
#include "texture/Txi.hpp"
#include "NeoWxUi.hpp"
#include "NeoViewState.hpp"
#include "NeoGameDirectoryMenu.hpp"

#include <wx/aboutdlg.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/checkbox.h>
#include <wx/collpane.h>
#include <wx/spinctrl.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/progdlg.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/statbox.h>
#include <wx/tglbtn.h>
#include <wx/timer.h>
#include <wx/wx.h>
#include <wx/wrapsizer.h>

#include "tpc_icon.xpm"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#ifndef __EMSCRIPTEN__
#include <future>
#include <thread>
#endif
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include "NeoBrowserFiles.hpp"
#include <neoshared/wasm_dialog_compat.h>
#include <wx/weakref.h>
#endif

namespace fs = std::filesystem;

namespace neotpc {
namespace {

constexpr const char* kAppName = "NeoTPC";

wxIcon makeAppIcon() {
    wxIcon icon;
    icon.CopyFromBitmap(wxBitmap(tpc_icon));
    return icon;
}

enum : int {
    ID_SAVE_AS = wxID_HIGHEST + 710,
    ID_CLOSE_TEXTURE,
    ID_SPLIT_TPC,
    ID_COMBINE_TGA_TXI,
    ID_IMPORT_TXI,
    ID_EXPORT_TXI,
    ID_BATCH_CONVERT,
    ID_OPEN_CONFLICTS,
    ID_CLOSE_CONFLICTS,
    ID_RECENT_FIRST,
    ID_RECENT_LAST = ID_RECENT_FIRST + 9,
    ID_CLEAR_RECENT,
    ID_FIT_IMAGE,
    ID_ACTUAL_SIZE,
    ID_ZOOM_IN,
    ID_ZOOM_OUT,
    ID_DARK_MODE,
    ID_SET_ALPHA,
    ID_SCALE_ALPHA,
    ID_INVERT_ALPHA,
    ID_FLIP_HORIZONTAL,
    ID_FLIP_VERTICAL,
    ID_PLAY,
    ID_ANIMATION_TIMER,
    ID_ENCODE_PREVIEW, ID_EXPORT_IMAGE, ID_APPLY_ENCODING, ID_COMMON_TXI,
    ID_PREVIOUS_FRAME, ID_NEXT_FRAME, ID_COMPARE_IMAGES, ID_CHOOSE_EXPORT,
    ID_FONT_INCREASE, ID_FONT_DECREASE, ID_FONT_RESET,
    ID_TXI_DICTIONARY,
};

const char* openWildcard() {
    return "Texture files (*.tpc;*.txb;*.tga;*.dds;*.png;*.jpg;*.jpeg;*.jpe;*.bmp;*.txi)|"
           "*.tpc;*.txb;*.tga;*.dds;*.png;*.jpg;*.jpeg;*.jpe;*.bmp;*.txi|"
           "Odyssey textures (*.tpc;*.txb;*.tga;*.dds;*.txi)|*.tpc;*.txb;*.tga;*.dds;*.txi|"
           "All files (*.*)|*.*";
}

wxImage makePreviewImage(const neotpc::texture::TextureLayer& layer, int mode) {
    wxImage image(static_cast<int>(layer.width), static_cast<int>(layer.height), false);
    unsigned char* output = image.GetData();
    if (output == nullptr) return {};
    constexpr int checkerSize = 8;
    for (std::uint32_t y = 0; y < layer.height; ++y) {
        for (std::uint32_t x = 0; x < layer.width; ++x) {
            const auto sourceOffset = (static_cast<std::size_t>(y) * layer.width + x) * 4;
            const auto outputOffset = (static_cast<std::size_t>(y) * layer.width + x) * 3;
            const unsigned alpha = layer.rgba[sourceOffset + 3];
            if (mode == 1) {
                output[outputOffset] = layer.rgba[sourceOffset];
                output[outputOffset + 1] = layer.rgba[sourceOffset + 1];
                output[outputOffset + 2] = layer.rgba[sourceOffset + 2];
            } else if (mode == 2) {
                output[outputOffset] = static_cast<unsigned char>(alpha);
                output[outputOffset + 1] = static_cast<unsigned char>(alpha);
                output[outputOffset + 2] = static_cast<unsigned char>(alpha);
            } else if (mode == 3) {
                output[outputOffset] = static_cast<unsigned char>(alpha);
                output[outputOffset + 1] = 0;
                output[outputOffset + 2] = 0;
            } else {
                const unsigned checker = (((x / checkerSize) + (y / checkerSize)) & 1u) ? 192u : 142u;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const unsigned source = layer.rgba[sourceOffset + channel];
                    output[outputOffset + channel] = static_cast<unsigned char>(
                        (source * alpha + checker * (255u - alpha) + 127u) / 255u);
                }
            }
        }
    }
    return image;
}

const neotpc::texture::TextureLayer* previewLayerAt(const neotpc::texture::TextureData& texture,
                                                   std::size_t layerIndex,
                                                   std::size_t mipIndex) {
    if (!texture.hasPixels()) return nullptr;
    if(layerIndex >= texture.layers.size()) return nullptr;
    const auto& layer = texture.layers[layerIndex];
    if(mipIndex > layer.mipmaps.size()) return nullptr;
    return mipIndex == 0 ? &layer : &layer.mipmaps[mipIndex - 1];
}

std::uint64_t decodedTextureBytes(const neotpc::texture::TextureData& texture) {
    std::uint64_t total = 0;
    auto add = [&](std::size_t bytes) {
        const auto value = static_cast<std::uint64_t>(bytes);
        if (value > std::numeric_limits<std::uint64_t>::max() - total) {
            total = std::numeric_limits<std::uint64_t>::max();
        } else {
            total += value;
        }
    };
    for (const auto& layer : texture.layers) {
        add(layer.rgba.size());
        for (const auto& mip : layer.mipmaps) add(mip.rgba.size());
    }
    return total;
}

struct DisplayKey {
    const texture::TextureLayer* layer = nullptr;
    std::uint64_t pixels = 0;
    int mode = 0;
    bool operator==(const DisplayKey& rhs) const {
        return layer == rhs.layer && pixels == rhs.pixels && mode == rhs.mode;
    }
};
struct DisplayImage {
    DisplayKey key;
    wxImage image;
    std::size_t bytes = 0;
};

struct ComparisonImage {
    fs::path path;
    neotpc::texture::TextureData texture;
    TextureCanvas* canvas = nullptr;
    wxStaticText* label = nullptr;
    bool encodedPreview = false;
};


#if defined(__EMSCRIPTEN__)

constexpr std::size_t kBrowserComparisonMaxImages = 16;
constexpr std::size_t kBrowserComparisonMaxSelectedFiles = 64;
constexpr std::size_t kBrowserComparisonMaxPathDepth = 16;
constexpr std::size_t kBrowserComparisonMaxTextureBytes = 64u * 1024u * 1024u;
constexpr std::size_t kBrowserComparisonMaxTxiBytes = 1u * 1024u * 1024u;
constexpr std::uint64_t kBrowserComparisonMaxAggregateInputBytes = UINT64_C(192) * 1024 * 1024;
constexpr std::uint64_t kBrowserComparisonMaxDecodedBytes = UINT64_C(192) * 1024 * 1024;

const char* browserComparisonAccept() {
    return ".tpc,.txb,.tga,.dds,.png,.jpg,.jpeg,.jpe,.bmp,.txi";
}

bool isBrowserComparisonPixelExtension(const std::string& extension) {
    return extension == "tpc" || extension == "txb" || extension == "tga" ||
           extension == "dds" || extension == "png" || extension == "jpg" ||
           extension == "jpeg" || extension == "jpe" || extension == "bmp";
}

std::optional<fs::path> normalizeBrowserComparisonPath(const std::string& value,
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
            if (++depth > kBrowserComparisonMaxPathDepth) {
                error = "A selected browser path exceeds the comparison depth limit.";
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

std::string browserComparisonPathKey(const fs::path& path) {
    return neotpc::texture::asciiLower(
        neotpc::texture::genericPathToUtf8(path.lexically_normal()));
}

struct BrowserComparisonCandidate {
    neobrowser::RetainedFileInfo source;
    std::optional<neobrowser::RetainedFileInfo> sidecar;
    fs::path relativePath;
};

struct BrowserComparisonState {
    std::uint64_t generation = 0;
    std::uint32_t sessionId = 0;
    std::vector<BrowserComparisonCandidate> candidates;
    std::size_t index = 0;
    std::uint32_t activeReadRequest = 0;
    std::vector<std::uint8_t> sourceBytes;
    std::string sidecarTxi;
    std::uint64_t decodedBytes = 0;
    std::vector<ComparisonImage> loaded;
    std::vector<std::string> failures;
    std::unique_ptr<wxProgressDialog> progress;
};

#endif

class TpcEncodingDialog final : public wxDialog {
public:
    TpcEncodingDialog(wxWindow* parent,
                      const neotpc::texture::TextureSaveOptions& initialOptions)
        : wxDialog(parent, wxID_ANY, "TPC encoding options", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* content = new layout::ScrolledPage(this);
        auto* fields = new wxBoxSizer(wxVERTICAL);
        optionsPanel_ = new EncodingOptionsPanel(content);
        optionsPanel_->setOptions(initialOptions);
        fields->Add(optionsPanel_, 0, wxEXPAND | wxALL, FromDIP(10));
        content->SetSizer(fields);
        root->Add(content, 1, wxEXPAND);
        root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        SetSizerAndFit(root);
        wxui::configureResponsiveWindow(*this, wxSize(520, 620), wxSize(400, 360));
        CentreOnParent();
        wxui::constrainWindowToDisplay(*this);
    }

    neotpc::texture::TextureSaveOptions options() const {
        return optionsPanel_->options();
    }

private:
    EncodingOptionsPanel* optionsPanel_ = nullptr;
};

class TextureEditorPanelImpl;

class TextureDropTarget final : public wxFileDropTarget {
public:
    explicit TextureDropTarget(TextureEditorPanelImpl* owner) : owner_(owner) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override;
private:
    TextureEditorPanelImpl* owner_ = nullptr;
};

class TextureEditorPanelImpl final : public neotpc::ui::TextureEditorPanel {
public:
    TextureEditorPanelImpl(wxWindow* parent, neomodules::Context context = {})
        : TextureEditorPanel(parent, std::move(context)),
          settings_(kAppName), animationTimer_(this, ID_ANIMATION_TIMER), darkMode_(settings_.darkMode()) {
        buildMenus();
        buildInterface();
        SetDropTarget(new TextureDropTarget(this));
        Bind(wxEVT_TIMER, &TextureEditorPanelImpl::onAnimationTimer, this, ID_ANIMATION_TIMER);
        wxui::applyTheme(this, darkMode_);
        txiEditor_->applyTheme(darkMode_);
        applyTxiHintTheme();
        canvas_->setDarkMode(darkMode_);
        fontScale_ = settings_.fontScale();
        applyUiScale();
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) { CallAfter([this] { applyUiScale(); }); e.Skip(); });
        updateWindowState();
        setModuleStatusText( "Ready - open or drop a texture", 0);
    }

    ~TextureEditorPanelImpl() override {
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, false);
#endif
    }

    bool openFile(const fs::path& path) override { return openPath(path); }

    bool activateResource(const std::string& identity) override {
        return !identity.empty() && document_.isOpen() && resourceIdentity_ == identity;
    }

    bool openResource(neoshared::ResourceDocument resource) override {
        if (resource.identity.empty() || resource.fileName.empty()) return false;
        if (activateResource(resource.identity)) return true;
        if (busy_ || !maybeSave()) return false;
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, true);
#endif
        try {
            setAnimationPlaying(false);
            TextureBusyGuard busy(busy_);
            TextureDocument opened;
            opened.openMemory(std::move(resource.bytes), fs::path(resource.fileName));
            document_ = std::move(opened);
            resourceIdentity_ = std::move(resource.identity);
            resourceProtectedInputs_ = std::move(resource.protectedInputs);
            resourceSourceDescription_ = std::move(resource.sourceDescription);
            staged_.reset();
            comparisonImages_.clear();
            rebuildComparisonWorkspace();
            exportPreferences_.clear();
            refreshDocumentUi(true);
            applyToPreviewCanvases([](auto& c) { c.fitImage(); });
            setModuleStatusText("Opened archive resource snapshot", 0);
            return true;
        } catch (const texture::OperationCancelled&) {
            setModuleStatusText("Open cancelled; previous document retained", 0);
            return false;
        } catch (const std::exception& error) {
            wxui::showError(this, error);
            return false;
        }
    }

    bool saveActiveAs(const fs::path& path) override {
        if (busy_ || !document_.isOpen() || path.empty()) return false;
        try {
            validateOutputPath(path);
            setAnimationPlaying(false);
            TextureBusyGuard busy(busy_);
            runTextureTask(this, "Save copy", [&](TextureTaskProgress&) { document_.saveAs(path); });
            resourceIdentity_.clear();
            resourceProtectedInputs_.clear();
            resourceSourceDescription_.clear();
            staged_.reset();
            comparisonImages_.clear();
            resetExportDestination();
            rebuildComparisonWorkspace();
            refreshDocumentUi();
            settings_.addRecentFile(path);
            refreshRecentFiles();
            setModuleStatusText(wxString("Saved as ") + wxpath::toWx(path), 0);
            return true;
        } catch (const texture::OperationCancelled&) {
            setModuleStatusText("Save As cancelled", 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
        return false;
    }

    std::size_t documentCount() const override { return document_.isOpen() ? 1u : 0u; }
    TextureDocument* activeDocumentModel() override { return document_.isOpen() ? &document_ : nullptr; }
    std::vector<fs::path> openPaths() const override {
        if (document_.isOpen() && document_.sourceBacked() && !document_.path().empty()) return {document_.path()};
        return {};
    }
    bool canClose() override {
        if (busy_) return false;
        if (!maybeSave()) return false;
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, true);
#endif
        return true;
    }
    void setAppearance(bool dark, double fontScale) override {
        darkMode_ = dark;
        fontScale_ = fontScale;
        wxui::applyTheme(this, darkMode_);
        txiEditor_->applyTheme(darkMode_);
        applyTxiHintTheme();
        applyToPreviewCanvases([this](auto& canvas) { canvas.setDarkMode(darkMode_); });
        applyUiScale();
        if (darkModeItem_) darkModeItem_->Check(darkMode_);
    }

    bool openPath(const fs::path& path) {
        if (busy_ || path.empty() || !maybeSave()) return false;
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, true);
#endif
        try {
            setAnimationPlaying(false);
            TextureBusyGuard busy(busy_);
            auto opened=runTextureTask(this,"Open texture",[&](TextureTaskProgress&) {
                TextureDocument result; result.open(path); return result;
            });
            document_=std::move(opened);
            resourceIdentity_.clear(); resourceProtectedInputs_.clear(); resourceSourceDescription_.clear();
            staged_.reset();
            comparisonImages_.clear();
            rebuildComparisonWorkspace();
            exportPreferences_.clear();
            refreshDocumentUi(true);
            applyToPreviewCanvases([](auto& c) { c.fitImage(); });
            settings_.addRecentFile(path);
            refreshRecentFiles();
            setModuleStatusText( wxString("Opened ") + wxpath::toWx(path), 0);
            return true;
        } catch (const texture::OperationCancelled&) {
            setModuleStatusText("Open cancelled; previous document retained",0); return false;
        } catch (const std::exception& error) {
            wxui::showError(this, error);
            return false;
        }
    }

    bool openDropped(const wxArrayString& names) {
        if(busy_ || names.empty()) return false;
        std::vector<fs::path> files;
        for(const auto& name:names) {
            const auto path=wxpath::fromWx(name);
            std::error_code ec;
            if(fs::is_directory(path,ec)) {
                if(names.size()!=1) {
                    wxMessageBox("Drop one folder for batch conversion, or only image files for comparison.","Choose one operation",wxOK|wxICON_INFORMATION,this);return false;
                }
                showBatchAt(name); return true;
            }
            files.push_back(path);
        }
        if(files.size()==1) return openPath(files.front());
        if(!wxui::confirm(this,"Compare dropped images?","Open the first file for editing and the other files as read-only comparisons? Use a folder drop for batch conversion.")) return false;
        if(!openPath(files.front())) return false;
#ifndef __EMSCRIPTEN__
        openConflictingImagesFromPaths(std::move(files));
#else
        // This path concerns already-imported virtual files, not retained folder handles.
        try {
            TextureBusyGuard busy(busy_);
            auto images=runTextureTask(this,"Open comparisons",[&](TextureTaskProgress&) {
                std::vector<ComparisonImage> result;
                texture::parser::ScopedResourceLimits limits(64u*1024*1024,128u*1024*1024);
                std::uint64_t bytes=0;
                for(std::size_t i=1;i<files.size();++i) {
                    if(i>=16) throw texture::TextureError("At most 16 dropped comparison images are supported in the browser.");
                    auto image=texture::loadTexture(files[i]);bytes+=decodedTextureBytes(image);
                    if(bytes>128u*1024*1024) throw texture::TextureError("Comparison images exceed the browser memory limit.");
                    result.push_back(ComparisonImage{files[i],std::move(image),nullptr,nullptr});
                }
                return result;
            });
            comparisonImages_=std::move(images);rebuildComparisonWorkspace();refreshPreview();
        } catch(const std::exception& error){wxui::showError(this,error);return false;}
#endif
        return true;
    }

private:
    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;

    void buildMenus() {
        auto* menuBar = new wxMenuBar();
        auto* file = new wxMenu();
        file->Append(wxID_OPEN, context_.embedded ? "&Open..." : "&Open...\tCtrl+O");
        recentFilesMenu_ = new wxMenu();
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->AppendSeparator();
        file->Append(wxID_SAVE, "&Save\tCtrl+S");
        file->Append(ID_SAVE_AS, "Save &As (preserve format)...\tCtrl+Shift+S");
        file->Append(ID_EXPORT_IMAGE, "Export / Convert...\tCtrl+E");
        file->AppendSeparator();
        file->Append(ID_COMPARE_IMAGES, "&Compare images...\tCtrl+Shift+O");
        file->Append(ID_OPEN_CONFLICTS, "Find same-name variants");
        file->Append(ID_CLOSE_CONFLICTS, "Close Image Comparison");
        file->AppendSeparator();
        file->Append(ID_SPLIT_TPC, "Split TPC into TGA + TXI...");
        file->Append(ID_COMBINE_TGA_TXI, "Combine TGA + TXI into TPC...");
        file->Append(ID_BATCH_CONVERT, "&Batch Convert...");
        file->AppendSeparator();
        file->Append(ID_CLOSE_TEXTURE, "&Close Texture\tCtrl+W");
        if (!context_.embedded) {
            gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
                *this, *file, [this](const std::filesystem::path& directory) {
                    chooseOpen(directory);
                });
            file->AppendSeparator();
            file->Append(wxID_EXIT, "E&xit\tAlt+F4");
        }
        menuBar->Append(file, "&File");
        auto* edit=new wxMenu();
        undoMenuItem_=edit->Append(wxID_UNDO,"Undo texture change\tCtrl+Z");
        redoMenuItem_=edit->Append(wxID_REDO,"Redo texture change\tCtrl+Y");
        edit->AppendSeparator();
        edit->Append(ID_APPLY_ENCODING,"Change current file encoding...");
        menuBar->Append(edit,"&Edit");

        auto* view = new wxMenu();
        view->Append(ID_FIT_IMAGE, context_.embedded ? "&Fit to Window" : "&Fit to Window\tCtrl+0");
        view->Append(ID_ACTUAL_SIZE, context_.embedded ? "&Actual Pixels (100%)" : "&Actual Pixels (100%)\tCtrl+1");
        view->Append(ID_ZOOM_IN, context_.embedded ? "Zoom &In" : "Zoom &In\tCtrl++");
        view->Append(ID_ZOOM_OUT, context_.embedded ? "Zoom &Out" : "Zoom &Out\tCtrl+-");
        if (!context_.embedded) {
            view->AppendSeparator();
            view->Append(ID_FONT_INCREASE, "Increase UI font size\tCtrl+Shift+]");
            view->Append(ID_FONT_DECREASE, "Decrease UI font size\tCtrl+Shift+[");
            view->Append(ID_FONT_RESET, "Reset UI font size\tCtrl+Shift+0");
            view->AppendSeparator();
            darkModeItem_=view->AppendCheckItem(ID_DARK_MODE, "&Dark Mode");
            darkModeItem_->Check(darkMode_);
        }
        menuBar->Append(view, "&View");

        auto* tools = new wxMenu();
        tools->Append(ID_SET_ALPHA, "Set Alpha...");
        tools->Append(ID_SCALE_ALPHA, "Scale Alpha...");
        tools->Append(ID_INVERT_ALPHA, "Invert Alpha");
        tools->AppendSeparator();
        tools->Append(ID_FLIP_HORIZONTAL, "Flip Pixels Horizontally");
        tools->Append(ID_FLIP_VERTICAL, "Flip Pixels Vertically");
        menuBar->Append(tools, "&Tools");

        auto* help = new wxMenu();
        help->Append(ID_TXI_DICTIONARY, "TXI &Reference");
        help->AppendSeparator();
        help->Append(wxID_ABOUT, "&About NeoTPC");
        menuBar->Append(help, "&Help");
        setModuleMenus(menuBar);

        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showReferencePage(); }, ID_TXI_DICTIONARY);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { chooseOpen(); }, wxID_OPEN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { save(); }, wxID_SAVE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { saveAs(); }, ID_SAVE_AS);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showExportPage(); }, ID_EXPORT_IMAGE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { undoRedo(false); }, wxID_UNDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { undoRedo(true); }, wxID_REDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { splitTpc(); }, ID_SPLIT_TPC);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { combineTgaTxi(); }, ID_COMBINE_TGA_TXI);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showBatch(); }, ID_BATCH_CONVERT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { openConflictingImages(); }, ID_OPEN_CONFLICTS);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { compareImages(); }, ID_COMPARE_IMAGES);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyEncoding(); }, ID_APPLY_ENCODING);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { changeUiScale(1); }, ID_FONT_INCREASE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { changeUiScale(-1); }, ID_FONT_DECREASE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { fontScale_=1.0; settings_.setFontScale(fontScale_); applyUiScale(); }, ID_FONT_RESET);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { closeImageComparison(); }, ID_CLOSE_CONFLICTS);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { closeTexture(); }, ID_CLOSE_TEXTURE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestModuleClose(); }, wxID_EXIT);
        Bind(wxEVT_MENU, &TextureEditorPanelImpl::onOpenRecent, this, ID_RECENT_FIRST, ID_RECENT_LAST);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            settings_.clearRecentFiles();
            refreshRecentFiles();
        }, ID_CLEAR_RECENT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.fitImage(); }); refreshStatus(); }, ID_FIT_IMAGE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.actualSize(); }); refreshStatus(); }, ID_ACTUAL_SIZE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.zoomIn(); }); refreshStatus(); }, ID_ZOOM_IN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.zoomOut(); }); refreshStatus(); }, ID_ZOOM_OUT);
        Bind(wxEVT_MENU, &TextureEditorPanelImpl::toggleDarkMode, this, ID_DARK_MODE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setAlpha(); }, ID_SET_ALPHA);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { scaleAlpha(); }, ID_SCALE_ALPHA);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { invertAlpha(); }, ID_INVERT_ALPHA);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { flipHorizontal(); }, ID_FLIP_HORIZONTAL);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { flipVertical(); }, ID_FLIP_VERTICAL);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showAbout(); }, wxID_ABOUT);
        refreshRecentFiles();
    }

    void refreshRecentFiles() {
        if (recentFilesMenu_ != nullptr) {
            neosettings::populateRecentFilesMenu(*recentFilesMenu_, settings_, ID_RECENT_FIRST, ID_CLEAR_RECENT);
        }
    }

    void onOpenRecent(wxCommandEvent& event) {
        const auto files = settings_.recentFiles();
        const auto index = static_cast<std::size_t>(event.GetId() - ID_RECENT_FIRST);
        if (index < files.size()) openPath(files[index]);
    }

    void buildInterface() {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                              wxSP_LIVE_UPDATE | wxSP_3D);
        splitter->SetMinimumPaneSize(FromDIP(300));
        splitter->SetSashGravity(1.0);
        previewHost_ = new wxPanel(splitter);
        previewHostSizer_ = new wxBoxSizer(wxVERTICAL);
        previewHost_->SetSizer(previewHostSizer_);
        notebook_ = new wxNotebook(splitter, wxID_ANY);
        notebook_->SetMinSize(FromDIP(wxSize(300, 120)));
        notebook_->SetName("Texture inspector");
        splitter->SetName("Viewport / inspector splitter");
        buildTexturePage();
        buildTxiPage();
        buildExportPage();
        buildCatalogPage();
        notebook_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& event) {
            if (auto* page = notebook_->GetCurrentPage()) page->Layout();
            event.Skip();
        });
        rebuildComparisonWorkspace();
        splitter->SplitVertically(previewHost_, notebook_, -FromDIP(380));
        root->Add(splitter, 1, wxEXPAND);
        SetSizer(root);

        createModuleStatusBar(2);
        const int widths[2] = {-1, FromDIP(280)};
        moduleStatusBar()->SetStatusWidths(2, widths);
    }

    const neotpc::texture::TextureData* comparisonTexture(std::size_t paneIndex) const {
        if (paneIndex == 0) return document_.isOpen() ? &document_.texture() : nullptr;
        const auto comparisonIndex = paneIndex - 1;
        if(comparisonIndex >= comparisonImages_.size()) return nullptr;
        const auto& image=comparisonImages_[comparisonIndex];
        return image.encodedPreview ? (staged_ ? &staged_->decoded : nullptr) : &image.texture;
    }

    fs::path comparisonPath(std::size_t paneIndex) const {
        if (paneIndex == 0) return document_.isOpen() ? document_.path() : fs::path{};
        const auto comparisonIndex = paneIndex - 1;
        return comparisonIndex < comparisonImages_.size() ? comparisonImages_[comparisonIndex].path : fs::path{};
    }

    wxPanel* createComparisonPane(wxWindow* parent, std::size_t paneIndex) {
        auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* label = new wxStaticText(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        auto* canvas = new TextureCanvas(panel);
        canvas->SetMinSize(FromDIP(wxSize(150, 120)));
        canvas->setDarkMode(darkMode_);
        canvas->setSmooth(samplingChoice_ && samplingChoice_->GetSelection()==1);
        canvas->setGrid(gridChoice_ ? static_cast<TextureCanvas::Grid>(gridChoice_->GetSelection()) : TextureCanvas::Grid::None);
        canvas->setViewHandler([this,canvas](const TextureCanvas::View& view) {
            if(syncingView_ || busy_) return;
            syncingView_=true;
            applyToPreviewCanvases([&](TextureCanvas& other){if(&other!=canvas)other.setView(view);});
            syncingView_=false;refreshStatus();
        });
        canvas->setPixelHandler([this,paneIndex](int x,int y){
            if(busy_)return;const auto* image=comparisonTexture(paneIndex);if(!image)return;
            const auto* layer=previewLayerAt(*image,mappedLayer(*image,static_cast<std::size_t>(std::max(0,layerChoice_->GetSelection()))),static_cast<std::size_t>(std::max(0,mipChoice_->GetSelection())));
            if(!layer || x<0 || y<0 || static_cast<unsigned>(x)>=layer->width || static_cast<unsigned>(y)>=layer->height)return;
            const auto offset=(static_cast<std::size_t>(y)*layer->width+static_cast<unsigned>(x))*4;
            const auto& rgba=layer->rgba;
            setModuleStatusText(wxString::Format("Pixel %d, %d | RGBA %u, %u, %u, %u",x,y,static_cast<unsigned>(rgba[offset]),static_cast<unsigned>(rgba[offset+1]),static_cast<unsigned>(rgba[offset+2]),static_cast<unsigned>(rgba[offset+3])),0);
        });
        sizer->Add(label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(6));
        sizer->Add(canvas, 1, wxEXPAND);
        panel->SetSizer(sizer);

        if (paneIndex == 0) {
            canvas_ = canvas;
            sourceLabel_ = label;
        } else {
            auto& comparison = comparisonImages_[paneIndex - 1];
            comparison.canvas = canvas;
            comparison.label = label;
        }

        refreshComparisonLabel(paneIndex, label);
        canvas->clearImage("Open a texture (Ctrl+O)\nDrop one file to edit, several to compare,\nor a folder to batch convert.");
        return panel;
    }

    void refreshComparisonLabel(std::size_t paneIndex, wxStaticText* label) {
        if (!label) return;
        const auto path = comparisonPath(paneIndex);
        const auto* image = comparisonTexture(paneIndex);
        wxString title = paneIndex == 0 ? "Current document | " :
            (comparisonImages_[paneIndex - 1].encodedPreview ? "Encoded preview (not saved) | " : "Read-only comparison | ");
        title += path.empty() ? wxString("No texture open") : wxpath::toWx(path.filename());
        if (image) {
            title += " | " + wxui::toWx(texture::textureFileKindToString(image->kind));
            if (image->hasPixels()) title += wxString::Format(" | %u x %u | %llu layer(s)",
                image->layers.front().width, image->layers.front().height,
                static_cast<unsigned long long>(image->layers.size()));
            else title += " | metadata only";
        }
        label->SetLabel(title);
        if (!path.empty()) label->SetToolTip(wxpath::toWx(path));
        label->SetMinSize(wxSize(1, -1));
    }

    wxWindow* buildComparisonTree(wxWindow* parent,
                                  std::size_t first,
                                  std::size_t last,
                                  bool splitIntoColumns) {
        if (last - first == 1) return createComparisonPane(parent, first);
        auto* splitter = new wxSplitterWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                              wxSP_LIVE_UPDATE | wxSP_3D);
        splitter->SetMinimumPaneSize(FromDIP(90));
        splitter->SetSashGravity(0.5);
        const std::size_t middle = first + (last - first + 1) / 2;
        auto* leading = buildComparisonTree(splitter, first, middle, !splitIntoColumns);
        auto* trailing = buildComparisonTree(splitter, middle, last, !splitIntoColumns);
        if (splitIntoColumns) splitter->SplitVertically(leading, trailing);
        else splitter->SplitHorizontally(leading, trailing);
        comparisonSplitters_.emplace_back(splitter, splitIntoColumns);
        return splitter;
    }

    void balanceComparisonSplitters() {
        for (const auto& item : comparisonSplitters_) {
            auto* splitter = item.first;
            if (splitter == nullptr || !splitter->IsSplit()) continue;
            const auto size = splitter->GetClientSize();
            const int span = item.second ? size.GetWidth() : size.GetHeight();
            if (span > FromDIP(180)) splitter->SetSashPosition(span / 2);
        }
    }

    void rebuildComparisonWorkspace() {
        clearDisplayCache();
        refreshExportSummary();
        if (previewHostSizer_ == nullptr) return;
        const auto retainedView=canvas_ ? canvas_->view() : TextureCanvas::View{};
        canvas_ = nullptr;
        sourceLabel_ = nullptr;
        for (auto& comparison : comparisonImages_) {
            comparison.canvas = nullptr;
            comparison.label = nullptr;
        }
        comparisonSplitters_.clear();
        previewHostSizer_->Clear(true);
        const std::size_t paneCount = 1 + comparisonImages_.size();
        auto* tree = buildComparisonTree(previewHost_, 0, paneCount, true);
        previewHostSizer_->Add(tree, 1, wxEXPAND);
        previewHost_->Layout();
        wxui::applyTheme(previewHost_, darkMode_);
        applyToPreviewCanvases([&](auto& canvas){canvas.setView(retainedView);});
        if(layerChoice_) refreshLayerChoices();
        CallAfter([this]() { balanceComparisonSplitters(); });
    }

    void applyToPreviewCanvases(const std::function<void(TextureCanvas&)>& action) {
        if (canvas_ != nullptr) action(*canvas_);
        for (auto& comparison : comparisonImages_) {
            if (comparison.canvas != nullptr) action(*comparison.canvas);
        }
    }

    void buildTexturePage() {
        auto* page = new layout::ScrolledPage(notebook_);
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* previewBox = new layout::GroupSizer(page, "Preview");
        auto* previewFields = new wxBoxSizer(wxVERTICAL);
        previewFields->Add(new wxStaticText(page, wxID_ANY, "Layer / frame"),
                           0, wxBOTTOM, FromDIP(4));
        auto* layerRow = new wxWrapSizer(wxHORIZONTAL);
        layerChoice_ = new wxChoice(page, wxID_ANY);
        layerChoice_->SetMinSize(FromDIP(wxSize(100, -1)));
        layerChoice_->SetName("Preview layer or animation frame");
        playButton_ = new wxToggleButton(page, ID_PLAY, "Play");
        playButton_->SetToolTip("Play or pause the loaded animation; this does not change TXI");
        // Keep transport buttons together; move them below the selector if needed.
        auto* transport = new wxBoxSizer(wxHORIZONTAL);
        transport->Add(new wxButton(page, ID_PREVIOUS_FRAME, "<", wxDefaultPosition,
                                    FromDIP(wxSize(30, -1))), 0, wxRIGHT, FromDIP(3));
        transport->Add(playButton_);
        transport->Add(new wxButton(page, ID_NEXT_FRAME, ">", wxDefaultPosition,
                                    FromDIP(wxSize(30, -1))), 0, wxLEFT, FromDIP(3));
        layerRow->Add(layerChoice_, 1, wxRIGHT | wxBOTTOM, FromDIP(6));
        layerRow->Add(transport, 0, wxBOTTOM, FromDIP(6));
        previewFields->Add(layerRow, 0, wxEXPAND);

        auto addPreviewField = [page, previewFields](const wxString& label, wxChoice* choice) {
            choice->SetMinSize(page->FromDIP(wxSize(150, -1)));
            previewFields->Add(layout::fieldRow(page, label, choice), 0, wxEXPAND);
        };
        mipChoice_ = new wxChoice(page, wxID_ANY);
        addPreviewField("Mipmap", mipChoice_);
        previewMode_ = new wxChoice(page, wxID_ANY);
        for (const char* label : {"RGBA over checkerboard", "RGB (opaque)", "Alpha (grayscale)", "Alpha (red mask)"}) {
            previewMode_->Append(label);
        }
        previewMode_->SetSelection(0);
        addPreviewField("Channel view", previewMode_);
        samplingChoice_ = new wxChoice(page, wxID_ANY);
        samplingChoice_->Append("Nearest (pixel inspection)");
        samplingChoice_->Append("Smooth (display only)");
        samplingChoice_->SetSelection(0);
        addPreviewField("Display filter", samplingChoice_);
        gridChoice_ = new wxChoice(page, wxID_ANY);
        gridChoice_->Append("None"); gridChoice_->Append("Pixels");
        gridChoice_->Append("4 x 4 compression blocks"); gridChoice_->SetSelection(0);
        addPreviewField("Grid", gridChoice_);
        samplingChoice_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){applyToPreviewCanvases([this](auto& c){c.setSmooth(samplingChoice_->GetSelection()==1);});});
        gridChoice_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){applyToPreviewCanvases([this](auto& c){c.setGrid(static_cast<TextureCanvas::Grid>(gridChoice_->GetSelection()));});});
        previewBox->Add(previewFields, 0, wxEXPAND | wxALL, FromDIP(8));
        root->Add(previewBox, 0, wxEXPAND | wxALL, FromDIP(8));

        imageInfo_ = new layout::WrappedLabel(page, wxID_ANY, "Open a texture to view its dimensions and format.");
        root->Insert(0, imageInfo_, 0, wxEXPAND | wxALL, FromDIP(8));
        auto* details = new wxCollapsiblePane(page, wxID_ANY, "Image details", wxDefaultPosition, wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
        auto* detailSizer = new wxBoxSizer(wxVERTICAL);
        summary_ = new wxTextCtrl(details->GetPane(), wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(-1, 140)),
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
        summary_->SetMinSize(FromDIP(wxSize(1, 140)));
        detailSizer->Add(summary_, 1, wxEXPAND); details->GetPane()->SetSizer(detailSizer);
        root->Add(details, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        details->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [page](wxCollapsiblePaneEvent&) { page->Layout(); page->FitInside(); });

        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){stepFrame(-1);},ID_PREVIOUS_FRAME);
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){stepFrame(1);},ID_NEXT_FRAME);
        root->AddStretchSpacer();
        page->SetSizer(root);
        page->FitInside();
        notebook_->AddPage(page, "Texture", true);

        layerChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { setAnimationPlaying(false); refreshMipmapChoices(); refreshPreview(); });
        mipChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refreshPreview(); });
        previewMode_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refreshPreview(); });
        playButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { setAnimationPlaying(playButton_->GetValue()); });
    }

    void buildExportPage() {
        auto* page = new layout::ScrolledPage(notebook_);
        exportPage_ = page;
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* intro = new layout::WrappedLabel(page, wxID_ANY,
            "1. Choose the format and encoding.\n2. Choose a separate destination.\n3. Preview, then export a copy.\n\n"
            "Preview writes no files. Export leaves the open document and its unsaved edits unchanged. Save keeps the document's encoding.");
        root->Add(intro, 0, wxEXPAND | wxALL, FromDIP(8));
        outputFormat_=new wxChoice(page,wxID_ANY);
        for(const char* value:{"tpc","tga","dds","png","jpg","bmp","txi"})outputFormat_->Append(value);
        outputFormat_->SetName("Export format");
        outputFormat_->SetSelection(0);
        outputFormat_->SetMinSize(FromDIP(wxSize(150, -1)));
        auto* outputRow = layout::fieldRow(page, "Output format", outputFormat_);
        root->Add(outputRow,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        outputFormat_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){changeExportFormat();});
        optionsPanel_ = new EncodingOptionsPanel(page);
        optionsPanel_->setChangeHandler([this]() { onOptionsChanged(); });
        root->Add(optionsPanel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* destinationRow = new wxBoxSizer(wxVERTICAL);
        exportPath_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        exportPath_->SetName("Export destination");
        exportPath_->SetMinSize(FromDIP(wxSize(1, -1)));
        destinationRow->Add(exportPath_, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        destinationRow->Add(new wxButton(page, ID_CHOOSE_EXPORT, "Destination..."), 0);
        root->Add(destinationRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { chooseExportDestination(); }, ID_CHOOSE_EXPORT);
        auto* exportButtons = new wxWrapSizer(wxHORIZONTAL);
        exportButtons->Add(new wxButton(page,ID_ENCODE_PREVIEW,"Preview output"),0,wxRIGHT | wxBOTTOM,FromDIP(6));
        exportButtons->Add(new wxButton(page,ID_EXPORT_IMAGE,"Export copy..."),0,wxBOTTOM,FromDIP(6));
        root->Add(exportButtons,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        encodingSummary_ = new layout::WrappedLabel(page, wxID_ANY,
            "Export settings do not change the open document until applied.");
        root->Add(encodingSummary_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){buildEncodedPreview();},ID_ENCODE_PREVIEW);
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){exportImage();},ID_EXPORT_IMAGE);
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){applyEncoding();},ID_APPLY_ENCODING);
        root->AddStretchSpacer();
        page->SetSizer(root);
        page->FitInside();
        notebook_->AddPage(page, "Export", false);
    }

    void showExportPage() {
        if (busy_ || !document_.isOpen() || !exportPage_) return;
        const int index = notebook_->FindPage(exportPage_);
        if (index != wxNOT_FOUND) notebook_->SetSelection(static_cast<std::size_t>(index));
        outputFormat_->SetFocus();
    }

    void applyTxiHintTheme() {
        if (txiAutocompleteHint_ == nullptr || txiEditor_ == nullptr) return;
        const wxui::ThemePalette palette = wxui::themePalette(darkMode_);
        const int pointSize = std::max(9, txiEditor_->GetFont().GetPointSize());
        txiAutocompleteHint_->SetFont(
            wxFont(wxFontInfo(pointSize).Family(wxFONTFAMILY_TELETYPE)));
        txiAutocompleteHint_->SetForegroundColour(palette.mutedText);
        txiAutocompleteHint_->Refresh(false);
    }

    void buildTxiPage() {
        auto* page = new layout::ScrolledPage(notebook_);
        auto* root = new wxBoxSizer(wxVERTICAL);
        txiHelp_ = new layout::WrappedLabel(page, wxID_ANY,
            "TXI controls Odyssey material, animation, cube-map, font, and procedural behavior. Start typing a "
            "directive to see a muted completion and press Tab to accept it. Once the directive is complete, the "
            "same hint shows its value type or range. Ctrl+Space opens all matching dictionary entries.");
        root->Add(txiHelp_, 0, wxEXPAND | wxALL, FromDIP(8));
        txiEditor_ = new TxiEditor(page);
        txiEditor_->SetMinSize(FromDIP(wxSize(1, 180)));
        root->Add(txiEditor_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        txiAutocompleteHint_ = new layout::WrappedLabel(
            page, wxID_ANY, "Start typing a TXI directive. Tab accepts a unique completion.");
        root->Add(txiAutocompleteHint_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        auto* txiButtons = new wxWrapSizer(wxHORIZONTAL);
        txiButtons->Add(new wxButton(page, ID_IMPORT_TXI, "Import TXI..."), 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        txiButtons->Add(new wxButton(page, ID_EXPORT_TXI, "Export TXI..."), 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        txiButtons->Add(new wxButton(page, ID_COMMON_TXI, "Common values..."), 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){editCommonTxi();},ID_COMMON_TXI);
        root->Add(txiButtons, 0, wxEXPAND | wxALL, FromDIP(8));
        txiSummary_ = new layout::WrappedLabel(page, wxID_ANY, "No TXI metadata");
        root->Add(txiSummary_, 0, wxEXPAND | wxALL, FromDIP(8));
        txiIssues_ = new TxiDiagnosticsPanel(page);
        txiIssues_->setJumpHandler([this](std::size_t line) { txiEditor_->goToOneBasedLine(line); });
        txiIssues_->setLookupHandler([this](const std::string& key) {
            catalog_->selectDirective(key);
            showReferencePage();
            if (referencePage_ != nullptr) referencePage_->Layout();
        });
        root->Add(txiIssues_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        page->SetSizer(root);
        notebook_->AddPage(page, "TXI", false);
#ifdef __EMSCRIPTEN__
        txiEditor_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { onTxiChanged(); });
#else
        txiEditor_->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent&) { onTxiChanged(); });
#endif
        txiEditor_->Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& e){document_.finishEditGroup();e.Skip();});
        txiEditor_->Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& e){
            if(e.CmdDown() && (e.GetKeyCode()=='Z' || e.GetKeyCode()=='Y')) {
                undoRedo(e.GetKeyCode()=='Y' || e.ShiftDown());return;
            }
            e.Skip();
        });
        txiEditor_->setHintHandler([this](const wxString& hint) {
            if (txiAutocompleteHint_ == nullptr) return;
            txiAutocompleteHint_->SetLabel(hint);
            txiAutocompleteHint_->GetParent()->Layout();
        });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { importTxi(); }, ID_IMPORT_TXI);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { exportTxi(); }, ID_EXPORT_TXI);
    }

    void buildCatalogPage() {
        auto* page = new layout::ScrolledPage(notebook_);
        referencePage_ = page;
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* larger = new wxButton(page, wxID_ANY, "Open larger reference...");
        larger->SetToolTip("Open the TXI reference in a separate window.");
        larger->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { showTxiDictionary(); });
        root->Add(larger, 0, wxALL, FromDIP(8));
        catalog_ = new TxiDictionaryPanel(page);
        root->Add(catalog_, 1, wxEXPAND);
        page->SetSizer(root);
        notebook_->AddPage(page, "Reference", false);
    }

    void showReferencePage() {
        if (busy_ || referencePage_ == nullptr || notebook_ == nullptr) return;
        const int index = notebook_->FindPage(referencePage_);
        if (index != wxNOT_FOUND) notebook_->SetSelection(static_cast<std::size_t>(index));
        if (catalog_ != nullptr) catalog_->focusSearch();
    }

    void showTxiDictionary() {
        if (busy_) return;
        wxDialog dialog(this, wxID_ANY, "TXI dictionary", wxDefaultPosition, wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* page = new layout::ScrolledPage(&dialog);
        auto* content = new wxBoxSizer(wxVERTICAL);
        auto* dictionary = new TxiDictionaryPanel(page);
        dictionary->setQuery(catalog_->query(), catalog_->category());
        // Keep the current filters and selection when opening the larger view.
        dictionary->restoreSelection(catalog_->selectedDirective());
        content->Add(dictionary, 1, wxEXPAND);
        page->SetSizer(content);
        root->Add(page, 1, wxEXPAND);
        root->Add(dialog.CreateSeparatedButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, FromDIP(8));
        dialog.SetSizer(root);
        dialog.SetEscapeId(wxID_CLOSE);
        dialog.Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CLOSE); }, wxID_CLOSE);
        wxui::applyTheme(&dialog, darkMode_);
        neoview::applyFontScale(&dialog, fontScale_);
        wxui::configureResponsiveWindow(dialog, wxSize(940, 760), wxSize(400, 420));
        dialog.CentreOnParent(); wxui::constrainWindowToDisplay(dialog);
        dictionary->focusSearch();
        dialog.ShowModal();
        catalog_->setQuery(dictionary->query(), dictionary->category());
        catalog_->restoreSelection(dictionary->selectedDirective());
    }

    void chooseOpen(const std::filesystem::path& initialDirectory = {}) {
        if(busy_) return;
        wxFileDialog dialog(this, "Open texture", wxpath::toWx(initialDirectory), wxEmptyString, openWildcard(),
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) openPath(wxpath::fromWx(dialog.GetPath()));
    }

    bool save() {
        if(busy_ || !document_.isOpen())return false;
        if (!workflow::writableFormat(document_.texture().kind)) { showExportPage(); return false; }
        if (!document_.sourceBacked() || !workflow::canSaveInPlace(document_.texture().kind, document_.path())) return saveAs();
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto exportPrefs=optionsPanel_->options();const int outputSelection=outputFormat_->GetSelection();
            runTextureTask(this,"Save texture",[&](TextureTaskProgress&){document_.save();});
            discardEncodedPreview();refreshDocumentUi();
            optionsPanel_->setOptions(exportPrefs);outputFormat_->SetSelection(outputSelection);updateExportTarget();
            refreshExportSummary(); updateWindowState();
            setModuleStatusText("Saved original format; export preferences were not applied",0);return true;
        } catch(const texture::OperationCancelled&) {setModuleStatusText("Save cancelled; destination unchanged",0);return false;}
          catch(const std::exception& error){wxui::showError(this,error);return false;}
    }

    bool confirmSidecarReplacement(const fs::path& output) {
        if((document_.sourceBacked() && texture::canonicalPathKey(output)==texture::canonicalPathKey(document_.path())) || !texture::usesTxiSidecar(output))return true;
        try {
            const auto sidecar=texture::findTxiSidecar(output);
            return !sidecar || wxui::confirm(this,"Replace output metadata?",
                "The existing TXI will be replaced or removed to match the new image:\n\n"+texture::pathToUtf8(*sidecar));
        }catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    bool saveAs() {
        if(busy_ || !document_.isOpen())return false;
        if (!workflow::writableFormat(document_.texture().kind)) { showExportPage(); return false; }
        const auto ext = workflow::preserveFormatExtension(document_.texture().kind, document_.path());
        auto suggested = document_.path().filename(); suggested.replace_extension("." + ext);
        const auto filter=wxui::toWx("Preserve source (*."+ext+")|*."+ext);
        wxFileDialog dialog(this,"Save As - preserve source format",wxpath::toWx(document_.path().parent_path()),
            wxpath::toWx(suggested),filter,wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(dialog.ShowModal()!=wxID_OK)return false;
        auto output=wxpath::fromWx(dialog.GetPath());if(output.extension().empty())output+="."+ext;
        if(texture::kindForExtension(output)!=document_.texture().kind){wxMessageBox("Use Export / Convert to select a different format.","Preserve source format",wxOK|wxICON_INFORMATION,this);return false;}
        try { validateOutputPath(output); } catch(const std::exception& error) { wxui::showError(this,error); return false; }
        if(!confirmSidecarReplacement(output))return false;
        return saveActiveAs(output);
    }

    fs::path proposedOutput() const {
        const auto extension = wxui::toStd(outputFormat_->GetStringSelection());
        auto name = document_.path().filename(); name.replace_extension("." + extension);
        fs::path output = exportDestination_;
        if (output.empty()) output = document_.path().parent_path() / "exports" / name;
        // Keep a deliberately selected valid alias such as .jpeg or .jpe.
        if (!exportDestinationChosen_ || texture::kindForExtension(output) != texture::kindForExtension(fs::path("output." + extension)))
            output.replace_extension("." + extension);
        return output;
    }
    void resetExportDestination() {
        auto folder = document_.path().parent_path() / "exports";
#ifndef __EMSCRIPTEN__
        if (const auto remembered = settings_.readPath("Export/Directory")) folder = *remembered;
#endif
        exportDestination_ = folder / document_.path().filename();
        exportDestinationChosen_ = false;
    }
    bool chooseExportDestination() {
        if (busy_ || !document_.isOpen()) return false;
        const auto suggested=proposedOutput(); const auto ext=texture::extensionLower(suggested);
        wxFileDialog dialog(this,"Export destination",wxpath::toWx(suggested.parent_path()),wxpath::toWx(suggested.filename()),
            wxui::toWx("Output (*."+ext+")|*."+ext),wxFD_SAVE);
        if(dialog.ShowModal()!=wxID_OK)return false;
        auto path=wxpath::fromWx(dialog.GetPath()); if(path.extension().empty())path.replace_extension("."+ext);
        if(texture::kindForExtension(path)!=texture::kindForExtension(suggested)) {
            wxMessageBox("Choose the output format first, then use its extension.","Export format",wxOK|wxICON_INFORMATION,this);return false;
        }
        try { document_.validateExportDestination(path); validateOutputPath(path); }
        catch(const std::exception& e) { wxui::showError(this,e);return false; }
        exportDestination_=path;exportDestinationChosen_=true;
        updateExportTarget(); refreshExportSummary();return true;
    }
    void changeExportFormat() {
        if (loading_ || busy_ || !document_.isOpen()) return;
        exportPreferences_[activeExportFormat_] = optionsPanel_->options();
        activeExportFormat_ = wxui::toStd(outputFormat_->GetStringSelection());
        exportDestinationChosen_ = false;
        updateExportTarget();
        const auto found=exportPreferences_.find(activeExportFormat_);
        optionsPanel_->setOptions(found==exportPreferences_.end()?document_.saveOptions():found->second);
        exportDestinationChosen_=false;onOptionsChanged();
    }
    void updateExportTarget() {
        if(!document_.isOpen())return;
        optionsPanel_->setTarget(texture::kindForExtension(proposedOutput()),document_.texture().ddsDialect,document_.texture().sourceMipMapCount>1);
        if(exportPath_) {
            exportPath_->ChangeValue(wxpath::toWx(proposedOutput()));
            exportPath_->SetToolTip(wxpath::toWx(proposedOutput()));
        }
        for (std::size_t i = 0; i < comparisonImages_.size(); ++i) {
            auto& image = comparisonImages_[i];
            if (image.encodedPreview) { image.path = proposedOutput(); refreshComparisonLabel(i + 1, image.label); }
        }
    }
    void refreshExportSummary() {
        if (!encodingSummary_) return;
        if (!document_.isOpen()) {
            if (exportPath_) exportPath_->ChangeValue(wxString{});
            encodingSummary_->SetLabel("Open a texture to choose an output.");
            encodingSummary_->GetParent()->Layout();
            return;
        }
        const auto text = document_.exportSummary(proposedOutput(), optionsPanel_->options(), staged_.get());
        encodingSummary_->SetLabel(wxui::toWx(text));
        encodingSummary_->GetParent()->Layout();
        if (auto* scroll = wxDynamicCast(encodingSummary_->GetParent(), wxScrolledWindow)) scroll->FitInside();
    }
    void discardEncodedPreview() {
        staged_.reset();
        const auto old=comparisonImages_.size();
        comparisonImages_.erase(std::remove_if(comparisonImages_.begin(),comparisonImages_.end(),[](const auto& image){return image.encodedPreview;}),comparisonImages_.end());
        if(comparisonImages_.size()!=old){
            const bool playing=animationTimer_.IsRunning();
            rebuildComparisonWorkspace();refreshLayerChoices();refreshMipmapChoices();setAnimationPlaying(playing);refreshPreview();
        }
        refreshExportSummary();
    }
    bool buildEncodedPreview() {
        if(busy_ || !document_.isOpen())return false;
        const auto issue=document_.outputIssue(proposedOutput(),optionsPanel_->options());
        if(!issue.empty()){refreshExportSummary();wxMessageBox(wxui::toWx(issue),"Choose compatible settings",wxOK|wxICON_INFORMATION,this);return false;}
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto output=proposedOutput();const auto options=optionsPanel_->options();
            auto result=runTextureTask(this,"Encode proposed output",[&](TextureTaskProgress&) {return document_.preview(output,options);});
            discardEncodedPreview();staged_=std::make_unique<TexturePreview>(std::move(result));
            if (staged_->decoded.hasPixels())
                comparisonImages_.push_back(ComparisonImage{output,{},nullptr,nullptr,true});
            rebuildComparisonWorkspace();refreshMipmapChoices();refreshPreview();
            refreshExportSummary();
            updateWindowState();
            setModuleStatusText( staged_->decoded.hasPixels() ? "Encoded preview ready; no files written" : "TXI output prepared; no files written", 0);
            return true;
        } catch(const texture::OperationCancelled&) {setModuleStatusText("Encoded preview cancelled; no output written",0);return false;}
          catch(const std::exception& error){discardEncodedPreview();encodingSummary_->SetLabel(wxui::toWx(error.what()));encodingSummary_->GetParent()->Layout();wxui::showError(this,error);return false;}
    }
    bool exportImage() {
        if(busy_ || !document_.isOpen())return false;
        // Choose and validate ALL destinations before spending time encoding.
        if(!exportDestinationChosen_ && !chooseExportDestination())return false;
        const auto output=proposedOutput();
        try { document_.validateExportDestination(output); validateOutputPath(output); }
        catch(const std::exception& e){wxui::showError(this,e);return false;}
        std::error_code ec;
        if(fs::exists(output,ec) && !wxui::confirm(this,"Replace exported image?",texture::pathToUtf8(output)))return false;
        if(!confirmSidecarReplacement(output))return false;
        if(texture::kindForExtension(output)==texture::TextureFileKind::Jpeg && document_.texture().hasAlpha &&
            !wxui::confirm(this,"JPEG discards alpha","Export this image without transparency?"))return false;
        // A final settings check also covers typed controls and future handlers
        // which may forget to invalidate a previously encoded preview.
        if (staged_ && !document_.previewMatches(output, optionsPanel_->options(), *staged_))
            discardEncodedPreview();
        if(!staged_ && !buildEncodedPreview())return false;
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            runTextureTask(this,"Write encoded output",[&](TextureTaskProgress&){document_.commitPreview(output,*staged_,false);});
#ifndef __EMSCRIPTEN__
            settings_.writePath("Export/Directory",output.parent_path());
#endif
            setModuleStatusText(wxString("Exported copy; source unchanged: ")+wxpath::toWx(output),0);
            return true;
        } catch(const texture::OperationCancelled&){setModuleStatusText("Export cancelled",0);}
          catch(const std::exception& error){wxui::showError(this,error);}
        return false;
    }
    void applyEncoding() {
        if(busy_ || !document_.isOpen() || !document_.texture().hasPixels() ||
            !workflow::writableFormat(document_.texture().kind)) return;
        if(texture::kindForExtension(proposedOutput())!=document_.texture().kind){wxMessageBox("Choose this document's current format in Export settings first. Use Export for another format.","Change current encoding",wxOK|wxICON_INFORMATION,this);return;}
        if(!wxui::confirm(this,"Change current file encoding?","These settings may recompress pixels on the next Save. This change can be undone. Nothing is written now."))return;
        document_.finishEditGroup();document_.setSaveOptions(optionsPanel_->options());document_.finishEditGroup();discardEncodedPreview();afterPixelChange();
    }
    void undoRedo(bool redo) {
        if(busy_ || !document_.isOpen() || (redo?!document_.canRedo():!document_.canUndo()))return;
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto view=canvas_->view();const auto layer=layerChoice_->GetSelection();const auto mip=mipChoice_->GetSelection();
            runTextureTask(this,redo?"Redo texture change":"Undo texture change",[&](TextureTaskProgress&){if(redo)document_.redo();else document_.undo();});
            discardEncodedPreview();refreshDocumentUi();
            if(layer>=0&&static_cast<unsigned>(layer)<layerChoice_->GetCount())layerChoice_->SetSelection(layer);
            refreshMipmapChoices();if(mip>=0&&static_cast<unsigned>(mip)<mipChoice_->GetCount())mipChoice_->SetSelection(mip);
            refreshPreview();applyToPreviewCanvases([&](auto& c){c.setView(view);});
        } catch(const std::exception& error){wxui::showError(this,error);}
    }
    void editCommonTxi() {
        if (busy_ || !document_.isOpen()) return;
        wxDialog dialog(this, wxID_ANY, "Common TXI values", wxDefaultPosition, wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* content = new layout::ScrolledPage(&dialog);
        auto* form = new wxBoxSizer(wxVERTICAL);
        auto* help = new layout::WrappedLabel(content, wxID_ANY,
            "Blank removes a directive. Unchanged fields are left alone. The last duplicate is displayed; "
            "changing a field resolves its duplicates. Other directives, lists and comments are retained.");
        form->Add(help, 0, wxEXPAND | wxALL, FromDIP(10));
        std::array<wxTextCtrl*, 6> fields{};
        std::array<wxWindow*, 6> controls{};
        wxChoice* procedure = nullptr;
        const auto entries = texture::parseTxiEntries(document_.texture().txi);
        for (int group = 0; group < 2; ++group) {
            auto* box = new layout::GroupSizer(content, group == 0 ? "Animation" : "Material references");
            for (int i = group == 0 ? 0 : 4; i < (group == 0 ? 4 : 6); ++i) {
                const auto& field = workflow::commonTxiFields[i];
                const auto original = texture::getTxiValue(document_.texture().txi, field.key).value_or("");
                const auto count = std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
                    return !entry.blankOrComment && !entry.listData && entry.key == field.key;
                });
                wxString label = wxui::toWx(field.label);
                if (count > 1) label += " (duplicates)";
                box->Add(new layout::WrappedLabel(content, wxID_ANY, label), 0,
                         wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
                if (i == 0) {
                    procedure = new wxChoice(content, wxID_ANY);
                    procedure->Append("(unset)");
                    if (const auto info = texture::findTxiDirective(field.key))
                        for (const auto& value : info->allowedValues) procedure->Append(wxui::toWx(value));
                    int selected = original.empty() ? 0 : procedure->FindString(wxui::toWx(original), true);
                    if (selected == wxNOT_FOUND) selected = procedure->Append(wxui::toWx(original));
                    procedure->SetSelection(selected);
                    controls[i] = procedure;
                } else {
                    fields[i] = new wxTextCtrl(content, wxID_ANY, wxui::toWx(original));
                    controls[i] = fields[i];
                }
                controls[i]->SetName(wxui::toWx(field.label));
                controls[i]->SetMinSize(wxSize(1, -1));
                controls[i]->SetToolTip(wxui::toWx(texture::txiDirectiveHint(field.key)));
                box->Add(controls[i], 0, wxEXPAND | wxALL, FromDIP(8));
            }
            form->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        }
        form->Add(new layout::WrappedLabel(content, wxID_ANY,
            "Frame counts: 1-32767. FPS uses a decimal point, for example 7.5. "
            "Resource names are single tokens. Existing unknown values are retained unless you change them."),
            0, wxEXPAND | wxALL, FromDIP(10));
        content->SetSizer(form);
        root->Add(content, 1, wxEXPAND);
        auto* errorLabel = new layout::WrappedLabel(&dialog, wxID_ANY, wxEmptyString);
        root->Add(errorLabel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
        root->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(10));
        dialog.SetSizer(root);
        std::string candidate;
        dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
            std::array<std::string, 6> values;
            values[0] = procedure->GetSelection() <= 0 ? "" : wxui::toStd(procedure->GetStringSelection());
            for (std::size_t i = 1; i < fields.size(); ++i) values[i] = wxui::toStd(fields[i]->GetValue());
            try {
                const auto result = workflow::editCommonTxiValues(document_.texture().txi, values);
                if (result.invalidField >= 0) {
                    errorLabel->SetLabel(wxui::toWx(std::string(workflow::commonTxiFields[result.invalidField].label) +
                        ": " + result.error + " No edits have been applied."));
                    dialog.Layout(); // Do not resize/recentre the user's dialog on validation failure.
                    controls[result.invalidField]->SetFocus();
                    if (fields[result.invalidField]) fields[result.invalidField]->SelectAll();
                    return;
                }
                candidate = result.text;
                dialog.EndModal(wxID_OK);
            } catch (const std::exception& error) {
                errorLabel->SetLabel(wxui::toWx(std::string(error.what()) + " No edits have been applied."));
                dialog.Layout();
            }
        }, wxID_OK);
        wxui::applyTheme(&dialog, darkMode_); neoview::applyFontScale(&dialog, fontScale_);
        wxui::configureResponsiveWindow(dialog, wxSize(560, 680), wxSize(360, 360));
        dialog.CentreOnParent(); wxui::constrainWindowToDisplay(dialog);
        if (dialog.ShowModal() != wxID_OK || candidate == document_.texture().txi) return;
        try {
            document_.finishEditGroup(); document_.setTxi(std::move(candidate)); document_.finishEditGroup();
            loading_ = true; txiEditor_->setValue(wxui::toWx(document_.texture().txi)); loading_ = false;
            const bool playing = animationTimer_.IsRunning();
            afterPixelChange(); refreshTxiValidation();
            if (playing) setAnimationPlaying(true);
        } catch (const std::exception& error) { loading_ = false; wxui::showError(this, error); }
    }

    void splitTpc() {
        if (busy_) return;
        if (!document_.isOpen() || document_.texture().kind != neotpc::texture::TextureFileKind::Tpc ||
            !document_.texture().hasPixels()) {
            return;
        }

        auto suggested = document_.path().filename();
        suggested.replace_extension(".tga");
        wxFileDialog dialog(this, "Split TPC into TGA + TXI", wxpath::toWx(document_.path().parent_path()),
                            wxpath::toWx(suggested), "Targa image (*.tga)|*.tga",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) return;
        auto outputTga = wxpath::fromWx(dialog.GetPath());
#if !defined(__EMSCRIPTEN__)
        if (outputTga.extension().empty()) outputTga.replace_extension(".tga");
#endif
        fs::path outputTxi;
        try {
            document_.validateExportDestination(outputTga);
            validateOutputPath(outputTga);
            outputTxi=texture::findTxiSidecar(outputTga).value_or(fs::path(outputTga).replace_extension(".txi"));
            validateOutputPath(outputTxi);
        } catch(const std::exception& error) {wxui::showError(this,error);return;}

        std::error_code ec;
        if (fs::exists(outputTxi, ec) &&
            !wxui::confirm(this, "Replace TXI sidecar",
                           "The matching TXI file already exists. Replace both output files?")) {
            return;
        }

        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto pair = runTextureTask(this,"Split TPC",[&](TextureTaskProgress&){
                document_.validateExportDestination(outputTga);
                validateOutputPath(outputTga); validateOutputPath(outputTxi);
                return texture::saveTgaTxiPair(document_.texture(), outputTga);
            });
            setModuleStatusText(
                wxString("Split to ") + wxpath::toWx(pair.tga.filename()) + " + " +
                    wxpath::toWx(pair.txi.filename()),
                0);
        } catch (const texture::OperationCancelled&) {
            setModuleStatusText( "Split cancelled; unfinished output was not committed.", 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    void combineTgaTxi() {
        if(busy_) return;
        wxFileDialog tgaDialog(this, "Select TGA image", wxEmptyString, wxEmptyString,
                               "Targa image (*.tga)|*.tga", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (tgaDialog.ShowModal() != wxID_OK) return;
        const auto inputTga = wxpath::fromWx(tgaDialog.GetPath());

        std::optional<fs::path> inputTxi;
        try { inputTxi=texture::findTxiSidecar(inputTga); }
        catch(const std::exception& error) {wxui::showError(this,error);return;}
        if (inputTxi) {
            // Unique case-insensitive sidecar is already selected.
        } else {
            wxMessageDialog choice(this,
                "No same-name TXI sidecar was found.\n\nChoose Yes to select another TXI file, "
                "No to combine the TGA without TXI metadata, or Cancel to stop.",
                "Combine TGA + TXI", wxYES_NO | wxCANCEL | wxICON_QUESTION);
            const int result = choice.ShowModal();
            if (result == wxID_CANCEL) return;
            if (result == wxID_YES) {
                wxFileDialog txiDialog(this, "Select TXI metadata", wxpath::toWx(inputTga.parent_path()),
                                       wxEmptyString, "TXI metadata (*.txi)|*.txi",
                                       wxFD_OPEN | wxFD_FILE_MUST_EXIST);
                if (txiDialog.ShowModal() != wxID_OK) return;
                inputTxi = wxpath::fromWx(txiDialog.GetPath());
            }
        }

        auto suggested = inputTga.filename();
        suggested.replace_extension(".tpc");
        wxFileDialog outputDialog(this, "Save combined TPC", wxpath::toWx(inputTga.parent_path()),
                                  wxpath::toWx(suggested), "TPC texture (*.tpc)|*.tpc",
                                  wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (outputDialog.ShowModal() != wxID_OK) return;
        auto outputTpc = wxpath::fromWx(outputDialog.GetPath());
#if !defined(__EMSCRIPTEN__)
        if (outputTpc.extension().empty()) outputTpc.replace_extension(".tpc");
#endif

        try { if (document_.isOpen()) document_.validateExportDestination(outputTpc); validateOutputPath(outputTpc); }
        catch (const std::exception& error) { wxui::showError(this, error); return; }

        neotpc::texture::TextureSaveOptions initialOptions;
        if (document_.isOpen()) initialOptions = document_.saveOptions();
        TpcEncodingDialog optionsDialog(this, initialOptions);
        wxui::applyTheme(&optionsDialog, darkMode_);
        if (optionsDialog.ShowModal() != wxID_OK) return;

        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto options=optionsDialog.options();
            runTextureTask(this,"Combine TGA and TXI",[&](TextureTaskProgress&){
                if (document_.isOpen()) document_.validateExportDestination(outputTpc);
                validateOutputPath(outputTpc);
                texture::combineTgaTxiToTpc(inputTga,inputTxi,outputTpc,options);
            });
            settings_.addRecentFile(outputTpc);
            refreshRecentFiles();
            setModuleStatusText( wxString("Combined TGA + TXI into ") + wxpath::toWx(outputTpc), 0);
        } catch (const texture::OperationCancelled&) {
            setModuleStatusText( "Combine cancelled; unfinished output was not committed.", 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    void importTxi() {
        if (busy_ || !document_.isOpen()) return;
        wxFileDialog dialog(this, "Import TXI metadata", wxpath::toWx(document_.path().parent_path()),
                            wxEmptyString, "TXI metadata (*.txi)|*.txi",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        try {
            const auto path=wxpath::fromWx(dialog.GetPath());
            TextureBusyGuard busy(busy_);
            const auto imported=runTextureTask(this,"Import TXI",[&](TextureTaskProgress&){return texture::loadTexture(path);});
            document_.finishEditGroup(); document_.setTxi(imported.txi); document_.finishEditGroup();
            const bool playing = animationTimer_.IsRunning();
            discardEncodedPreview();
            if (playing) setAnimationPlaying(true);
            loading_ = true;
            txiEditor_->setValue(wxui::toWx(document_.texture().txi));
            loading_ = false;
            summary_->ChangeValue(wxui::toWx(document_.summary()));
            refreshTxiValidation();
            updateWindowState();
            setModuleStatusText( "Imported TXI metadata", 0);
        } catch (const texture::OperationCancelled&) {
            setModuleStatusText( "TXI import cancelled; document unchanged.", 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    void exportTxi() {
        if (busy_ || !document_.isOpen()) return;
        auto suggested = document_.path().filename();
        suggested.replace_extension(".txi");
        wxFileDialog dialog(this, "Export TXI metadata", wxpath::toWx(proposedOutput().parent_path()),
                            wxpath::toWx(suggested), "TXI metadata (*.txi)|*.txi",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) return;
        try {
            const auto output = workflow::txiExportDestination(wxpath::fromWx(dialog.GetPath()));
            TextureBusyGuard busy(busy_);
            runTextureTask(this,"Export TXI",[&](TextureTaskProgress&){
                document_.validateExportDestination(output);
                validateOutputPath(output);
                const auto encoded=document_.preview(output,document_.saveOptions());
                document_.commitPreview(output,encoded,false);
            });
            setModuleStatusText( wxString("Exported TXI to ") + wxpath::toWx(output), 0);
        } catch (const texture::OperationCancelled&) {
            setModuleStatusText( "TXI export cancelled.", 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    bool maybeSave() {
        if(busy_)return false;
        if (!document_.isOpen() || !document_.dirty()) return true;
        const bool readOnly = !workflow::writableFormat(document_.texture().kind);
        wxString message = wxString("Save changes to ") + wxpath::toWx(document_.path().filename()) + "?";
        if (readOnly) message = "This source format is read-only. Export a copy to preserve your edits before continuing.";
        wxMessageDialog dialog(this, message, "Unsaved texture changes",
            wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_WARNING);
        dialog.SetYesNoCancelLabels(readOnly ? "Export copy..." : "Save", "Discard changes", "Cancel");
        const int result = dialog.ShowModal();
        if (result == wxID_CANCEL) return false;
        if (result == wxID_NO) return true;
        if (!readOnly) return save();
        // Metadata-only export cannot preserve pending pixel/encoding edits.
        if ((document_.contentDirty() || document_.optionsDirty()) &&
            outputFormat_->GetStringSelection() == "txi") {
            outputFormat_->SetStringSelection("tpc"); changeExportFormat();
        }
        showExportPage();
        return exportImage();
    }

    void closeTexture() {
        if (busy_ || !maybeSave()) return;
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, true);
#endif
        document_.close(); resourceIdentity_.clear(); resourceProtectedInputs_.clear(); resourceSourceDescription_.clear(); staged_.reset();
        exportDestination_.clear(); exportDestinationChosen_ = false; activeExportFormat_.clear();
        comparisonImages_.clear();
        rebuildComparisonWorkspace();
        refreshDocumentUi();
    }

    void showBatch() {
        const auto initial = document_.isOpen() ? wxpath::toWx(document_.path().parent_path()) : wxString{};
        showBatchAt(initial);
    }

    void showBatchAt(const wxString& initial) {
        if (busy_) return;
        setAnimationPlaying(false);
        TextureBusyGuard busy(busy_); // No edits/open/close while a modal batch owns its snapshot.
        std::vector<fs::path> affectingOutputs, directOutputs;
        BatchEditorHooks hooks;
        hooks.prepare = [&](const texture::BatchPlan& plan) {
            affectingOutputs.clear(); directOutputs.clear();
            if (!document_.isOpen()) return std::string{};
            const auto sourceMembers = texture::batchOutputMembers(document_.path());
            for (const auto& row : plan.items) {
                if (texture::batchItemWritesOutput(row)) validateOutputPath(row.output);
                if (!texture::batchItemWritesOutput(row) || !document_.outputTouchesSource(row.output)) continue;
                affectingOutputs.push_back(row.output);
                const auto outputs = texture::batchOutputMembers(row.output);
                if (std::any_of(outputs.begin(), outputs.end(), [&](const auto& output) {
                    return std::any_of(sourceMembers.begin(), sourceMembers.end(), [&](const auto& source) {
                        return texture::canonicalPathKey(output) == texture::canonicalPathKey(source);
                    });
                })) directOutputs.push_back(row.output);
            }
            if (affectingOutputs.empty()) return std::string{};
            std::string note = "This batch updates the open texture or its TXI. The editor will refresh after a successful replacement.\n";
            if (document_.dirty()) note += "Unsaved edits to that document will be replaced and its old Undo history cleared after a successful replacement.\n";
            return note + "\n";
        };
        hooks.completed = [&](wxWindow* owner, const texture::BatchReport& report) {
            const bool replaced = std::any_of(report.items.begin(), report.items.end(), [&](const auto& row) {
                return row.status == texture::BatchItemStatus::Converted && row.wroteOutput &&
                    std::find(affectingOutputs.begin(), affectingOutputs.end(), row.output) != affectingOutputs.end();
            });
            if (!replaced || !document_.isOpen()) return;
            const auto view = canvas_->view();
            const bool directReplacement = std::any_of(report.items.begin(), report.items.end(), [&](const auto& row) {
                return row.status == texture::BatchItemStatus::Converted && row.wroteOutput &&
                    std::find(directOutputs.begin(), directOutputs.end(), row.output) != directOutputs.end();
            });
            const bool refreshed = runTextureTask(owner, "Refresh open texture", [&](TextureTaskProgress&) {
                // Real acknowledged replacement resets edits even if its bytes
                // happen to match. A renamed hard-link alias may not have changed
                // our source at all; don't discard edits in that case.
                if (directReplacement) { const auto path = document_.path(); document_.open(path); return true; }
                return document_.reloadIfSourceChanged();
            });
            if (!refreshed) return;
            staged_.reset();
            comparisonImages_.erase(std::remove_if(comparisonImages_.begin(), comparisonImages_.end(),
                [](const auto& image) { return image.encodedPreview; }), comparisonImages_.end());
            rebuildComparisonWorkspace();
            refreshDocumentUi(); // Retains valid frame, mip, channel and export choices.
            applyToPreviewCanvases([&](auto& canvas) { canvas.setView(view); });
            setModuleStatusText( "Open texture refreshed from the completed batch output", 0);
        };
        BatchDialog dialog(this, initial, darkMode_, std::move(hooks));
        neoview::applyFontScale(&dialog, fontScale_);
        dialog.Layout(); wxui::constrainWindowToDisplay(dialog);
        dialog.ShowModal();
    }

    void applyUiScale() {
        neoview::applyFontScale(this,fontScale_);
        txiEditor_->applyTheme(darkMode_);
        applyTxiHintTheme();
        Layout();
        for (std::size_t i = 0; i < notebook_->GetPageCount(); ++i)
            notebook_->GetPage(i)->Layout();
    }
    void changeUiScale(int steps) {
        fontScale_=neoview::steppedFontScale(fontScale_,steps);
        settings_.setFontScale(fontScale_);applyUiScale();
    }
    void compareImages() {
        if(busy_ || !document_.isOpen())return;
#if defined(__EMSCRIPTEN__)
        requestBrowserComparisonFiles();
#else
        wxFileDialog dialog(this,"Compare images",wxpath::toWx(document_.path().parent_path()),wxEmptyString,openWildcard(),wxFD_OPEN|wxFD_FILE_MUST_EXIST|wxFD_MULTIPLE);
        if(dialog.ShowModal()!=wxID_OK)return;
        wxArrayString selected;dialog.GetPaths(selected);std::vector<fs::path> paths;
        for(const auto& path:selected)paths.push_back(wxpath::fromWx(path));
        openConflictingImagesFromPaths(std::move(paths));
#endif
    }

    void openConflictingImages() {
        if (busy_ || !document_.isOpen()) return;
        discardEncodedPreview();
#if defined(__EMSCRIPTEN__)
        requestBrowserComparisonFiles();
#else
        openConflictingImagesFromPaths({}, true);
#endif
    }

#if defined(__EMSCRIPTEN__)
    void requestBrowserComparisonFiles() {
        if (busy_ || browserComparison_) return;
        const std::uint64_t request = ++browserComparisonGeneration_;
        wxWeakRef<TextureEditorPanelImpl> weak(this);
        neobrowser::requestRetainedFiles(
            "Select conflicting texture images", browserComparisonAccept(), true,
            [weak, request](neobrowser::RetainedFileSetResult result) mutable {
                if (!weak) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                auto* frame = weak.get();
                if (frame->browserComparisonGeneration_ != request || !frame->document_.isOpen()) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                frame->beginBrowserComparison(std::move(result));
            });
    }

    void beginBrowserComparison(neobrowser::RetainedFileSetResult result) {
        if (!result.error.empty()) {
            if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(wxui::toWx(result.error), "Open Conflicting Images",
                         wxOK | wxICON_ERROR, this);
            return;
        }
        if (result.cancelled()) return;
        if (result.sessionId == 0 || result.files.empty()) {
            if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox("No additional conflicting images were selected.",
                         "Open Conflicting Images", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (result.files.size() > kBrowserComparisonMaxSelectedFiles) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(
                wxString::Format("Select at most %llu texture and TXI files for one comparison.",
                                 static_cast<unsigned long long>(kBrowserComparisonMaxSelectedFiles)),
                "Comparison Selection Too Large", wxOK | wxICON_ERROR, this);
            return;
        }

        struct IndexedFile {
            neobrowser::RetainedFileInfo file;
            fs::path relative;
        };

        std::vector<IndexedFile> indexed;
        indexed.reserve(result.files.size());
        std::unordered_map<std::string, std::size_t> byPath;
        std::vector<std::string> failures;
        for (auto& file : result.files) {
            if (file.fileId == 0) {
                failures.push_back("A selected browser file has no retained-file identifier");
                continue;
            }
            std::string pathError;
            const auto relative = normalizeBrowserComparisonPath(file.relativePath, pathError);
            if (!relative) {
                failures.push_back(pathError);
                continue;
            }
            const std::string extension = neotpc::texture::extensionLower(*relative);
            if (!isBrowserComparisonPixelExtension(extension) && extension != "txi") continue;
            const std::uint64_t maximum = extension == "txi"
                ? static_cast<std::uint64_t>(kBrowserComparisonMaxTxiBytes)
                : static_cast<std::uint64_t>(kBrowserComparisonMaxTextureBytes);
            if (file.size > maximum) {
                failures.push_back(neotpc::texture::genericPathToUtf8(*relative) +
                                   ": exceeds the per-file browser comparison limit");
                continue;
            }
            const std::string key = browserComparisonPathKey(*relative);
            if (byPath.find(key) != byPath.end()) {
                failures.push_back(neotpc::texture::genericPathToUtf8(*relative) +
                                   ": duplicates another selected path ignoring case");
                continue;
            }
            const std::size_t index = indexed.size();
            indexed.push_back(IndexedFile{std::move(file), *relative});
            byPath.emplace(key, index);
        }

        std::vector<std::size_t> pixelIndices;
        pixelIndices.reserve(indexed.size());
        const std::string currentLeaf = neotpc::texture::asciiLower(
            neotpc::texture::pathToUtf8(document_.path().filename()));
        for (std::size_t index = 0; index < indexed.size(); ++index) {
            const auto& entry = indexed[index];
            const std::string extension = neotpc::texture::extensionLower(entry.relative);
            if (!isBrowserComparisonPixelExtension(extension)) continue;
            const std::string leaf = neotpc::texture::asciiLower(
                neotpc::texture::pathToUtf8(entry.relative.filename()));
            if (leaf == currentLeaf) continue;
            pixelIndices.push_back(index);
        }
        std::sort(pixelIndices.begin(), pixelIndices.end(), [&indexed](std::size_t left, std::size_t right) {
            return browserComparisonPathKey(indexed[left].relative) <
                   browserComparisonPathKey(indexed[right].relative);
        });

        std::vector<BrowserComparisonCandidate> candidates;
        std::vector<std::uint32_t> retainedIds;
        std::unordered_set<std::uint32_t> retainedIdSet;
        std::uint64_t aggregateBytes = 0;
        const std::size_t attemptCount = std::min(pixelIndices.size(), kBrowserComparisonMaxImages);
        candidates.reserve(attemptCount);
        for (std::size_t ordinal = 0; ordinal < attemptCount; ++ordinal) {
            const auto& entry = indexed[pixelIndices[ordinal]];
            BrowserComparisonCandidate candidate;
            candidate.source = entry.file;
            candidate.relativePath = entry.relative;

            fs::path sidecarPath = entry.relative;
            sidecarPath.replace_extension(".txi");
            const auto sidecar = byPath.find(browserComparisonPathKey(sidecarPath));
            if (sidecar != byPath.end()) candidate.sidecar = indexed[sidecar->second].file;

            std::uint64_t itemBytes = candidate.source.size;
            if (candidate.sidecar) {
                if (candidate.sidecar->size > kBrowserComparisonMaxAggregateInputBytes - itemBytes) {
                    failures.push_back(neotpc::texture::genericPathToUtf8(entry.relative) +
                                       ": source and TXI sidecar exceed the comparison input limit");
                    continue;
                }
                itemBytes += candidate.sidecar->size;
            }
            if (itemBytes > kBrowserComparisonMaxAggregateInputBytes - aggregateBytes) {
                failures.push_back(std::to_string(attemptCount - ordinal) +
                                   " image(s) skipped at the aggregate comparison input limit");
                break;
            }
            aggregateBytes += itemBytes;
            if (retainedIdSet.insert(candidate.source.fileId).second) {
                retainedIds.push_back(candidate.source.fileId);
            }
            if (candidate.sidecar && retainedIdSet.insert(candidate.sidecar->fileId).second) {
                retainedIds.push_back(candidate.sidecar->fileId);
            }
            candidates.push_back(std::move(candidate));
        }
        if (pixelIndices.size() > attemptCount) {
            failures.push_back(std::to_string(pixelIndices.size() - attemptCount) +
                               " additional image(s) skipped at the 16-pane browser limit");
        }

        if (candidates.empty()) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            std::string message = "No additional conflicting images could be loaded.";
            if (!failures.empty()) {
                message += "\n\n";
                for (const auto& failure : failures) message += failure + '\n';
            }
            wxMessageBox(wxui::toWx(message), "Open Conflicting Images",
                         wxOK | (failures.empty() ? wxICON_INFORMATION : wxICON_WARNING), this);
            return;
        }

        // Drop an existing decoded comparison before building its replacement so
        // two complete comparison working sets never coexist in browser memory.
        comparisonImages_.clear();
        rebuildComparisonWorkspace();
        refreshPreview();

        neobrowser::retainOnlyRetainedFiles(result.sessionId, retainedIds);
        auto state = std::make_unique<BrowserComparisonState>();
        state->generation = browserComparisonGeneration_;
        state->sessionId = result.sessionId;
        state->candidates = std::move(candidates);
        state->failures = std::move(failures);
        state->progress = std::make_unique<wxProgressDialog>(
            "Open Conflicting Images", "Preparing comparison images...",
            static_cast<int>(state->candidates.size()), this,
            wxPD_CAN_ABORT | wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
        browserComparison_ = std::move(state);
        updateWindowState();
        wxWeakRef<TextureEditorPanelImpl> weak(this);
        const std::uint64_t generation = browserComparison_->generation;
        wxTheApp->CallAfter([weak, generation]() {
            if (!weak || !weak->browserComparison_ ||
                weak->browserComparison_->generation != generation) return;
            weak->continueBrowserComparisonLoad();
        });
    }

    void continueBrowserComparisonLoad() {
        if (!browserComparison_) return;
        if (browserComparison_->index >= browserComparison_->candidates.size()) {
            finishBrowserComparisonLoad(false);
            return;
        }

        const auto& candidate = browserComparison_->candidates[browserComparison_->index];
        const wxString displayName = wxpath::toWx(candidate.relativePath.filename());
        if (browserComparison_->progress &&
            !browserComparison_->progress->Update(
                static_cast<int>(browserComparison_->index), wxString("Reading ") + displayName)) {
            finishBrowserComparisonLoad(true);
            return;
        }
        setModuleStatusText(
            wxString::Format("Comparison %llu/%llu: reading %s",
                             static_cast<unsigned long long>(browserComparison_->index + 1),
                             static_cast<unsigned long long>(browserComparison_->candidates.size()),
                             displayName.c_str()), 0);

        const std::uint64_t generation = browserComparison_->generation;
        wxWeakRef<TextureEditorPanelImpl> weak(this);
        browserComparison_->activeReadRequest = browser::requestRetainedFileBytes(
            browserComparison_->sessionId, candidate.source.fileId, candidate.source.size,
            kBrowserComparisonMaxTextureBytes,
            [weak, generation](browser::RetainedReadResult result) mutable {
                if (!weak) return;
                weak->handleBrowserComparisonSource(
                    generation, std::move(result.bytes), std::move(result.error));
            });
    }

    void handleBrowserComparisonSource(std::uint64_t generation,
                                       std::vector<std::uint8_t> bytes,
                                       std::string error) {
        if (!browserComparison_ || browserComparison_->generation != generation) return;
        browserComparison_->activeReadRequest = 0;
        if (!error.empty()) {
            completeBrowserComparisonItem(std::move(error));
            return;
        }
        browserComparison_->sourceBytes = std::move(bytes);
        const auto& candidate = browserComparison_->candidates[browserComparison_->index];
        if (!candidate.sidecar) {
            scheduleBrowserComparisonDecode(generation);
            return;
        }

        wxWeakRef<TextureEditorPanelImpl> weak(this);
        browserComparison_->activeReadRequest = browser::requestRetainedFileBytes(
            browserComparison_->sessionId, candidate.sidecar->fileId, candidate.sidecar->size,
            kBrowserComparisonMaxTxiBytes,
            [weak, generation](browser::RetainedReadResult result) mutable {
                if (!weak) return;
                weak->handleBrowserComparisonSidecar(
                    generation, std::move(result.bytes), std::move(result.error));
            });
    }

    void handleBrowserComparisonSidecar(std::uint64_t generation,
                                        std::vector<std::uint8_t> bytes,
                                        std::string error) {
        if (!browserComparison_ || browserComparison_->generation != generation) return;
        browserComparison_->activeReadRequest = 0;
        if (!error.empty()) {
            completeBrowserComparisonItem(std::move(error));
            return;
        }
        browserComparison_->sidecarTxi.assign(bytes.begin(), bytes.end());
        scheduleBrowserComparisonDecode(generation);
    }

    void scheduleBrowserComparisonDecode(std::uint64_t generation) {
        wxWeakRef<TextureEditorPanelImpl> weak(this);
        wxTheApp->CallAfter([weak, generation]() {
            if (!weak || !weak->browserComparison_ ||
                weak->browserComparison_->generation != generation) return;
            weak->decodeBrowserComparisonItem(generation);
        });
    }

    void decodeBrowserComparisonItem(std::uint64_t generation) {
        if (!browserComparison_ || browserComparison_->generation != generation ||
            browserComparison_->index >= browserComparison_->candidates.size()) return;
        const auto candidate = browserComparison_->candidates[browserComparison_->index];
        TextureBusyGuard busy(busy_);
        auto lastYield=std::chrono::steady_clock::now();
        texture::OperationScope operation([&]{
            if(std::chrono::steady_clock::now()-lastYield>std::chrono::milliseconds(45)) {
                if(browserComparison_->progress&&!browserComparison_->progress->Pulse("Decoding comparison"))throw texture::OperationCancelled();
                emscripten_sleep(1);lastYield=std::chrono::steady_clock::now();
            }
        });
        try {
            if (browserComparison_->progress &&
                !browserComparison_->progress->Pulse(
                    wxString("Decoding ") + wxpath::toWx(candidate.relativePath.filename()))) {
                finishBrowserComparisonLoad(true);
                return;
            }
            if (browserComparison_->decodedBytes >= kBrowserComparisonMaxDecodedBytes) {
                completeBrowserComparisonItem("comparison decoded-memory limit reached");
                return;
            }
            const std::uint64_t remainingDecoded =
                kBrowserComparisonMaxDecodedBytes - browserComparison_->decodedBytes;
            neotpc::texture::parser::ScopedResourceLimits limits(
                kBrowserComparisonMaxTextureBytes, remainingDecoded);
            auto texture = neotpc::texture::loadTextureBytes(
                browserComparison_->sourceBytes, candidate.relativePath,
                std::move(browserComparison_->sidecarTxi));
            std::vector<std::uint8_t>().swap(browserComparison_->sourceBytes);
            browserComparison_->sidecarTxi.clear();

            if (!texture.hasPixels()) {
                completeBrowserComparisonItem("no pixel data");
                return;
            }
            const std::uint64_t imageBytes = decodedTextureBytes(texture);
            if (imageBytes > remainingDecoded) {
                completeBrowserComparisonItem("comparison decoded-memory limit reached");
                return;
            }
            browserComparison_->decodedBytes += imageBytes;
            browserComparison_->loaded.push_back(
                ComparisonImage{candidate.relativePath, std::move(texture), nullptr, nullptr});
            completeBrowserComparisonItem({});
        } catch (const texture::OperationCancelled&) {
            finishBrowserComparisonLoad(true);
        } catch (const std::exception& error) {
            std::vector<std::uint8_t>().swap(browserComparison_->sourceBytes);
            browserComparison_->sidecarTxi.clear();
            completeBrowserComparisonItem(error.what());
        }
    }

    void completeBrowserComparisonItem(std::string failure) {
        if (!browserComparison_ ||
            browserComparison_->index >= browserComparison_->candidates.size()) return;
        const auto path = browserComparison_->candidates[browserComparison_->index].relativePath;
        if (!failure.empty()) {
            browserComparison_->failures.push_back(
                neotpc::texture::genericPathToUtf8(path) + ": " + std::move(failure));
        }
        std::vector<std::uint8_t>().swap(browserComparison_->sourceBytes);
        browserComparison_->sidecarTxi.clear();
        ++browserComparison_->index;
        const std::uint64_t generation = browserComparison_->generation;
        wxWeakRef<TextureEditorPanelImpl> weak(this);
        wxTheApp->CallAfter([weak, generation]() {
            if (!weak || !weak->browserComparison_ ||
                weak->browserComparison_->generation != generation) return;
            weak->continueBrowserComparisonLoad();
        });
    }

    void finishBrowserComparisonLoad(bool cancelled) {
        if (!browserComparison_) return;
        auto state = std::move(browserComparison_);
        if (state->activeReadRequest != 0) {
            browser::cancelRetainedFileRead(state->activeReadRequest);
        }
        if (state->sessionId != 0) neobrowser::releaseRetainedFileSet(state->sessionId);
        if (cancelled) state->failures.push_back("Comparison loading was cancelled");
        if (state->progress) {
            if (!cancelled) {
                state->progress->Update(
                    static_cast<int>(state->candidates.size()), "Comparison images loaded");
            }
            state->progress.reset();
        }

        comparisonImages_ = std::move(state->loaded);
        rebuildComparisonWorkspace();
        refreshPreview();
        updateWindowState();
        Layout();

        if (comparisonImages_.empty() && state->failures.empty()) {
            wxMessageBox("No additional conflicting images were selected.",
                         "Open Conflicting Images", wxOK | wxICON_INFORMATION, this);
        } else if (!state->failures.empty()) {
            std::string message = "Opened " + std::to_string(comparisonImages_.size()) +
                                  " conflicting image(s). The following selection(s) were not previewed:\n\n";
            for (const auto& failure : state->failures) message += failure + '\n';
            wxMessageBox(wxui::toWx(message), "Open Conflicting Images",
                         wxOK | wxICON_WARNING, this);
        }
        if (comparisonImages_.empty()) {
            setModuleStatusText(
                state->failures.empty() ? "No conflicting images found" :
                                          "No conflicting images could be previewed", 0);
        } else {
            setModuleStatusText(
                wxString::Format("Comparison view: %llu image pane(s)",
                                 static_cast<unsigned long long>(comparisonImages_.size() + 1)), 0);
        }
    }

    void cancelBrowserComparisonLoad(bool announce, bool refreshState) {
        ++browserComparisonGeneration_;
        if (!browserComparison_) return;
        auto state = std::move(browserComparison_);
        if (state->activeReadRequest != 0) {
            browser::cancelRetainedFileRead(state->activeReadRequest);
        }
        if (state->sessionId != 0) neobrowser::releaseRetainedFileSet(state->sessionId);
        state->progress.reset();
        if (announce) setModuleStatusText( "Comparison loading cancelled", 0);
        if (refreshState) updateWindowState();
    }
#else
    void openConflictingImagesFromPaths(std::vector<fs::path> paths, bool findVariants = false) {
        if(busy_ || !document_.isOpen()) return;
        struct Loaded {std::vector<ComparisonImage> images;std::vector<std::string> failures;bool cancelled=false;};
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto source=document_.path();
            auto result=runTextureTask(this,"Open comparisons",[&](TextureTaskProgress& progress){
                if (findVariants) paths=texture::findConflictingTexturePaths(source);
                Loaded loaded;std::uint64_t bytes=0;
                constexpr std::uint64_t limit=UINT64_C(256)*1024*1024;
                const auto count=std::min<std::size_t>(paths.size(),64);
                for(std::size_t i=0;i<count;++i) {
                    progress.set(i,count,"Reading "+texture::pathToUtf8(paths[i].filename()));
                    try {
                        texture::checkOperation();
                        std::error_code ec;
                        if(paths[i]==source || (fs::equivalent(paths[i],source,ec)&&!ec))continue;
                        if(bytes>=limit) {loaded.failures.push_back("Comparison memory limit reached; remaining images skipped");break;}
                        texture::parser::ScopedResourceLimits limits(UINT64_C(128)*1024*1024,limit-bytes);
                        auto image=texture::loadTexture(paths[i]);
                        const auto size=decodedTextureBytes(image);
                        if(!image.hasPixels())throw texture::TextureError("No pixel data");
                        if(size>limit-bytes)throw texture::TextureError("Comparison memory limit exceeded");
                        bytes+=size;loaded.images.push_back(ComparisonImage{paths[i],std::move(image),nullptr,nullptr});
                    } catch(const texture::OperationCancelled&) {loaded.cancelled=true;break;}
                      catch(const std::exception& error){loaded.failures.push_back(texture::pathToUtf8(paths[i].filename())+": "+error.what());}
                }
                if(paths.size()>count) loaded.failures.push_back("Additional images skipped at the 64-pane limit");
                return loaded;
            });
            comparisonImages_=std::move(result.images);staged_.reset();
            rebuildComparisonWorkspace();refreshMipmapChoices();refreshPreview();refreshExportSummary();updateWindowState();Layout();
            if(!result.failures.empty()) {
                std::string details;
                for(const auto& failure:result.failures)details+=failure+'\n';
                wxMessageBox(wxui::toWx(details),"Comparison results",wxOK|wxICON_WARNING,this);
            }
            setModuleStatusText(result.cancelled?wxString("Comparison loading cancelled; completed images retained"):
                wxString::Format("%llu read-only comparison(s)",static_cast<unsigned long long>(comparisonImages_.size())),0);
        } catch(const texture::OperationCancelled&){setModuleStatusText("Comparison loading cancelled",0);}
          catch(const std::exception& error){wxui::showError(this,error);}
    }

#endif

    void closeImageComparison() {
        if(busy_)return;
#if defined(__EMSCRIPTEN__)
        const bool wasLoading = browserComparison_ != nullptr;
        cancelBrowserComparisonLoad(wasLoading, true);
        if (wasLoading && comparisonImages_.empty()) return;
#endif
        if (busy_ || comparisonImages_.empty()) return;
        const int oldLayer = layerChoice_->GetSelection(), oldMip = mipChoice_->GetSelection();
        const bool playing = animationTimer_.IsRunning();
        staged_.reset(); comparisonImages_.clear();
        rebuildComparisonWorkspace(); refreshLayerChoices(); refreshMipmapChoices();
        setAnimationPlaying(playing); refreshPreview(); refreshExportSummary(); updateWindowState(); Layout();
        const bool changed = oldLayer != layerChoice_->GetSelection() || oldMip != mipChoice_->GetSelection();
        setModuleStatusText( changed ? "Comparison closed — selected an available source frame/mip" : "Image comparison closed", 0);
    }

    void refreshDocumentUi(bool resetExport = false) {
        setAnimationPlaying(false);
        const int retainedLayer=resetExport?0:std::max(0,layerChoice_->GetSelection());
        const int retainedMip=resetExport?0:std::max(0,mipChoice_->GetSelection());
        loading_ = true;
        layerChoice_->Clear();
        mipChoice_->Clear();
        if (!document_.isOpen()) {
            summary_->ChangeValue(wxEmptyString);
            txiEditor_->setValue(wxEmptyString, false);
            txiIssues_->setIssues({});
            txiSummary_->SetLabel("No TXI metadata");
            txiHelp_->SetLabel(
                "TXI controls Odyssey material, animation, cube-map, font, and procedural behavior. Open a texture "
                "to edit its embedded or sidecar metadata. Type a directive or press Ctrl+Space for TXI index suggestions.");
            canvas_->clearImage("Open a texture (Ctrl+O)\nDrop one file to edit, several to compare,\nor a folder to batch convert.");
            playButton_->Enable(false);
            playButton_->Show(false);
            mipChoice_->Enable(false);
            loading_ = false;
            refreshExportSummary();
            updateWindowState();
            return;
        }
        const auto& texture = document_.texture();
        refreshLayerChoices();
        if(layerChoice_->GetCount())layerChoice_->SetSelection(std::min(retainedLayer,static_cast<int>(layerChoice_->GetCount())-1));
        refreshMipmapChoices();
        if(mipChoice_->GetCount())mipChoice_->SetSelection(std::min(retainedMip,static_cast<int>(mipChoice_->GetCount())-1));
        if (resetExport || activeExportFormat_.empty()) {
            const auto extension = workflow::defaultExportExtension(texture.kind);
            outputFormat_->SetStringSelection(wxui::toWx(extension)); activeExportFormat_=extension;
            resetExportDestination(); updateExportTarget(); optionsPanel_->setOptions(document_.saveOptions());
        } else updateExportTarget();
        if (texture.kind == neotpc::texture::TextureFileKind::Tpc) {
            txiHelp_->SetLabel(
                "This TXI is embedded in the TPC. Edit it here and use Save. Type a directive or press Ctrl+Space "
                "for TXI index suggestions. When TXI is the only change, NeoTPC replaces only the metadata footer "
                "and preserves the encoded image and mipmaps byte-for-byte.");
        } else if (texture.kind == texture::TextureFileKind::Txb) {
            txiHelp_->SetLabel("TXI is embedded in this read-only TXB source. Export to TPC to keep image and metadata together, or export TXI separately. Save does not write TXB.");
        } else if (texture.kind == texture::TextureFileKind::Txi) {
            txiHelp_->SetLabel("This is a standalone TXI document; there are no image pixels. Save updates this file. Export TXI writes a separate copy.");
        } else {
            txiHelp_->SetLabel(
                "This metadata is stored in a same-name TXI sidecar when the image format does not embed TXI. "
                "Type a directive or press Ctrl+Space for TXI index suggestions. Save or convert the texture to write it.");
        }
        if (resetExport) notebook_->SetSelection(texture.kind == texture::TextureFileKind::Txi ? 1 : 0);
        txiEditor_->setValue(wxui::toWx(texture.txi), !resetExport);
        summary_->ChangeValue(wxui::toWx(document_.summary()));
        loading_ = false;
        refreshTxiValidation();
        refreshPreview();
        updateWindowState();
        refreshExportSummary();
        Layout();
    }

    const texture::TextureData* layerReference() const {
        const texture::TextureData* reference=document_.isOpen()?&document_.texture():nullptr;
        for(std::size_t i=1;i<=comparisonImages_.size();++i) {
            const auto* candidate=comparisonTexture(i);
            if(candidate && (!reference||candidate->layers.size()>reference->layers.size()))reference=candidate;
        }
        return reference;
    }
    std::size_t mappedLayer(const texture::TextureData& image,std::size_t selection) const {
        const auto* reference=layerReference();
        if(reference && reference->cubeMap && reference->cubeFaces.size()==reference->layers.size() &&
            selection<reference->cubeFaces.size() && image.cubeMap && image.cubeFaces.size()==image.layers.size()) {
            const auto face=std::find(image.cubeFaces.begin(),image.cubeFaces.end(),reference->cubeFaces[selection]);
            return static_cast<std::size_t>(std::distance(image.cubeFaces.begin(),face));
        }
        return selection;
    }
    void refreshLayerChoices() {
        const int previous=layerChoice_->GetSelection();layerChoice_->Clear();
        const auto* reference=layerReference();
        if(reference)for(std::size_t i=0;i<reference->layers.size();++i) {
            std::string name=(reference->animated?"Frame ":reference->cubeMap?"Cube face ":"Layer ")+std::to_string(i+1);
            if(reference->cubeMap&&reference->cubeFaces.size()==reference->layers.size())name="Cube face "+texture::cubeFaceToString(reference->cubeFaces[i]);
            layerChoice_->Append(wxui::toWx(name));
        }
        const auto count=static_cast<int>(layerChoice_->GetCount());
        if(count>0)layerChoice_->SetSelection(std::clamp(previous,0,count-1));
        layerChoice_->Enable(count>1);
        const bool playable=reference&&reference->animated&&count>1;
        playButton_->Enable(playable);playButton_->Show(playable);
        for(int id:{ID_PREVIOUS_FRAME,ID_NEXT_FRAME})if(auto* button=FindWindow(id)) {
            button->Enable(count>1);
            button->SetToolTip(id == ID_PREVIOUS_FRAME ? "Previous layer or frame" : "Next layer or frame");
        }
    }
    void refreshMipmapChoices() {
        const int previous=mipChoice_->GetSelection();mipChoice_->Clear();
        std::size_t maximum=0;bool pixels=false;
        const auto layer=static_cast<std::size_t>(std::max(0,layerChoice_->GetSelection()));
        for(std::size_t i=0;i<=comparisonImages_.size();++i){const auto* image=comparisonTexture(i);if(image){const auto mapped=mappedLayer(*image,layer);if(mapped<image->layers.size()){pixels=true;maximum=std::max(maximum,image->layers[mapped].mipmaps.size());}}}
        if(!pixels){mipChoice_->Enable(false);return;}
        for(std::size_t i=0;i<=maximum;++i)mipChoice_->Append(i==0?wxString("Level 0 (base)"):wxString::Format("Level %llu",static_cast<unsigned long long>(i)));
        mipChoice_->SetSelection(std::clamp(previous,0,static_cast<int>(maximum)));mipChoice_->Enable(maximum>0);
    }

    void clearDisplayCache() {
        displayImages_.clear(); displayedKeys_.clear(); displayBytes_ = 0;
    }
    wxImage displayImage(const DisplayKey& key) {
        const auto found = std::find_if(displayImages_.begin(), displayImages_.end(),
            [&](const auto& item) { return item.key == key; });
        if (found != displayImages_.end()) {
            displayImages_.splice(displayImages_.begin(), displayImages_, found);
            return displayImages_.front().image; // wxImage shares immutable pixel storage.
        }
        auto image = makePreviewImage(*key.layer, key.mode);
        constexpr std::size_t budget = 64u * 1024u * 1024u;
        const std::size_t bytes = image.IsOk() ? static_cast<std::size_t>(image.GetWidth()) * image.GetHeight() * 3u : 0;
        if (bytes && bytes <= budget) {
            while (!displayImages_.empty() && (displayBytes_ + bytes > budget || displayImages_.size() >= 64)) {
                displayBytes_ -= displayImages_.back().bytes; displayImages_.pop_back();
            }
            displayImages_.push_front({key, image, bytes}); displayBytes_ += bytes;
        }
        return image;
    }

    void refreshPreview() {
        const auto retainedView=canvas_ ? canvas_->view() : TextureCanvas::View{};
        if (!document_.isOpen()) {
            applyToPreviewCanvases([](auto& canvas) { canvas.clearImage("Open or drop a texture to begin"); });
            refreshStatus();
            return;
        }
        int selection = layerChoice_->GetSelection();
        if (selection == wxNOT_FOUND) selection = 0;
        int mipSelection = mipChoice_->GetSelection();
        if (mipSelection == wxNOT_FOUND) mipSelection = 0;
        const auto layerIndex = static_cast<std::size_t>(std::max(0, selection));
        const auto mipIndex = static_cast<std::size_t>(std::max(0, mipSelection));
        const int previewMode = previewMode_->GetSelection();
        for(std::size_t pane=0;pane<=comparisonImages_.size();++pane) {
            auto* canvas=pane==0?canvas_:comparisonImages_[pane-1].canvas;
            const auto* image=comparisonTexture(pane);
            if(!canvas)continue;
            const auto* pixels=image?previewLayerAt(*image,mappedLayer(*image,layerIndex),mipIndex):nullptr;
            // Do not rebuild unchanged panes when another pane animates. A
            // bounded LRU also reuses frames/channel views when revisited.
            const DisplayKey key{pixels, pane == 0 ? document_.pixelRevision() : 0, previewMode};
            const auto displayed = displayedKeys_.find(canvas);
            if (displayed != displayedKeys_.end() && displayed->second == key) continue;
            if (pixels) canvas->setImage(displayImage(key));
            else canvas->clearImage(image && !image->hasPixels() ? "TXI metadata only\nEdit the TXI tab; there is no pixel preview." : "Selected mip / frame is not present");
            displayedKeys_[canvas] = key;
        }
        applyToPreviewCanvases([&](auto& c){c.setView(retainedView);});
        refreshStatus();
    }

    void refreshStatus() {
        if(!document_.isOpen() || !document_.texture().hasPixels()){setModuleStatusText("Ready",1);return;}
        const auto layer=static_cast<std::size_t>(std::max(0,layerChoice_->GetSelection()));
        const auto mip=static_cast<std::size_t>(std::max(0,mipChoice_->GetSelection()));
        if(const auto* image=previewLayerAt(document_.texture(),mappedLayer(document_.texture(),layer),mip))
            setModuleStatusText(wxString::Format("%u x %u | layer %llu | mip %llu | %d%%",image->width,image->height,static_cast<unsigned long long>(layer+1),static_cast<unsigned long long>(mip),canvas_->zoomPercent()),1);
        else setModuleStatusText("Mip / frame not present in original",1);
    }

    void refreshTxiValidation() {
        if (!document_.isOpen()) { txiIssues_->setIssues({}); return; }
        const auto issues = neotpc::texture::validateTxiText(document_.texture().txi);
        txiIssues_->setIssues(issues);
        std::size_t errors = 0, warnings = 0, infos = 0;
        for (const auto& issue : issues) {
            if (issue.severity == texture::TxiIssueSeverity::Error) ++errors;
            else if (issue.severity == texture::TxiIssueSeverity::Warning) ++warnings;
            else ++infos;
        }
        if (issues.empty()) {
            txiSummary_->SetLabel(document_.texture().txi.empty() ? "No TXI metadata" : "TXI syntax/directive checks passed (not a game-layout check)");
        } else {
            txiSummary_->SetLabel(wxString::Format("%llu error(s), %llu warning(s), %llu information note(s)",
                static_cast<unsigned long long>(errors), static_cast<unsigned long long>(warnings),
                static_cast<unsigned long long>(infos)));
        }
    }

    void onOptionsChanged() {
        if (busy_ || loading_ || !document_.isOpen()) return;
        discardEncodedPreview();updateExportTarget();
        summary_->ChangeValue(wxui::toWx(document_.summary()));
        refreshExportSummary();
        updateWindowState();
    }

    void onTxiChanged() {
        if (busy_ || loading_ || !document_.isOpen()) return;
        document_.setTxi(wxui::toStd(txiEditor_->value()));
        const bool playing=animationTimer_.IsRunning();
        discardEncodedPreview();
        if (playing) setAnimationPlaying(true);
        summary_->ChangeValue(wxui::toWx(document_.summary()));
        refreshTxiValidation();
        updateWindowState();
    }

    void updateWindowState() {
        const bool open = document_.isOpen();
        enableModuleCommand(wxID_UNDO,open&&document_.canUndo());
        enableModuleCommand(wxID_REDO,open&&document_.canRedo());
        const bool validExport=open && document_.outputIssue(proposedOutput(),optionsPanel_->options()).empty();
        enableModuleCommand(ID_EXPORT_IMAGE,open); // Opens settings even when the current combination is invalid.
        const bool sourceWritable = open && workflow::writableFormat(document_.texture().kind);
        const bool editablePixels = open && document_.texture().hasPixels();
        const bool canApplyEncoding = editablePixels && sourceWritable && validExport &&
            texture::kindForExtension(proposedOutput()) == document_.texture().kind;
        enableModuleCommand(ID_APPLY_ENCODING,canApplyEncoding);
        if (undoMenuItem_) undoMenuItem_->SetItemLabel(document_.canUndo() ? wxui::toWx("Undo " + document_.undoLabel()) + "\tCtrl+Z" : wxString("Undo\tCtrl+Z"));
        if (redoMenuItem_) redoMenuItem_->SetItemLabel(document_.canRedo() ? wxui::toWx("Redo " + document_.redoLabel()) + "\tCtrl+Y" : wxString("Redo\tCtrl+Y"));
        enableModuleCommand(ID_COMPARE_IMAGES,open);
        if(imageInfo_) {
            if(open) {
                const auto& t=document_.texture();
                std::string info = texture::textureFileKindToString(t.kind);
                if (t.hasPixels()) info += " | " + std::to_string(t.canvasWidth) + " x " + std::to_string(t.canvasHeight) +
                    " | " + texture::textureCompressionToString(t.preferredCompression) + "\n" +
                    std::to_string(t.layers.size()) + " frame(s)/face(s) | " + std::to_string(t.sourceMipMapCount) + " mip level(s)";
                else info += " | metadata only\nEdit this document in the TXI tab.";
                if (!sourceWritable) info += "\nRead-only source format: export a copy to preserve edits.";
                else if (!document_.sourceBacked()) info += "\nArchive resource snapshot: Save creates a separate working file.";
                else if (!workflow::canSaveInPlace(t.kind, document_.path())) info += "\nFilename does not match the detected format. Save will ask for a correctly named copy.";
                if(document_.layoutPending())info+="\nLayout changes pending — current image uses the loaded layout.";
                imageInfo_->SetLabel(wxui::toWx(info));
            } else imageInfo_->SetLabel("Open a texture to begin.");
        }
        for(int id:{ID_ENCODE_PREVIEW,ID_EXPORT_IMAGE,ID_COMMON_TXI})if(auto* button=FindWindow(id))button->Enable(open);
        if(optionsPanel_)optionsPanel_->Enable(open);
        if(outputFormat_)outputFormat_->Enable(open);
        for(int id:{ID_ENCODE_PREVIEW,ID_EXPORT_IMAGE}) if(auto* button=FindWindow(id))button->Enable(validExport);
        if(auto* button=FindWindow(ID_CHOOSE_EXPORT))button->Enable(open);
        const bool pixels = editablePixels;
        const auto* reference = open ? layerReference() : nullptr;
        const bool previewPixels = reference && reference->hasPixels();
        for (auto* choice : {previewMode_, samplingChoice_, gridChoice_}) choice->Enable(previewPixels);
        layerChoice_->Enable(previewPixels && layerChoice_->GetCount() > 1);
        mipChoice_->Enable(previewPixels && mipChoice_->GetCount() > 1);
        playButton_->Enable(previewPixels && reference->animated && reference->layers.size() > 1);
        playButton_->Show(previewPixels && reference->animated && reference->layers.size() > 1);
        for (int id : {ID_PREVIOUS_FRAME, ID_NEXT_FRAME})
            if (auto* button = FindWindow(id)) button->Enable(previewPixels && layerChoice_->GetCount() > 1);
        for (int id : {ID_SET_ALPHA, ID_SCALE_ALPHA, ID_INVERT_ALPHA, ID_FLIP_HORIZONTAL, ID_FLIP_VERTICAL})
            if (auto* button = FindWindow(id)) button->Enable(pixels);
        if (auto* button = FindWindow(ID_ENCODE_PREVIEW))
            button->SetLabel(open && texture::kindForExtension(proposedOutput()) == texture::TextureFileKind::Txi
                ? "Prepare TXI" : "Preview output");
        refreshComparisonLabel(0, sourceLabel_);
        for (std::size_t i = 0; i < comparisonImages_.size(); ++i)
            refreshComparisonLabel(i + 1, comparisonImages_[i].label);
#if defined(__EMSCRIPTEN__)
        const bool comparisonLoading = browserComparison_ != nullptr;
#else
        const bool comparisonLoading = false;
#endif
        wxString name = open ? wxpath::toWx(document_.path().filename()) : wxString("Untitled");
        if (!comparisonImages_.empty()) {
            name += wxString::Format(" + %llu comparison(s)",
                                     static_cast<unsigned long long>(comparisonImages_.size()));
        }
        if (document_.dirty()) name += " *";
        setModuleTitle(name + " - NeoTPC");
        for (int id : {ID_SAVE_AS, ID_CLOSE_TEXTURE, ID_IMPORT_TXI, ID_EXPORT_TXI}) {
            enableModuleCommand(id, open);
        }
        enableModuleCommand(ID_OPEN_CONFLICTS, open && !comparisonLoading);
        enableModuleCommand(ID_SPLIT_TPC,
            open && pixels && document_.texture().kind == neotpc::texture::TextureFileKind::Tpc);
        enableModuleCommand(ID_COMBINE_TGA_TXI, true);
        enableModuleCommand(ID_CLOSE_CONFLICTS, comparisonLoading || !comparisonImages_.empty());
        enableModuleCommand(ID_SAVE_AS, sourceWritable);
        enableModuleCommand(wxID_SAVE, sourceWritable && document_.dirty());
        if (txiEditor_ != nullptr) txiEditor_->Enable(open);
        if (auto* importButton = FindWindow(ID_IMPORT_TXI)) importButton->Enable(open);
        if (auto* exportButton = FindWindow(ID_EXPORT_TXI)) exportButton->Enable(open);
        for (int id : {ID_FIT_IMAGE, ID_ACTUAL_SIZE, ID_ZOOM_IN, ID_ZOOM_OUT,
                       ID_SET_ALPHA, ID_SCALE_ALPHA, ID_INVERT_ALPHA, ID_FLIP_HORIZONTAL, ID_FLIP_VERTICAL}) {
            enableModuleCommand(id, id == ID_FIT_IMAGE || id == ID_ACTUAL_SIZE || id == ID_ZOOM_IN || id == ID_ZOOM_OUT ? previewPixels : pixels);
        }
        // Labels and visible transport controls can change without a size event.
        if (auto* page = notebook_->GetCurrentPage()) page->Layout();
    }

    void setAnimationPlaying(bool playing) {
        const auto* reference=layerReference();
        if (!playing || !reference || !reference->animated || reference->layers.size() < 2) {
            animationTimer_.Stop();
            playButton_->SetValue(false);
            playButton_->SetLabel("Play");
            return;
        }
        double fps=texture::parseTxiFeatures(reference->txi).fps;
        if(!std::isfinite(fps)||fps<=0.0)fps=8.0;
        fps=std::clamp(fps,.01,1000.0); // Playback limit only; never rewrites TXI.
        animationStart_=std::chrono::steady_clock::now();animationStartFrame_=std::max(0,layerChoice_->GetSelection());animationFps_=fps;
        playButton_->SetValue(true);
        animationTimer_.Start(static_cast<int>(std::clamp(1000.0/fps,16.0,10000.0)));
        playButton_->SetLabel("Pause");
    }

    void onAnimationTimer(wxTimerEvent&) {
        if(busy_ || layerChoice_->GetCount()<2)return;
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-animationStart_).count();
        const auto count=static_cast<unsigned>(layerChoice_->GetCount());
        const auto frame=static_cast<unsigned>(std::fmod(std::floor(seconds*animationFps_)+animationStartFrame_,count));
        if(static_cast<int>(frame)==layerChoice_->GetSelection())return;
        layerChoice_->SetSelection(static_cast<int>(frame));refreshMipmapChoices();refreshPreview();
    }
    void stepFrame(int direction) {
        if(busy_ || layerChoice_->GetCount()<2)return;setAnimationPlaying(false);
        const int count=static_cast<int>(layerChoice_->GetCount());
        layerChoice_->SetSelection((std::max(0,layerChoice_->GetSelection())+direction+count)%count);refreshMipmapChoices();refreshPreview();
    }
    void pixelOperation(const wxString& label,const std::function<void()>& operation) {
        if(busy_ || !document_.isOpen())return;
        try {setAnimationPlaying(false);TextureBusyGuard busy(busy_);runTextureTask(this,label,[&](TextureTaskProgress&){operation();});afterPixelChange();}
        catch(const texture::OperationCancelled&){setModuleStatusText("Pixel operation cancelled; document unchanged",0);}
        catch(const std::exception& error){wxui::showError(this,error);}
    }

    void setAlpha() {
        if (busy_ || !document_.isOpen() || !document_.texture().hasPixels()) return;
        wxTextEntryDialog dialog(this, "Alpha value (0-255)", "Set alpha", "255");
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        long value = 0;
        if (!dialog.GetValue().ToLong(&value) || value < 0 || value > 255) {
            wxMessageBox("Alpha must be an integer from 0 through 255.", "Set alpha", wxOK | wxICON_WARNING, this);
            return;
        }
        pixelOperation("Set alpha",[&]{document_.setAlpha(static_cast<std::uint8_t>(value));});
    }

    void scaleAlpha() {
        if (busy_ || !document_.isOpen() || !document_.texture().hasPixels()) return;
        wxTextEntryDialog dialog(this, "Non-negative scale factor", "Scale alpha", "1.0");
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        double value = 0.0;
        if (!dialog.GetValue().ToDouble(&value) || !std::isfinite(value) || value < 0.0) {
            wxMessageBox("Alpha scale must be a non-negative number.", "Scale alpha", wxOK | wxICON_WARNING, this);
            return;
        }
        pixelOperation("Scale alpha",[&]{document_.scaleAlpha(value);});
    }

    void invertAlpha() {
        if (busy_ || !document_.isOpen()) return;
        pixelOperation("Invert alpha",[&]{document_.invertAlpha();});
    }

    void flipHorizontal() {
        if (busy_ || !document_.isOpen()) return;
        pixelOperation("Flip horizontal",[&]{document_.flipHorizontal();});
    }

    void flipVertical() {
        if (busy_ || !document_.isOpen()) return;
        pixelOperation("Flip vertical",[&]{document_.flipVertical();});
    }

    void afterPixelChange() {
        discardEncodedPreview();
        summary_->ChangeValue(wxui::toWx(document_.summary()));
        refreshPreview();
        refreshExportSummary();
        updateWindowState();
    }

    void toggleDarkMode(wxCommandEvent& event) {
        darkMode_ = event.IsChecked();
        wxui::writeDarkMode(kAppName, darkMode_);
        wxui::applyTheme(this, darkMode_);
        txiEditor_->applyTheme(darkMode_);
        applyTxiHintTheme();
        applyToPreviewCanvases([this](auto& canvas) { canvas.setDarkMode(darkMode_); });
    }

    void showAbout() {
        wxAboutDialogInfo info;
        info.SetName(kAppName);
        info.SetVersion(NEOTPC_VERSION);
        info.SetDescription("TPC/TXB/TGA/DDS texture viewer, image comparator, TXI inspector, and converter for the Neo tool suite.");
        info.SetIcon(makeAppIcon());
        wxAboutBox(info, this);
    }


    void validateOutputPath(const fs::path& path) const {
        const auto validateOne = [this](const fs::path& candidate) {
            neoshared::checkResourceOutput(candidate, resourceProtectedInputs_);
            validateHostOutput(candidate);
        };
        validateOne(path);
        if (texture::usesTxiSidecar(path)) {
            auto sidecar = path;
            sidecar.replace_extension(".txi");
            validateOne(sidecar);
        }
    }

    neosettings::AppSettings settings_;
    TextureDocument document_;
    std::string resourceIdentity_;
    std::string resourceSourceDescription_;
    std::vector<fs::path> resourceProtectedInputs_;
    std::unique_ptr<TexturePreview> staged_;
    wxChoice* outputFormat_=nullptr;
    wxChoice* samplingChoice_=nullptr;
    wxChoice* gridChoice_=nullptr;
    wxStaticText* encodingSummary_=nullptr;
    wxTextCtrl* exportPath_=nullptr;
    wxStaticText* imageInfo_=nullptr;
    fs::path exportDestination_;
    bool exportDestinationChosen_=false;
    std::string activeExportFormat_;
    std::unordered_map<std::string,texture::TextureSaveOptions> exportPreferences_;
    double fontScale_=1.0;
    bool busy_=false,syncingView_=false;
    std::chrono::steady_clock::time_point animationStart_;
    int animationStartFrame_=0;double animationFps_=8;
    TextureCanvas* canvas_ = nullptr;
    wxPanel* previewHost_ = nullptr;
    wxBoxSizer* previewHostSizer_ = nullptr;
    std::vector<ComparisonImage> comparisonImages_;
    std::list<DisplayImage> displayImages_;
    std::unordered_map<TextureCanvas*, DisplayKey> displayedKeys_;
    std::size_t displayBytes_ = 0;
#if defined(__EMSCRIPTEN__)
    std::unique_ptr<BrowserComparisonState> browserComparison_;
    std::uint64_t browserComparisonGeneration_ = 0;
#endif
    std::vector<std::pair<wxSplitterWindow*, bool>> comparisonSplitters_;
    wxNotebook* notebook_ = nullptr;
    wxScrolledWindow* exportPage_ = nullptr;
    wxScrolledWindow* referencePage_ = nullptr;
    wxStaticText* sourceLabel_ = nullptr;
    wxChoice* layerChoice_ = nullptr;
    wxChoice* mipChoice_ = nullptr;
    wxToggleButton* playButton_ = nullptr;
    wxChoice* previewMode_ = nullptr;
    wxTextCtrl* summary_ = nullptr;
    EncodingOptionsPanel* optionsPanel_ = nullptr;
    wxStaticText* txiHelp_ = nullptr;
    TxiEditor* txiEditor_ = nullptr;
    wxStaticText* txiAutocompleteHint_ = nullptr;
    wxStaticText* txiSummary_ = nullptr;
    TxiDiagnosticsPanel* txiIssues_ = nullptr;
    TxiDictionaryPanel* catalog_ = nullptr;
    wxMenu* recentFilesMenu_ = nullptr;
    wxMenuItem* undoMenuItem_ = nullptr;
    wxMenuItem* redoMenuItem_ = nullptr;
    wxMenuItem* darkModeItem_ = nullptr;
    wxTimer animationTimer_;
    bool darkMode_ = false;
    bool loading_ = false;
};

bool TextureDropTarget::OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) {
    if (owner_ == nullptr || filenames.IsEmpty()) return false;
    return owner_->openDropped(filenames);
}

} // namespace

namespace ui {
TextureEditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context) {
    return new TextureEditorPanelImpl(parent, std::move(context));
}
} // namespace ui

} // namespace neotpc
