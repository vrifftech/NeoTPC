#include "BatchDialog.hpp"
#if defined(__EMSCRIPTEN__)
#include "BrowserWorkload.hpp"
#endif
#include "EncodingOptionsPanel.hpp"
#include "PathUtils.hpp"
#include "TextureCanvas.hpp"
#include "TextureTask.hpp"
#include "TxiEditor.hpp"
#include "core/TextureDocument.hpp"
#include "texture/FileUtil.hpp"
#include "texture/ParserLimits.hpp"
#include "texture/Txi.hpp"
#include "NeoWxUi.hpp"
#include "NeoGameDirectoryMenu.hpp"

#include <wx/aboutdlg.h>
#include <wx/artprov.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/checkbox.h>
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
#include <wx/toolbar.h>
#include <wx/wx.h>

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
    ID_PREVIOUS_FRAME, ID_NEXT_FRAME,
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
        optionsPanel_ = new EncodingOptionsPanel(this);
        optionsPanel_->setOptions(initialOptions);
        root->Add(optionsPanel_, 1, wxEXPAND | wxALL, FromDIP(10));
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

class MainFrame;

class TextureDropTarget final : public wxFileDropTarget {
public:
    explicit TextureDropTarget(MainFrame* owner) : owner_(owner) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override;
private:
    MainFrame* owner_ = nullptr;
};

class MainFrame final : public wxFrame {
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, kAppName, wxDefaultPosition, wxDefaultSize),
          settings_(kAppName), animationTimer_(this, ID_ANIMATION_TIMER), darkMode_(settings_.darkMode()) {
        SetIcon(makeAppIcon());
        buildMenus();
        buildInterface();
        wxui::configureResponsiveWindow(*this, wxSize(1280, 820), wxSize(720, 480));
        settings_.restoreWindowPlacement(*this);
        SetDropTarget(new TextureDropTarget(this));
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::onCloseWindow, this);
        Bind(wxEVT_TIMER, &MainFrame::onAnimationTimer, this, ID_ANIMATION_TIMER);
        wxui::applyTheme(this, darkMode_);
        txiEditor_->applyTheme(darkMode_);
        applyTxiHintTheme();
        canvas_->setDarkMode(darkMode_);
        refreshCatalog();
        updateWindowState();
        wxui::setStatusText(*this, "Ready - open or drop a texture", 0);
    }

    ~MainFrame() override {
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, false);
#endif
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
            document_=std::move(opened); staged_.reset();
            comparisonImages_.clear();
            rebuildComparisonWorkspace();
            refreshDocumentUi();
            settings_.addRecentFile(path);
            refreshRecentFiles();
            wxui::setStatusText(*this, wxString("Opened ") + wxpath::toWx(path), 0);
            return true;
        } catch (const texture::OperationCancelled&) {
            wxui::setStatusText(*this,"Open cancelled; previous document retained",0); return false;
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
                BatchDialog dialog(this,name,darkMode_);dialog.ShowModal();return true;
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
        file->Append(wxID_OPEN, "&Open...\tCtrl+O");
        recentFilesMenu_ = new wxMenu();
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->Append(ID_OPEN_CONFLICTS, "Open &Conflicting Images\tCtrl+Shift+O");
        file->Append(ID_CLOSE_CONFLICTS, "Close Image Comparison");
        file->Append(wxID_SAVE, "&Save\tCtrl+S");
        file->Append(ID_SAVE_AS, "Save &As (preserve format)...\tCtrl+Shift+S");
        file->AppendSeparator();
        file->Append(ID_SPLIT_TPC, "Split TPC into TGA + TXI...");
        file->Append(ID_COMBINE_TGA_TXI, "Combine TGA + TXI into TPC...");
        file->AppendSeparator();
        file->Append(ID_IMPORT_TXI, "Import TXI...");
        file->Append(ID_EXPORT_TXI, "Export TXI...");
        file->AppendSeparator();
        file->Append(ID_BATCH_CONVERT, "&Batch Convert...");
        file->AppendSeparator();
        file->Append(ID_CLOSE_TEXTURE, "&Close Texture\tCtrl+W");
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file, [this](const std::filesystem::path& directory) {
                chooseOpen(directory);
            });
        file->AppendSeparator();
        file->Append(wxID_EXIT, "E&xit\tAlt+F4");
        file->Insert(3,ID_EXPORT_IMAGE,"Export / Convert...\tCtrl+E");
        menuBar->Append(file, "&File");
        auto* edit=new wxMenu();
        edit->Append(wxID_UNDO,"Undo texture change\tCtrl+Z");
        edit->Append(wxID_REDO,"Redo texture change\tCtrl+Y");
        menuBar->Append(edit,"&Edit");

        auto* view = new wxMenu();
        view->Append(ID_FIT_IMAGE, "&Fit to Window\tCtrl+0");
        view->Append(ID_ACTUAL_SIZE, "&Actual Pixels (100%)\tCtrl+1");
        view->Append(ID_ZOOM_IN, "Zoom &In\tCtrl++");
        view->Append(ID_ZOOM_OUT, "Zoom &Out\tCtrl+-");
        view->AppendSeparator();
        view->AppendCheckItem(ID_DARK_MODE, "&Dark Mode")->Check(darkMode_);
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
        help->Append(wxID_ABOUT, "&About NeoTPC");
        menuBar->Append(help, "&Help");
        SetMenuBar(menuBar);

        auto* toolbar = CreateToolBar(wxTB_FLAT | wxTB_HORIZONTAL | wxTB_NODIVIDER);
        toolbar->AddTool(wxID_OPEN, "Open", wxArtProvider::GetBitmap(wxART_FILE_OPEN, wxART_TOOLBAR));
        toolbar->AddTool(wxID_SAVE, "Save", wxArtProvider::GetBitmap(wxART_FILE_SAVE, wxART_TOOLBAR));
        toolbar->AddSeparator();
        toolbar->AddTool(ID_FIT_IMAGE, "Fit", wxArtProvider::GetBitmap(wxART_FIND, wxART_TOOLBAR));
        toolbar->Realize();

        Bind(wxEVT_MENU, [this](wxCommandEvent&) { chooseOpen(); }, wxID_OPEN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { save(); }, wxID_SAVE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { saveAs(); }, ID_SAVE_AS);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { exportImage(); }, ID_EXPORT_IMAGE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { undoRedo(false); }, wxID_UNDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { undoRedo(true); }, wxID_REDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { splitTpc(); }, ID_SPLIT_TPC);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { combineTgaTxi(); }, ID_COMBINE_TGA_TXI);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { importTxi(); }, ID_IMPORT_TXI);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { exportTxi(); }, ID_EXPORT_TXI);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showBatch(); }, ID_BATCH_CONVERT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { openConflictingImages(); }, ID_OPEN_CONFLICTS);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { closeImageComparison(); }, ID_CLOSE_CONFLICTS);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { closeTexture(); }, ID_CLOSE_TEXTURE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_MENU, &MainFrame::onOpenRecent, this, ID_RECENT_FIRST, ID_RECENT_LAST);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            settings_.clearRecentFiles();
            refreshRecentFiles();
        }, ID_CLEAR_RECENT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.fitImage(); }); refreshStatus(); }, ID_FIT_IMAGE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.actualSize(); }); refreshStatus(); }, ID_ACTUAL_SIZE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.zoomIn(); }); refreshStatus(); }, ID_ZOOM_IN);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.zoomOut(); }); refreshStatus(); }, ID_ZOOM_OUT);
        Bind(wxEVT_MENU, &MainFrame::toggleDarkMode, this, ID_DARK_MODE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setAlpha(); }, ID_SET_ALPHA);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { scaleAlpha(); }, ID_SCALE_ALPHA);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { invertAlpha(); }, ID_INVERT_ALPHA);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { flipHorizontal(); }, ID_FLIP_HORIZONTAL);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { flipVertical(); }, ID_FLIP_VERTICAL);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showAbout(); }, wxID_ABOUT);
        Bind(wxEVT_TOOL, [this](wxCommandEvent&) { chooseOpen(); }, wxID_OPEN);
        Bind(wxEVT_TOOL, [this](wxCommandEvent&) { save(); }, wxID_SAVE);
        Bind(wxEVT_TOOL, [this](wxCommandEvent&) { applyToPreviewCanvases([](auto& canvas) { canvas.fitImage(); }); refreshStatus(); }, ID_FIT_IMAGE);
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
        notebook_->SetMinSize(FromDIP(wxSize(390, 400)));
        buildTexturePage();
        buildTxiPage();
        buildCatalogPage();
        rebuildComparisonWorkspace();
        splitter->SplitVertically(previewHost_, notebook_, -FromDIP(430));
        root->Add(splitter, 1, wxEXPAND);
        SetSizer(root);

        auto* status = wxui::createStatusBar(*this, 2);
        const int widths[2] = {-1, FromDIP(280)};
        status->SetStatusWidths(2, widths);
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
        auto* label = new wxStaticText(panel, wxID_ANY, wxEmptyString);
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
            wxui::setStatusText(*this,wxString::Format("Pixel %d, %d | RGBA %u, %u, %u, %u",x,y,static_cast<unsigned>(rgba[offset]),static_cast<unsigned>(rgba[offset+1]),static_cast<unsigned>(rgba[offset+2]),static_cast<unsigned>(rgba[offset+3])),0);
        });
        sizer->Add(label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(6));
        sizer->Add(canvas, 1, wxEXPAND);
        panel->SetSizer(sizer);

        if (paneIndex == 0) {
            canvas_ = canvas;
        } else {
            auto& comparison = comparisonImages_[paneIndex - 1];
            comparison.canvas = canvas;
            comparison.label = label;
        }

        const auto path = comparisonPath(paneIndex);
        const auto* texture = comparisonTexture(paneIndex);
        wxString title = paneIndex == 0 ? "Original / current document | " :
            (comparisonImages_[paneIndex-1].encodedPreview ? "Encoded output (not saved) | " : "Comparison (read-only) | ");
        title += path.empty() ? wxString("No texture open") : wxpath::toWx(path.filename());
        if (texture != nullptr) {
            title += " | ";
            title += wxui::toWx(neotpc::texture::textureFileKindToString(texture->kind));
            if (texture->hasPixels()) {
                title += wxString::Format(" | %u x %u | %llu layer(s)",
                    texture->layers.front().width, texture->layers.front().height,
                    static_cast<unsigned long long>(texture->layers.size()));
            }
        }
        label->SetLabel(title);
        if (!path.empty()) label->SetToolTip(wxpath::toWx(path));
        canvas->clearImage(texture != nullptr && !texture->hasPixels() ? "No pixel data" : "Open or drop a texture to begin");
        return panel;
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
        if (previewHostSizer_ == nullptr) return;
        const auto retainedView=canvas_ ? canvas_->view() : TextureCanvas::View{};
        canvas_ = nullptr;
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
        auto* page = new wxScrolledWindow(notebook_, wxID_ANY);
        page->SetScrollRate(0, FromDIP(12));
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* previewBox = new wxStaticBoxSizer(wxVERTICAL, page, "Preview");
        auto* previewGrid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(8));
        previewGrid->AddGrowableCol(1, 1);
        previewGrid->Add(new wxStaticText(page, wxID_ANY, "Layer / frame"), 0, wxALIGN_CENTER_VERTICAL);
        auto* layerRow = new wxBoxSizer(wxHORIZONTAL);
        layerChoice_ = new wxChoice(page, wxID_ANY);
        playButton_ = new wxToggleButton(page, ID_PLAY, "Play");
        layerRow->Add(layerChoice_, 1, wxRIGHT, FromDIP(6));
        layerRow->Add(new wxButton(page,ID_PREVIOUS_FRAME,"<",wxDefaultPosition,FromDIP(wxSize(30,-1))),0,wxRIGHT,FromDIP(3));
        layerRow->Add(playButton_);
        layerRow->Add(new wxButton(page,ID_NEXT_FRAME,">",wxDefaultPosition,FromDIP(wxSize(30,-1))),0,wxLEFT,FromDIP(3));
        previewGrid->Add(layerRow, 1, wxEXPAND);
        previewGrid->Add(new wxStaticText(page, wxID_ANY, "Mipmap"), 0, wxALIGN_CENTER_VERTICAL);
        mipChoice_ = new wxChoice(page, wxID_ANY);
        previewGrid->Add(mipChoice_, 1, wxEXPAND);
        previewGrid->Add(new wxStaticText(page, wxID_ANY, "Channel view"), 0, wxALIGN_CENTER_VERTICAL);
        previewMode_ = new wxChoice(page, wxID_ANY);
        for (const char* label : {"RGBA over checkerboard", "RGB (opaque)", "Alpha (grayscale)", "Alpha (red mask)"}) {
            previewMode_->Append(label);
        }
        previewMode_->SetSelection(0);
        previewGrid->Add(previewMode_, 1, wxEXPAND);
        previewGrid->Add(new wxStaticText(page,wxID_ANY,"Display filter"),0,wxALIGN_CENTER_VERTICAL);
        samplingChoice_=new wxChoice(page,wxID_ANY);samplingChoice_->Append("Nearest (pixel inspection)");samplingChoice_->Append("Smooth (display only)");samplingChoice_->SetSelection(0);
        previewGrid->Add(samplingChoice_,1,wxEXPAND);
        previewGrid->Add(new wxStaticText(page,wxID_ANY,"Grid"),0,wxALIGN_CENTER_VERTICAL);
        gridChoice_=new wxChoice(page,wxID_ANY);gridChoice_->Append("None");gridChoice_->Append("Pixels");gridChoice_->Append("4 x 4 compression blocks");gridChoice_->SetSelection(0);previewGrid->Add(gridChoice_,1,wxEXPAND);
        samplingChoice_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){applyToPreviewCanvases([this](auto& c){c.setSmooth(samplingChoice_->GetSelection()==1);});});
        gridChoice_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){applyToPreviewCanvases([this](auto& c){c.setGrid(static_cast<TextureCanvas::Grid>(gridChoice_->GetSelection()));});});
        previewBox->Add(previewGrid, 1, wxEXPAND | wxALL, FromDIP(8));
        conflictButton_ = new wxButton(page, ID_OPEN_CONFLICTS, "Open conflicting images");
        conflictButton_->SetToolTip("Find same-folder texture files whose base name differs only by case, numbered duplicate suffixes, copy suffixes, or image extension.");
        previewBox->Add(conflictButton_, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        root->Add(previewBox, 0, wxEXPAND | wxALL, FromDIP(8));

        summary_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(-1, 205)),
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        root->Add(summary_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* outputRow=new wxBoxSizer(wxHORIZONTAL);
        outputRow->Add(new wxStaticText(page,wxID_ANY,"Export format"),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
        outputFormat_=new wxChoice(page,wxID_ANY);
        for(const char* value:{"tpc","tga","dds","png","jpg","bmp","txi"})outputFormat_->Append(value);
        outputFormat_->SetSelection(0);outputRow->Add(outputFormat_,1,wxEXPAND);
        root->Add(outputRow,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        outputFormat_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){onOptionsChanged();});
        optionsPanel_ = new EncodingOptionsPanel(page);
        root->Add(optionsPanel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* exportButtons=new wxBoxSizer(wxHORIZONTAL);
        exportButtons->Add(new wxButton(page,ID_ENCODE_PREVIEW,"Preview encoded output"),1,wxRIGHT,FromDIP(5));
        exportButtons->Add(new wxButton(page,ID_EXPORT_IMAGE,"Export..."),0);
        root->Add(exportButtons,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        encodingSummary_=new wxStaticText(page,wxID_ANY,"Export settings do not change the open document until applied.");
        encodingSummary_->Wrap(FromDIP(380));root->Add(encodingSummary_,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        root->Add(new wxButton(page,ID_APPLY_ENCODING,"Apply settings to this document..."),0,wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){buildEncodedPreview();},ID_ENCODE_PREVIEW);
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){exportImage();},ID_EXPORT_IMAGE);
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){applyEncoding();},ID_APPLY_ENCODING);
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){stepFrame(-1);},ID_PREVIOUS_FRAME);
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){stepFrame(1);},ID_NEXT_FRAME);
        auto* transforms = new wxStaticBoxSizer(wxHORIZONTAL, page, "Pixel operations (undoable)");
        transforms->Add(new wxButton(page, ID_SET_ALPHA, "Set alpha..."), 0, wxALL, FromDIP(5));
        transforms->Add(new wxButton(page, ID_INVERT_ALPHA, "Invert alpha"), 0, wxTOP | wxBOTTOM | wxRIGHT, FromDIP(5));
        transforms->Add(new wxButton(page, ID_FLIP_HORIZONTAL, "Flip X"), 0, wxTOP | wxBOTTOM | wxRIGHT, FromDIP(5));
        transforms->Add(new wxButton(page, ID_FLIP_VERTICAL, "Flip Y"), 0, wxTOP | wxBOTTOM | wxRIGHT, FromDIP(5));
        root->Add(transforms, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        root->AddStretchSpacer();
        page->SetSizer(root);
        page->FitInside();
        notebook_->AddPage(page, "Texture", true);

        layerChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { setAnimationPlaying(false); refreshMipmapChoices(); refreshPreview(); });
        mipChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refreshPreview(); });
        previewMode_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refreshPreview(); });
        playButton_->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) { setAnimationPlaying(playButton_->GetValue()); });
        optionsPanel_->setChangeHandler([this]() { onOptionsChanged(); });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { setAlpha(); }, ID_SET_ALPHA);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { invertAlpha(); }, ID_INVERT_ALPHA);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { flipHorizontal(); }, ID_FLIP_HORIZONTAL);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { flipVertical(); }, ID_FLIP_VERTICAL);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { openConflictingImages(); }, ID_OPEN_CONFLICTS);
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
        auto* page = new wxPanel(notebook_, wxID_ANY);
        auto* root = new wxBoxSizer(wxVERTICAL);
        txiHelp_ = new wxStaticText(page, wxID_ANY,
            "TXI controls Odyssey material, animation, cube-map, font, and procedural behavior. Start typing a "
            "directive to see a muted completion and press Tab to accept it. Once the directive is complete, the "
            "same hint shows its value type or range. Ctrl+Space opens all matching dictionary entries.");
        txiHelp_->Wrap(FromDIP(520));
        root->Add(txiHelp_, 0, wxEXPAND | wxALL, FromDIP(8));
        txiEditor_ = new TxiEditor(page);
        root->Add(txiEditor_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        txiAutocompleteHint_ = new wxStaticText(
            page, wxID_ANY, "Start typing a TXI directive. Tab accepts a unique completion.");
        txiAutocompleteHint_->Wrap(FromDIP(680));
        root->Add(txiAutocompleteHint_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        auto* txiButtons = new wxBoxSizer(wxHORIZONTAL);
        txiButtons->Add(new wxButton(page, ID_IMPORT_TXI, "Import TXI..."), 0, wxRIGHT, FromDIP(6));
        txiButtons->Add(new wxButton(page, ID_EXPORT_TXI, "Export TXI..."));
        txiButtons->Add(new wxButton(page,ID_COMMON_TXI,"Common values..."),0,wxLEFT,FromDIP(6));
        Bind(wxEVT_BUTTON,[this](wxCommandEvent&){editCommonTxi();},ID_COMMON_TXI);
        txiButtons->AddStretchSpacer();
        root->Add(txiButtons, 0, wxEXPAND | wxALL, FromDIP(8));
        txiSummary_ = new wxStaticText(page, wxID_ANY, "No TXI metadata");
        root->Add(txiSummary_, 0, wxEXPAND | wxALL, FromDIP(8));
        txiIssues_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 210)),
                                    wxLC_REPORT | wxLC_SINGLE_SEL);
        wxui::setColumns(*txiIssues_, {{"Severity", 75}, {"Line", 55}, {"Key", 120}, {"Message", 420}});
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
            txiAutocompleteHint_->Wrap(FromDIP(680));
            txiAutocompleteHint_->GetParent()->Layout();
        });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { importTxi(); }, ID_IMPORT_TXI);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { exportTxi(); }, ID_EXPORT_TXI);
        txiIssues_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &MainFrame::onTxiIssueActivated, this);
    }

    void buildCatalogPage() {
        auto* page = new wxPanel(notebook_, wxID_ANY);
        auto* root = new wxBoxSizer(wxVERTICAL);
        catalogFilter_ = new wxTextCtrl(page, wxID_ANY);
        catalogFilter_->SetHint("Filter TXI directives, value types, defaults, or behavior...");
        catalog_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        root->Add(catalogFilter_, 0, wxEXPAND | wxALL, FromDIP(8));
        root->Add(catalog_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        page->SetSizer(root);
        notebook_->AddPage(page, "TXI dictionary", false);
        catalogFilter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refreshCatalog(); });
    }

    void chooseOpen(const std::filesystem::path& initialDirectory = {}) {
        if(busy_) return;
        wxFileDialog dialog(this, "Open texture", wxpath::toWx(initialDirectory), wxEmptyString, openWildcard(),
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) openPath(wxpath::fromWx(dialog.GetPath()));
    }

    bool save() {
        if(busy_ || !document_.isOpen())return false;
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto exportPrefs=optionsPanel_->options();const int outputSelection=outputFormat_->GetSelection();
            runTextureTask(this,"Save texture",[&](TextureTaskProgress&){document_.save();});
            discardEncodedPreview();refreshDocumentUi();
            optionsPanel_->setOptions(exportPrefs);outputFormat_->SetSelection(outputSelection);updateExportTarget();
            wxui::setStatusText(*this,"Saved original format; export preferences were not applied",0);return true;
        } catch(const texture::OperationCancelled&) {wxui::setStatusText(*this,"Save cancelled; destination unchanged",0);return false;}
          catch(const std::exception& error){wxui::showError(this,error);return false;}
    }

    bool confirmSidecarReplacement(const fs::path& output) {
        if(texture::canonicalPathKey(output)==texture::canonicalPathKey(document_.path()) || !texture::usesTxiSidecar(output))return true;
        try {
            const auto sidecar=texture::findTxiSidecar(output);
            return !sidecar || wxui::confirm(this,"Replace output metadata?",
                wxString("The existing TXI will be replaced or removed to match the new image:\n\n")+wxpath::toWx(*sidecar));
        }catch(const std::exception& error){wxui::showError(this,error);return false;}
    }
    bool saveAs() {
        if(busy_ || !document_.isOpen())return false;
        const auto ext=texture::extensionLower(document_.path());
        const auto filter=wxui::toWx("Preserve source (*."+ext+")|*."+ext);
        wxFileDialog dialog(this,"Save As - preserve source format",wxpath::toWx(document_.path().parent_path()),
            wxpath::toWx(document_.path().filename()),filter,wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(dialog.ShowModal()!=wxID_OK)return false;
        auto output=wxpath::fromWx(dialog.GetPath());if(output.extension().empty())output+="."+ext;
        if(texture::extensionLower(output)!=ext){wxMessageBox("Use Export / Convert to select a different format.","Preserve source format",wxOK|wxICON_INFORMATION,this);return false;}
        if(!confirmSidecarReplacement(output))return false;
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            runTextureTask(this,"Save copy",[&](TextureTaskProgress&){document_.saveAs(output);});
            staged_.reset();comparisonImages_.clear();rebuildComparisonWorkspace();refreshDocumentUi();
            settings_.addRecentFile(output);refreshRecentFiles();return true;
        } catch(const texture::OperationCancelled&) {wxui::setStatusText(*this,"Save As cancelled",0);return false;}
          catch(const std::exception& error){wxui::showError(this,error);return false;}
    }

    fs::path proposedOutput() const {
        auto output=document_.path();output.replace_extension("."+wxui::toStd(outputFormat_->GetStringSelection()));return output;
    }
    void updateExportTarget() {
        if(!document_.isOpen())return;
        optionsPanel_->setTarget(texture::kindForExtension(proposedOutput()),document_.texture().ddsDialect,document_.texture().sourceMipMapCount>1);
    }
    void discardEncodedPreview() {
        staged_.reset();
        const auto old=comparisonImages_.size();
        comparisonImages_.erase(std::remove_if(comparisonImages_.begin(),comparisonImages_.end(),[](const auto& image){return image.encodedPreview;}),comparisonImages_.end());
        if(comparisonImages_.size()!=old){rebuildComparisonWorkspace();refreshMipmapChoices();refreshPreview();}
        if(encodingSummary_)encodingSummary_->SetLabel("Settings are for Export. Preview again after changing pixels, TXI or settings.");
    }
    bool buildEncodedPreview() {
        if(busy_ || !document_.isOpen())return false;
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto output=proposedOutput();const auto options=optionsPanel_->options();
            auto result=runTextureTask(this,"Encode proposed output",[&](TextureTaskProgress&) {return document_.preview(output,options);});
            discardEncodedPreview();staged_=std::make_unique<TexturePreview>(std::move(result));
            comparisonImages_.push_back(ComparisonImage{output,{},nullptr,nullptr,true});
            rebuildComparisonWorkspace();refreshMipmapChoices();refreshPreview();
            std::string info=texture::textureSummary(staged_->decoded)+"\nEncoded image: "+std::to_string(staged_->encoded.image.size())+" bytes";
            if(staged_->encoded.sidecar)info+="; TXI: "+std::to_string(staged_->encoded.sidecar->size())+" bytes";
            info+=staged_->pixelsPreserved ? "\nOriginal pixel representation preserved." : "\nRe-encoded output; compare pixels before exporting.";
            encodingSummary_->SetLabel(wxui::toWx(info));encodingSummary_->Wrap(FromDIP(380));encodingSummary_->GetParent()->Layout();
            if(auto* scroll=wxDynamicCast(encodingSummary_->GetParent(),wxScrolledWindow))scroll->FitInside();
            updateWindowState();return true;
        } catch(const texture::OperationCancelled&) {wxui::setStatusText(*this,"Encoded preview cancelled; no output written",0);return false;}
          catch(const std::exception& error){discardEncodedPreview();encodingSummary_->SetLabel(wxui::toWx(error.what()));encodingSummary_->Wrap(FromDIP(380));wxui::showError(this,error);return false;}
    }
    void exportImage() {
        if(busy_ || !document_.isOpen())return;
        if(!staged_ && !buildEncodedPreview())return;
        const auto suggested=proposedOutput();const auto ext=texture::extensionLower(suggested);
        wxFileDialog dialog(this,"Export encoded result (keeps document open)",wxpath::toWx(suggested.parent_path()),wxpath::toWx(suggested.filename()),
            wxui::toWx("Encoded output (*."+ext+")|*."+ext),wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(dialog.ShowModal()!=wxID_OK)return;
        auto output=wxpath::fromWx(dialog.GetPath());if(output.extension().empty())output+="."+ext;
        if(texture::kindForExtension(output)!=staged_->outputKind){wxMessageBox("Select the output format before previewing, then export with that extension.","Output format changed",wxOK|wxICON_INFORMATION,this);return;}
        if(texture::canonicalPathKey(output)==texture::canonicalPathKey(document_.path())) {
            wxMessageBox("Choose a different output name. To deliberately re-encode the open file, use Apply settings to this document, then Save. Undo is retained.","Protect open document",wxOK|wxICON_INFORMATION,this);return;
        }
        if(!confirmSidecarReplacement(output))return;
        if(staged_->outputKind==texture::TextureFileKind::Jpeg && document_.texture().hasAlpha &&
            !wxui::confirm(this,"JPEG discards alpha","The encoded preview has no transparency. Export it?"))return;
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            runTextureTask(this,"Write encoded output",[&](TextureTaskProgress&){document_.commitPreview(output,*staged_,false);});
            wxui::setStatusText(*this,wxString("Exported exactly the previewed bytes: ")+wxpath::toWx(output),0);
        } catch(const texture::OperationCancelled&){wxui::setStatusText(*this,"Export cancelled",0);}
          catch(const std::exception& error){wxui::showError(this,error);}
    }
    void applyEncoding() {
        if(busy_ || !document_.isOpen())return;
        if(texture::kindForExtension(proposedOutput())!=document_.texture().kind){wxMessageBox("Apply is only for this document's current format. Use Export for another format.","Apply encoding",wxOK|wxICON_INFORMATION,this);return;}
        if(!wxui::confirm(this,"Apply encoding to this document?","These settings may recompress pixels on the next Save. This change can be undone. Nothing is written now."))return;
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
        if(busy_ || !document_.isOpen())return;
        const wxString choices[]={"Animation columns (numx)","Animation rows (numy)","Animation FPS (fps)","Animation type (proceduretype)","Environment map (envmaptexture)","Bump map (bumpmaptexture)"};
        const char* keys[]={"numx","numy","fps","proceduretype","envmaptexture","bumpmaptexture"};
        wxSingleChoiceDialog choice(this,"Edits preserve all other directives. Layout changes remain pending until validated by the encoder.","Common TXI values",6,choices);
        if(choice.ShowModal()!=wxID_OK)return;const auto key=std::string(keys[choice.GetSelection()]);std::string value;
        for(const auto& entry:texture::parseTxiEntries(document_.texture().txi))if(entry.key==key)value=entry.value;
        wxTextEntryDialog dialog(this,wxui::toWx(texture::txiDirectiveHint(key)),wxui::toWx(key),wxui::toWx(value));
        if(dialog.ShowModal()!=wxID_OK)return;
        document_.finishEditGroup();document_.setTxi(texture::setTxiValue(document_.texture().txi,key,wxui::toStd(dialog.GetValue())));document_.finishEditGroup();
        loading_=true;txiEditor_->setValue(wxui::toWx(document_.texture().txi));loading_=false;discardEncodedPreview();afterPixelChange();refreshTxiValidation();
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
            outputTxi=texture::findTxiSidecar(outputTga).value_or(fs::path(outputTga).replace_extension(".txi"));
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
                return texture::saveTgaTxiPair(document_.texture(), outputTga);
            });
            wxui::setStatusText(*this,
                wxString("Split to ") + wxpath::toWx(pair.tga.filename()) + " + " +
                    wxpath::toWx(pair.txi.filename()),
                0);
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

        neotpc::texture::TextureSaveOptions initialOptions;
        if (document_.isOpen()) initialOptions = document_.saveOptions();
        TpcEncodingDialog optionsDialog(this, initialOptions);
        wxui::applyTheme(&optionsDialog, darkMode_);
        if (optionsDialog.ShowModal() != wxID_OK) return;

        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto options=optionsDialog.options();
            runTextureTask(this,"Combine TGA and TXI",[&](TextureTaskProgress&){
                texture::combineTgaTxiToTpc(inputTga,inputTxi,outputTpc,options);
            });
            settings_.addRecentFile(outputTpc);
            refreshRecentFiles();
            wxui::setStatusText(*this, wxString("Combined TGA + TXI into ") + wxpath::toWx(outputTpc), 0);
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
            discardEncodedPreview();
            loading_ = true;
            txiEditor_->setValue(wxui::toWx(document_.texture().txi));
            loading_ = false;
            summary_->ChangeValue(wxui::toWx(document_.summary()));
            refreshTxiValidation();
            updateWindowState();
            wxui::setStatusText(*this, "Imported TXI metadata", 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    void exportTxi() {
        if (busy_ || !document_.isOpen()) return;
        auto suggested = document_.path().filename();
        suggested.replace_extension(".txi");
        wxFileDialog dialog(this, "Export TXI metadata", wxpath::toWx(document_.path().parent_path()),
                            wxpath::toWx(suggested), "TXI metadata (*.txi)|*.txi",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) return;
        auto output = wxpath::fromWx(dialog.GetPath());
#if !defined(__EMSCRIPTEN__)
        if (output.extension().empty()) output.replace_extension(".txi");
#endif
        try {
            TextureBusyGuard busy(busy_);
            runTextureTask(this,"Export TXI",[&](TextureTaskProgress&){texture::saveTexture(document_.texture(),output);});
            wxui::setStatusText(*this, wxString("Exported TXI to ") + wxpath::toWx(output), 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    bool maybeSave() {
        if(busy_)return false;
        if (!document_.isOpen() || !document_.dirty()) return true;
        wxMessageDialog dialog(this,
            wxString("Save changes to ") + wxpath::toWx(document_.path().filename()) + "?",
            "Unsaved texture changes", wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxICON_WARNING);
        const int result = dialog.ShowModal();
        if (result == wxID_CANCEL) return false;
        if (result == wxID_NO) return true;
        return save();
    }

    void closeTexture() {
        if (busy_ || !maybeSave()) return;
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, true);
#endif
        document_.close();staged_.reset();
        comparisonImages_.clear();
        rebuildComparisonWorkspace();
        refreshDocumentUi();
    }

    void showBatch() {
        if(busy_)return;
        setAnimationPlaying(false);
        const auto initial = document_.isOpen() ? wxpath::toWx(document_.path().parent_path()) : wxString{};
        BatchDialog dialog(this, initial, darkMode_);
        dialog.ShowModal();
    }

    void openConflictingImages() {
        if (busy_ || !document_.isOpen()) return;
        discardEncodedPreview();
#if defined(__EMSCRIPTEN__)
        requestBrowserComparisonFiles();
#else
        openConflictingImagesFromPaths(
            neotpc::texture::findConflictingTexturePaths(document_.path()));
#endif
    }

#if defined(__EMSCRIPTEN__)
    void requestBrowserComparisonFiles() {
        if (busy_ || browserComparison_) return;
        const std::uint64_t request = ++browserComparisonGeneration_;
        wxWeakRef<MainFrame> weak(this);
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
        wxWeakRef<MainFrame> weak(this);
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
        wxui::setStatusText(*this,
            wxString::Format("Comparison %llu/%llu: reading %s",
                             static_cast<unsigned long long>(browserComparison_->index + 1),
                             static_cast<unsigned long long>(browserComparison_->candidates.size()),
                             displayName.c_str()), 0);

        const std::uint64_t generation = browserComparison_->generation;
        wxWeakRef<MainFrame> weak(this);
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

        wxWeakRef<MainFrame> weak(this);
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
        wxWeakRef<MainFrame> weak(this);
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
        wxWeakRef<MainFrame> weak(this);
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
            wxui::setStatusText(*this,
                state->failures.empty() ? "No conflicting images found" :
                                          "No conflicting images could be previewed", 0);
        } else {
            wxui::setStatusText(*this,
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
        if (announce) wxui::setStatusText(*this, "Comparison loading cancelled", 0);
        if (refreshState) updateWindowState();
    }
#else
    void openConflictingImagesFromPaths(std::vector<fs::path> paths) {
        if(busy_ || !document_.isOpen()) return;
        struct Loaded {std::vector<ComparisonImage> images;std::vector<std::string> failures;bool cancelled=false;};
        try {
            setAnimationPlaying(false);TextureBusyGuard busy(busy_);
            const auto source=document_.path();
            auto result=runTextureTask(this,"Open comparisons",[&](TextureTaskProgress& progress){
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
            rebuildComparisonWorkspace();refreshMipmapChoices();refreshPreview();updateWindowState();Layout();
            if(!result.failures.empty()) {
                std::string details;
                for(const auto& failure:result.failures)details+=failure+'\n';
                wxMessageBox(wxui::toWx(details),"Comparison results",wxOK|wxICON_WARNING,this);
            }
            wxui::setStatusText(*this,result.cancelled?wxString("Comparison loading cancelled; completed images retained"):
                wxString::Format("%llu read-only comparison(s)",static_cast<unsigned long long>(comparisonImages_.size())),0);
        } catch(const texture::OperationCancelled&){wxui::setStatusText(*this,"Comparison loading cancelled",0);}
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
        staged_.reset();comparisonImages_.clear();
        rebuildComparisonWorkspace();
        refreshPreview();
        updateWindowState();
        Layout();
        wxui::setStatusText(*this, "Image comparison closed", 0);
    }

    void refreshDocumentUi() {
        setAnimationPlaying(false);
        loading_ = true;
        layerChoice_->Clear();
        mipChoice_->Clear();
        if (!document_.isOpen()) {
            summary_->ChangeValue(wxEmptyString);
            txiEditor_->setValue(wxEmptyString);
            txiIssues_->DeleteAllItems();
            txiSummary_->SetLabel("No TXI metadata");
            txiHelp_->SetLabel(
                "TXI controls Odyssey material, animation, cube-map, font, and procedural behavior. Open a texture "
                "to edit its embedded or sidecar metadata. Type a directive or press Ctrl+Space for TXI index suggestions.");
            txiHelp_->Wrap(FromDIP(380));
            canvas_->clearImage("Open or drop a texture to begin");
            playButton_->Enable(false);
            playButton_->Show(false);
            mipChoice_->Enable(false);
            loading_ = false;
            updateWindowState();
            return;
        }
        const auto& texture = document_.texture();
        refreshLayerChoices();
        refreshMipmapChoices();
        optionsPanel_->setOptions(document_.saveOptions());
        auto extension=texture::extensionLower(document_.path());if(extension=="txb")extension="tpc";if(extension=="jpeg"||extension=="jpe")extension="jpg";
        outputFormat_->SetStringSelection(wxui::toWx(extension));updateExportTarget();
        if (texture.kind == neotpc::texture::TextureFileKind::Tpc) {
            txiHelp_->SetLabel(
                "This TXI is embedded in the TPC. Edit it here and use Save. Type a directive or press Ctrl+Space "
                "for TXI index suggestions. When TXI is the only change, NeoTPC replaces only the metadata footer "
                "and preserves the encoded image and mipmaps byte-for-byte.");
        } else {
            txiHelp_->SetLabel(
                "This metadata is stored in a same-name TXI sidecar when the image format does not embed TXI. "
                "Type a directive or press Ctrl+Space for TXI index suggestions. Save or convert the texture to write it.");
        }
        txiHelp_->Wrap(FromDIP(380));
        txiEditor_->setValue(wxui::toWx(texture.txi));
        summary_->ChangeValue(wxui::toWx(document_.summary()));
        loading_ = false;
        refreshTxiValidation();
        refreshPreview();
        updateWindowState();
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
        for(int id:{ID_PREVIOUS_FRAME,ID_NEXT_FRAME})if(auto* button=FindWindow(id))button->Enable(count>1);
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
            if(pixels)canvas->setImage(makePreviewImage(*pixels,previewMode));
            else canvas->clearImage("Selected mip / frame is not present");
        }
        applyToPreviewCanvases([&](auto& c){c.setView(retainedView);});
        refreshStatus();
    }

    void refreshStatus() {
        if(!document_.isOpen() || !document_.texture().hasPixels()){wxui::setStatusText(*this,"Ready",1);return;}
        const auto layer=static_cast<std::size_t>(std::max(0,layerChoice_->GetSelection()));
        const auto mip=static_cast<std::size_t>(std::max(0,mipChoice_->GetSelection()));
        if(const auto* image=previewLayerAt(document_.texture(),mappedLayer(document_.texture(),layer),mip))
            wxui::setStatusText(*this,wxString::Format("%u x %u | layer %llu | mip %llu | %d%%",image->width,image->height,static_cast<unsigned long long>(layer+1),static_cast<unsigned long long>(mip),canvas_->zoomPercent()),1);
        else wxui::setStatusText(*this,"Mip / frame not present in original",1);
    }

    void refreshTxiValidation() {
        txiIssues_->DeleteAllItems();
        if (!document_.isOpen()) return;
        const auto issues = neotpc::texture::validateTxiText(document_.texture().txi);
        std::size_t errors = 0, warnings = 0, infos = 0;
        for (const auto& issue : issues) {
            if (issue.severity == neotpc::texture::TxiIssueSeverity::Error) ++errors;
            else if (issue.severity == neotpc::texture::TxiIssueSeverity::Warning) ++warnings;
            else ++infos;
            wxui::appendRow(*txiIssues_, {
                neotpc::texture::txiIssueSeverityToString(issue.severity),
                std::to_string(issue.lineNumber), issue.key, issue.message
            });
        }
        if (issues.empty()) {
            txiSummary_->SetLabel(document_.texture().txi.empty() ? "No TXI metadata" : "TXI validation passed");
        } else {
            txiSummary_->SetLabel(wxString::Format("%llu error(s), %llu warning(s), %llu information note(s)",
                static_cast<unsigned long long>(errors), static_cast<unsigned long long>(warnings),
                static_cast<unsigned long long>(infos)));
        }
    }

    void refreshCatalog() {
        const auto filter = catalogFilter_ ? wxui::toStd(catalogFilter_->GetValue()) : std::string{};
        catalog_->ChangeValue(wxui::toWx(filter.empty() ? neotpc::texture::txiKeyReferenceText(false)
                                                        : neotpc::texture::txiKeyReferenceText(filter)));
    }

    void onOptionsChanged() {
        if (busy_ || loading_ || !document_.isOpen()) return;
        discardEncodedPreview();updateExportTarget();
        summary_->ChangeValue(wxui::toWx(document_.summary()));
        updateWindowState();
    }

    void onTxiChanged() {
        if (busy_ || loading_ || !document_.isOpen()) return;
        document_.setTxi(wxui::toStd(txiEditor_->value()));
        discardEncodedPreview();
        summary_->ChangeValue(wxui::toWx(document_.summary()));
        refreshTxiValidation();
        updateWindowState();
    }

    void onTxiIssueActivated(wxListEvent& event) {
        const wxString lineText = txiIssues_->GetItemText(event.GetIndex(), 1);
        long line = 0;
        if (!lineText.ToLong(&line) || line < 1) return;
        txiEditor_->goToOneBasedLine(static_cast<std::size_t>(line));
    }

    void updateWindowState() {
        const bool open = document_.isOpen();
        GetMenuBar()->Enable(wxID_UNDO,open&&document_.canUndo());
        GetMenuBar()->Enable(wxID_REDO,open&&document_.canRedo());
        GetMenuBar()->Enable(ID_EXPORT_IMAGE,open);
        for(int id:{ID_ENCODE_PREVIEW,ID_EXPORT_IMAGE,ID_APPLY_ENCODING,ID_COMMON_TXI})if(auto* button=FindWindow(id))button->Enable(open);
        if(optionsPanel_)optionsPanel_->Enable(open);if(outputFormat_)outputFormat_->Enable(open);
        const bool pixels = open && document_.texture().hasPixels();
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
        SetTitle(name + " - NeoTPC");
        for (int id : {ID_SAVE_AS, ID_CLOSE_TEXTURE, ID_IMPORT_TXI, ID_EXPORT_TXI}) {
            GetMenuBar()->Enable(id, open);
        }
        GetMenuBar()->Enable(ID_OPEN_CONFLICTS, open && !comparisonLoading);
        GetMenuBar()->Enable(ID_SPLIT_TPC,
            open && pixels && document_.texture().kind == neotpc::texture::TextureFileKind::Tpc);
        GetMenuBar()->Enable(ID_COMBINE_TGA_TXI, true);
        GetMenuBar()->Enable(ID_CLOSE_CONFLICTS, comparisonLoading || !comparisonImages_.empty());
        GetMenuBar()->Enable(wxID_SAVE, open && document_.dirty());
        if (txiEditor_ != nullptr) txiEditor_->Enable(open);
        if (auto* importButton = FindWindow(ID_IMPORT_TXI)) importButton->Enable(open);
        if (auto* exportButton = FindWindow(ID_EXPORT_TXI)) exportButton->Enable(open);
        if (conflictButton_ != nullptr) conflictButton_->Enable(open && !comparisonLoading);
        for (int id : {ID_FIT_IMAGE, ID_ACTUAL_SIZE, ID_ZOOM_IN, ID_ZOOM_OUT,
                       ID_SET_ALPHA, ID_SCALE_ALPHA, ID_INVERT_ALPHA, ID_FLIP_HORIZONTAL, ID_FLIP_VERTICAL}) {
            GetMenuBar()->Enable(id, pixels);
        }
        if (auto* toolbar = GetToolBar()) {
            toolbar->EnableTool(wxID_SAVE, open && document_.dirty());
            toolbar->EnableTool(ID_FIT_IMAGE, pixels);
        }
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
        catch(const texture::OperationCancelled&){wxui::setStatusText(*this,"Pixel operation cancelled; document unchanged",0);}
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
        info.SetDescription("TPC/TXB/TGA/DDS texture viewer, conflict comparator, TXI inspector, and converter for the Neo tool suite.");
        info.SetIcon(makeAppIcon());
        wxAboutBox(info, this);
    }

    void onCloseWindow(wxCloseEvent& event) {
        if(busy_){if(event.CanVeto())event.Veto();return;}
        if (!event.CanVeto()) {
#if defined(__EMSCRIPTEN__)
            cancelBrowserComparisonLoad(false, true);
#endif
            settings_.saveWindowPlacement(*this);
            event.Skip();
            return;
        }
        if (!maybeSave()) {
            event.Veto();
            return;
        }
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, true);
#endif
        settings_.saveWindowPlacement(*this);
        event.Skip();
    }

    neosettings::AppSettings settings_;
    TextureDocument document_;
    std::unique_ptr<TexturePreview> staged_;
    wxChoice* outputFormat_=nullptr;
    wxChoice* samplingChoice_=nullptr;
    wxChoice* gridChoice_=nullptr;
    wxStaticText* encodingSummary_=nullptr;
    bool busy_=false,syncingView_=false;
    std::chrono::steady_clock::time_point animationStart_;
    int animationStartFrame_=0;double animationFps_=8;
    TextureCanvas* canvas_ = nullptr;
    wxPanel* previewHost_ = nullptr;
    wxBoxSizer* previewHostSizer_ = nullptr;
    std::vector<ComparisonImage> comparisonImages_;
#if defined(__EMSCRIPTEN__)
    std::unique_ptr<BrowserComparisonState> browserComparison_;
    std::uint64_t browserComparisonGeneration_ = 0;
#endif
    std::vector<std::pair<wxSplitterWindow*, bool>> comparisonSplitters_;
    wxNotebook* notebook_ = nullptr;
    wxChoice* layerChoice_ = nullptr;
    wxChoice* mipChoice_ = nullptr;
    wxToggleButton* playButton_ = nullptr;
    wxChoice* previewMode_ = nullptr;
    wxButton* conflictButton_ = nullptr;
    wxTextCtrl* summary_ = nullptr;
    EncodingOptionsPanel* optionsPanel_ = nullptr;
    wxStaticText* txiHelp_ = nullptr;
    TxiEditor* txiEditor_ = nullptr;
    wxStaticText* txiAutocompleteHint_ = nullptr;
    wxStaticText* txiSummary_ = nullptr;
    wxListCtrl* txiIssues_ = nullptr;
    wxTextCtrl* catalogFilter_ = nullptr;
    wxTextCtrl* catalog_ = nullptr;
    wxMenu* recentFilesMenu_ = nullptr;
    wxTimer animationTimer_;
    bool darkMode_ = false;
    bool loading_ = false;
};

bool TextureDropTarget::OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) {
    if (owner_ == nullptr || filenames.IsEmpty()) return false;
    return owner_->openDropped(filenames);
}

} // namespace

} // namespace neotpc

namespace {

class NeoTpcApp final : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        SetAppName(neotpc::kAppName);
        SetVendorName("Neo Tools");
        wxInitAllImageHandlers();
        auto* frame = new neotpc::MainFrame();
        frame->Show(true);
        if (argc > 1) {
            const fs::path path = neotpc::wxpath::fromWx(wxString(argv[1]));
            frame->CallAfter([frame, path]() { frame->openPath(path); });
        }
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoTpcApp);
