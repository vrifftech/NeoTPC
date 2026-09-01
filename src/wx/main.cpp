#include "BatchDialog.hpp"
#if defined(__EMSCRIPTEN__)
#include "BrowserWorkload.hpp"
#endif
#include "EncodingOptionsPanel.hpp"
#include "PathUtils.hpp"
#include "TextureCanvas.hpp"
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
};

const char* openWildcard() {
    return "Texture files (*.tpc;*.txb;*.tga;*.dds;*.png;*.jpg;*.jpeg;*.jpe;*.bmp;*.txi)|"
           "*.tpc;*.txb;*.tga;*.dds;*.png;*.jpg;*.jpeg;*.jpe;*.bmp;*.txi|"
           "Odyssey textures (*.tpc;*.txb;*.tga;*.dds;*.txi)|*.tpc;*.txb;*.tga;*.dds;*.txi|"
           "All files (*.*)|*.*";
}

const char* saveWildcard() {
    return "TPC texture (*.tpc)|*.tpc|Targa image (*.tga)|*.tga|DirectDraw Surface (*.dds)|*.dds|"
           "PNG image (*.png)|*.png|JPEG image (*.jpg)|*.jpg|Bitmap image (*.bmp)|*.bmp|"
           "TXI metadata (*.txi)|*.txi";
}

#if !defined(__EMSCRIPTEN__)
std::string extensionForFilter(int index) {
    static const char* extensions[] = {".tpc", ".tga", ".dds", ".png", ".jpg", ".bmp", ".txi"};
    if (index < 0 || index >= static_cast<int>(sizeof(extensions) / sizeof(extensions[0]))) return ".tpc";
    return extensions[index];
}
#endif

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
    layerIndex = std::min(layerIndex, texture.layers.size() - 1);
    const auto& layer = texture.layers[layerIndex];
    mipIndex = std::min(mipIndex, layer.mipmaps.size());
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
        if (path.empty() || !maybeSave()) return false;
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, true);
#endif
        try {
            wxBusyCursor busy;
            document_.open(path);
            comparisonImages_.clear();
            rebuildComparisonWorkspace();
            refreshDocumentUi();
            settings_.addRecentFile(path);
            refreshRecentFiles();
            wxui::setStatusText(*this, wxString("Opened ") + wxpath::toWx(path), 0);
            return true;
        } catch (const std::exception& error) {
            wxui::showError(this, error);
            return false;
        }
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
        file->Append(ID_SAVE_AS, "Save &As / Convert...\tCtrl+Shift+S");
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
        menuBar->Append(file, "&File");

        auto* view = new wxMenu();
        view->Append(ID_FIT_IMAGE, "&Fit to Window\tF");
        view->Append(ID_ACTUAL_SIZE, "&Actual Pixels (100%)\t1");
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
        return comparisonIndex < comparisonImages_.size() ? &comparisonImages_[comparisonIndex].texture : nullptr;
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
        wxString title = paneIndex == 0 ? "Current (editable) | " : "Conflict (read-only) | ";
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
        layerRow->Add(playButton_);
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
        previewBox->Add(previewGrid, 1, wxEXPAND | wxALL, FromDIP(8));
        conflictButton_ = new wxButton(page, ID_OPEN_CONFLICTS, "Open conflicting images");
        conflictButton_->SetToolTip("Find same-folder texture files whose base name differs only by case, numbered duplicate suffixes, copy suffixes, or image extension.");
        previewBox->Add(conflictButton_, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        root->Add(previewBox, 0, wxEXPAND | wxALL, FromDIP(8));

        summary_ = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(-1, 205)),
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        root->Add(summary_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        optionsPanel_ = new EncodingOptionsPanel(page);
        root->Add(optionsPanel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* transforms = new wxStaticBoxSizer(wxHORIZONTAL, page, "Pixel operations");
        transforms->Add(new wxButton(page, ID_SET_ALPHA, "Set alpha..."), 0, wxALL, FromDIP(5));
        transforms->Add(new wxButton(page, ID_INVERT_ALPHA, "Invert alpha"), 0, wxTOP | wxBOTTOM | wxRIGHT, FromDIP(5));
        transforms->Add(new wxButton(page, ID_FLIP_HORIZONTAL, "Flip X"), 0, wxTOP | wxBOTTOM | wxRIGHT, FromDIP(5));
        transforms->Add(new wxButton(page, ID_FLIP_VERTICAL, "Flip Y"), 0, wxTOP | wxBOTTOM | wxRIGHT, FromDIP(5));
        root->Add(transforms, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        root->AddStretchSpacer();
        page->SetSizer(root);
        page->FitInside();
        notebook_->AddPage(page, "Texture", true);

        layerChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refreshMipmapChoices(); refreshPreview(); });
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
        wxFileDialog dialog(this, "Open texture", wxpath::toWx(initialDirectory), wxEmptyString, openWildcard(),
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) openPath(wxpath::fromWx(dialog.GetPath()));
    }

    bool save() {
        if (!document_.isOpen()) return false;
        if (neotpc::texture::extensionLower(document_.path()) == "txb") return saveAs();
        document_.setSaveOptions(optionsPanel_->options());
        const bool patchedTxiOnly = document_.canPatchEmbeddedTxi();
        try {
            wxBusyCursor busy;
            document_.save();
            refreshDocumentUi();
            wxui::setStatusText(*this,
                (patchedTxiOnly ? wxString("Updated embedded TXI in ") : wxString("Saved ")) +
                    wxpath::toWx(document_.path()),
                0);
            return true;
        } catch (const std::exception& error) {
            wxui::showError(this, error);
            return false;
        }
    }

    bool saveAs() {
        if (!document_.isOpen()) return false;
        const auto path = document_.path();
        auto suggestedName = path.filename();
        if (neotpc::texture::extensionLower(suggestedName) == "txb") suggestedName.replace_extension(".tpc");
        wxFileDialog dialog(this, "Save / convert texture", wxpath::toWx(path.parent_path()),
                            wxpath::toWx(suggestedName), saveWildcard(), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) return false;
        auto output = wxpath::fromWx(dialog.GetPath());
#if !defined(__EMSCRIPTEN__)
        if (output.extension().empty()) output += extensionForFilter(dialog.GetFilterIndex());
#endif
        const auto extension = neotpc::texture::extensionLower(output);
        if ((extension == "jpg" || extension == "jpeg" || extension == "jpe") && document_.texture().hasAlpha &&
            !wxui::confirm(this, "JPEG discards alpha", "JPEG output cannot preserve transparency. Continue?")) {
            return false;
        }
        document_.setSaveOptions(optionsPanel_->options());
        try {
            wxBusyCursor busy;
            document_.saveAs(output);
            comparisonImages_.clear();
            rebuildComparisonWorkspace();
            refreshDocumentUi();
            settings_.addRecentFile(output);
            refreshRecentFiles();
            wxui::setStatusText(*this, wxString("Saved ") + wxpath::toWx(output), 0);
            return true;
        } catch (const std::exception& error) {
            wxui::showError(this, error);
            return false;
        }
    }

    void splitTpc() {
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
        auto outputTxi = outputTga;
        outputTxi.replace_extension(".txi");

        std::error_code ec;
        if (fs::exists(outputTxi, ec) &&
            !wxui::confirm(this, "Replace TXI sidecar",
                           "The matching TXI file already exists. Replace both output files?")) {
            return;
        }

        try {
            wxBusyCursor busy;
            const auto pair = neotpc::texture::saveTgaTxiPair(document_.texture(), outputTga);
            wxui::setStatusText(*this,
                wxString("Split to ") + wxpath::toWx(pair.tga.filename()) + " + " +
                    wxpath::toWx(pair.txi.filename()),
                0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    void combineTgaTxi() {
        wxFileDialog tgaDialog(this, "Select TGA image", wxEmptyString, wxEmptyString,
                               "Targa image (*.tga)|*.tga", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (tgaDialog.ShowModal() != wxID_OK) return;
        const auto inputTga = wxpath::fromWx(tgaDialog.GetPath());

        std::optional<fs::path> inputTxi;
        auto matchingTxi = inputTga;
        matchingTxi.replace_extension(".txi");
        std::error_code ec;
        if (fs::is_regular_file(matchingTxi, ec)) {
            inputTxi = matchingTxi;
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
            wxBusyCursor busy;
            neotpc::texture::combineTgaTxiToTpc(
                inputTga, inputTxi, outputTpc, optionsDialog.options());
            settings_.addRecentFile(outputTpc);
            refreshRecentFiles();
            wxui::setStatusText(*this, wxString("Combined TGA + TXI into ") + wxpath::toWx(outputTpc), 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    void importTxi() {
        if (!document_.isOpen()) return;
        wxFileDialog dialog(this, "Import TXI metadata", wxpath::toWx(document_.path().parent_path()),
                            wxEmptyString, "TXI metadata (*.txi)|*.txi",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        try {
            const auto imported = neotpc::texture::loadTexture(wxpath::fromWx(dialog.GetPath()));
            document_.setTxi(imported.txi);
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
        if (!document_.isOpen()) return;
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
            neotpc::texture::saveTexture(document_.texture(), output);
            wxui::setStatusText(*this, wxString("Exported TXI to ") + wxpath::toWx(output), 0);
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }

    bool maybeSave() {
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
        if (!maybeSave()) return;
#if defined(__EMSCRIPTEN__)
        cancelBrowserComparisonLoad(false, true);
#endif
        document_.close();
        comparisonImages_.clear();
        rebuildComparisonWorkspace();
        refreshDocumentUi();
    }

    void showBatch() {
        const auto initial = document_.isOpen() ? wxpath::toWx(document_.path().parent_path()) : wxString{};
        BatchDialog dialog(this, initial, darkMode_);
        dialog.ShowModal();
    }

    void openConflictingImages() {
        if (!document_.isOpen()) return;
#if defined(__EMSCRIPTEN__)
        requestBrowserComparisonFiles();
#else
        openConflictingImagesFromPaths(
            neotpc::texture::findConflictingTexturePaths(document_.path()));
#endif
    }

#if defined(__EMSCRIPTEN__)
    void requestBrowserComparisonFiles() {
        if (browserComparison_) return;
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
        try {
            std::vector<fs::path> candidates;
            candidates.reserve(paths.size());
            for (auto& path : paths) {
                std::error_code equivalentError;
                const bool sameFile = fs::equivalent(path, document_.path(), equivalentError);
                if ((!equivalentError && sameFile) || path == document_.path()) continue;
                candidates.push_back(std::move(path));
            }

            constexpr std::size_t kMaxComparisonImages = 64;
            constexpr std::uint64_t kMaxComparisonInputBytes = UINT64_C(128) * 1024 * 1024;
            constexpr std::uint64_t kMaxComparisonDecodedBytes = UINT64_C(256) * 1024 * 1024;
            const auto attemptCount = std::min(candidates.size(), kMaxComparisonImages);
            std::vector<ComparisonImage> loaded;
            std::vector<std::string> failures;
            std::uint64_t decodedBytes = 0;
            bool cancelled = false;
            {
                wxProgressDialog progress("Open Conflicting Images", "Preparing comparison images...",
                                          static_cast<int>(std::max<std::size_t>(1, attemptCount)), this,
                                          wxPD_CAN_ABORT | wxPD_APP_MODAL | wxPD_AUTO_HIDE |
                                          wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
                for (std::size_t index = 0; index < attemptCount; ++index) {
                    const auto displayName = wxpath::toWx(candidates[index].filename());
                    if (!progress.Update(static_cast<int>(index), wxString("Opening ") + displayName)) {
                        cancelled = true;
                        break;
                    }
                    if (decodedBytes >= kMaxComparisonDecodedBytes) {
                        failures.push_back(std::to_string(attemptCount - index) +
                                           " additional image(s) skipped because the comparison memory limit was reached");
                        break;
                    }

                    bool cancelRequested = false;
                    try {
                        const auto candidate = candidates[index];
                        const auto remainingDecodedBytes = kMaxComparisonDecodedBytes - decodedBytes;
                        auto future = std::async(std::launch::async,
                            [candidate, remainingDecodedBytes, inputByteLimit = kMaxComparisonInputBytes]() {
                                neotpc::texture::parser::ScopedResourceLimits limits(
                                    inputByteLimit, remainingDecodedBytes);
                                return neotpc::texture::loadTexture(candidate);
                            });

                        while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
                            if (!cancelRequested) {
                                if (!progress.Pulse(wxString("Opening ") + displayName)) cancelRequested = true;
                            } else {
                                wxYieldIfNeeded();
                            }
                        }

                        auto texture = future.get();
                        if (cancelRequested) {
                            cancelled = true;
                            break;
                        }
                        if (!texture.hasPixels()) {
                            failures.push_back(neotpc::texture::pathToUtf8(candidates[index].filename()) + ": no pixel data");
                            continue;
                        }
                        const auto imageBytes = decodedTextureBytes(texture);
                        if (imageBytes > remainingDecodedBytes) {
                            failures.push_back(neotpc::texture::pathToUtf8(candidates[index].filename()) +
                                               ": skipped because the comparison memory limit was reached");
                            continue;
                        }
                        decodedBytes += imageBytes;
                        loaded.push_back(ComparisonImage{candidates[index], std::move(texture), nullptr, nullptr});
                    } catch (const std::exception& error) {
                        if (cancelRequested) {
                            cancelled = true;
                            break;
                        }
                        failures.push_back(neotpc::texture::pathToUtf8(candidates[index].filename()) + ": " + error.what());
                    }
                }
                if (!cancelled && attemptCount != 0) {
                    progress.Update(static_cast<int>(attemptCount), "Comparison images loaded");
                } else {
                    progress.Hide();
                }
            }
            if (candidates.size() > attemptCount) {
                failures.push_back(std::to_string(candidates.size() - attemptCount) +
                                   " additional image(s) skipped at the 64-pane safety limit");
            }
            if (cancelled) failures.push_back("Comparison loading was cancelled");
            comparisonImages_ = std::move(loaded);
            rebuildComparisonWorkspace();
            refreshPreview();
            updateWindowState();
            Layout();

            if (comparisonImages_.empty() && failures.empty()) {
                wxMessageBox("No same-folder conflicting images were found for this base name.",
                             "Open Conflicting Images", wxOK | wxICON_INFORMATION, this);
            } else if (!failures.empty()) {
                std::string message = "Opened " + std::to_string(comparisonImages_.size()) +
                                      " conflicting image(s). The following file(s) could not be previewed:\n\n";
                for (const auto& failure : failures) message += failure + '\n';
                wxMessageBox(wxui::toWx(message), "Open Conflicting Images", wxOK | wxICON_WARNING, this);
            }
            if (comparisonImages_.empty()) {
                wxui::setStatusText(*this,
                    failures.empty() ? "No conflicting images found" :
                                       "No conflicting images could be previewed", 0);
            } else {
                wxui::setStatusText(*this,
                    wxString::Format("Comparison view: %llu image pane(s)",
                                     static_cast<unsigned long long>(comparisonImages_.size() + 1)), 0);
            }
        } catch (const std::exception& error) {
            wxui::showError(this, error);
        }
    }
#endif

    void closeImageComparison() {
#if defined(__EMSCRIPTEN__)
        const bool wasLoading = browserComparison_ != nullptr;
        cancelBrowserComparisonLoad(wasLoading, true);
        if (wasLoading && comparisonImages_.empty()) return;
#endif
        if (comparisonImages_.empty()) return;
        comparisonImages_.clear();
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
        for (std::size_t index = 0; index < texture.layers.size(); ++index) {
            const auto& layer = texture.layers[index];
            if (texture.cubeMap && texture.cubeFaces.size() == texture.layers.size()) {
                const auto faceName = wxui::toWx(neotpc::texture::cubeFaceToString(texture.cubeFaces[index]));
                layerChoice_->Append(wxString::Format("Cube face %s - %u x %u | %llu mip(s)",
                    faceName.c_str(),
                    layer.width, layer.height, static_cast<unsigned long long>(layer.mipmaps.size() + 1)));
            } else {
                wxString prefix = texture.cubeMap ? "Cube face" : texture.animated ? "Frame" : "Layer";
                layerChoice_->Append(wxString::Format("%s %llu - %u x %u | %llu mip(s)", prefix.c_str(),
                    static_cast<unsigned long long>(index + 1), layer.width, layer.height,
                    static_cast<unsigned long long>(layer.mipmaps.size() + 1)));
            }
        }
        if (layerChoice_->GetCount() != 0) layerChoice_->SetSelection(0);
        const bool multilayer = texture.layers.size() > 1;
        layerChoice_->Enable(multilayer);
        const bool playable = texture.animated && multilayer;
        playButton_->Enable(playable);
        playButton_->Show(playable);
        refreshMipmapChoices();
        optionsPanel_->setOptions(document_.saveOptions());
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

    void refreshMipmapChoices() {
        const int previous = mipChoice_->GetSelection();
        mipChoice_->Clear();
        if (!document_.isOpen() || !document_.texture().hasPixels()) {
            mipChoice_->Enable(false);
            return;
        }
        int layerSelection = layerChoice_->GetSelection();
        if (layerSelection == wxNOT_FOUND) layerSelection = 0;
        layerSelection = std::clamp(layerSelection, 0, static_cast<int>(document_.texture().layers.size() - 1));
        const auto& layer = document_.texture().layers[static_cast<std::size_t>(layerSelection)];
        mipChoice_->Append(wxString::Format("Level 0 (base) - %u x %u", layer.width, layer.height));
        for (std::size_t index = 0; index < layer.mipmaps.size(); ++index) {
            const auto& mip = layer.mipmaps[index];
            mipChoice_->Append(wxString::Format("Level %llu - %u x %u",
                static_cast<unsigned long long>(index + 1), mip.width, mip.height));
        }
        const int selection = previous == wxNOT_FOUND
            ? 0
            : std::clamp(previous, 0, static_cast<int>(mipChoice_->GetCount() - 1));
        mipChoice_->SetSelection(selection);
        mipChoice_->Enable(mipChoice_->GetCount() > 1);
    }

    void refreshPreview() {
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
        std::optional<neotpc::texture::CubeFace> selectedCubeFace;
        if (document_.texture().cubeMap &&
            document_.texture().cubeFaces.size() == document_.texture().layers.size() &&
            layerIndex < document_.texture().cubeFaces.size()) {
            selectedCubeFace = document_.texture().cubeFaces[layerIndex];
        }
        if (const auto* preview = previewLayerAt(document_.texture(), layerIndex, mipIndex)) {
            canvas_->setImage(makePreviewImage(*preview, previewMode));
        } else {
            canvas_->clearImage("TXI-only document - no pixel data");
        }
        for (auto& comparison : comparisonImages_) {
            if (comparison.canvas == nullptr) continue;
            std::size_t comparisonLayerIndex = layerIndex;
            if (selectedCubeFace && comparison.texture.cubeMap &&
                comparison.texture.cubeFaces.size() == comparison.texture.layers.size()) {
                const auto face = std::find(comparison.texture.cubeFaces.begin(),
                                            comparison.texture.cubeFaces.end(), *selectedCubeFace);
                if (face == comparison.texture.cubeFaces.end()) {
                    comparison.canvas->clearImage(
                        wxString("Cube face ") + wxui::toWx(neotpc::texture::cubeFaceToString(*selectedCubeFace)) +
                        " is not present");
                    continue;
                }
                comparisonLayerIndex = static_cast<std::size_t>(
                    std::distance(comparison.texture.cubeFaces.begin(), face));
            }
            if (const auto* preview = previewLayerAt(comparison.texture, comparisonLayerIndex, mipIndex)) {
                comparison.canvas->setImage(makePreviewImage(*preview, previewMode));
            } else {
                comparison.canvas->clearImage("No pixel data");
            }
        }
        refreshStatus();
    }

    void refreshStatus() {
        if (!document_.isOpen() || !document_.texture().hasPixels()) {
            wxui::setStatusText(*this, document_.isOpen() ? "TXI-only document" : "Ready", 1);
            return;
        }
        int selection = layerChoice_->GetSelection();
        if (selection == wxNOT_FOUND) selection = 0;
        const auto& layer = document_.texture().layers[static_cast<std::size_t>(selection)];
        int mipSelection = mipChoice_->GetSelection();
        if (mipSelection == wxNOT_FOUND) mipSelection = 0;
        mipSelection = std::clamp(mipSelection, 0, static_cast<int>(layer.mipmaps.size()));
        const auto& preview = mipSelection == 0 ? layer : layer.mipmaps[static_cast<std::size_t>(mipSelection - 1)];
        wxui::setStatusText(*this, wxString::Format("%u x %u | layer %d/%llu | mip %d/%llu | %d%%",
            preview.width, preview.height, selection + 1,
            static_cast<unsigned long long>(document_.texture().layers.size()), mipSelection,
            static_cast<unsigned long long>(layer.mipmaps.size()), canvas_->zoomPercent()), 1);
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
        if (loading_ || !document_.isOpen()) return;
        document_.setSaveOptions(optionsPanel_->options());
        summary_->ChangeValue(wxui::toWx(document_.summary()));
        updateWindowState();
    }

    void onTxiChanged() {
        if (loading_ || !document_.isOpen()) return;
        document_.setTxi(wxui::toStd(txiEditor_->value()));
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
        const bool pixels = open && document_.texture().hasPixels();
#if defined(__EMSCRIPTEN__)
        const bool comparisonLoading = browserComparison_ != nullptr;
#else
        const bool comparisonLoading = false;
#endif
        wxString name = open ? wxpath::toWx(document_.path().filename()) : wxString("Untitled");
        if (!comparisonImages_.empty()) {
            name += wxString::Format(" + %llu conflict(s)",
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
        if (!playing || !document_.isOpen() || !document_.texture().animated || document_.texture().layers.size() < 2) {
            animationTimer_.Stop();
            playButton_->SetValue(false);
            playButton_->SetLabel("Play");
            return;
        }
        double fps = neotpc::texture::parseTxiFeatures(document_.texture().txi).fps;
        if (fps <= 0.0) fps = 8.0;
        animationTimer_.Start(std::clamp(static_cast<int>(1000.0 / fps), 16, 10000));
        playButton_->SetLabel("Pause");
    }

    void onAnimationTimer(wxTimerEvent&) {
        if (layerChoice_->GetCount() < 2) return;
        int selection = layerChoice_->GetSelection();
        if (selection == wxNOT_FOUND) selection = 0;
        layerChoice_->SetSelection((selection + 1) % static_cast<int>(layerChoice_->GetCount()));
        refreshMipmapChoices();
        refreshPreview();
    }

    void setAlpha() {
        if (!document_.isOpen() || !document_.texture().hasPixels()) return;
        wxTextEntryDialog dialog(this, "Alpha value (0-255)", "Set alpha", "255");
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        long value = 0;
        if (!dialog.GetValue().ToLong(&value) || value < 0 || value > 255) {
            wxMessageBox("Alpha must be an integer from 0 through 255.", "Set alpha", wxOK | wxICON_WARNING, this);
            return;
        }
        document_.setAlpha(static_cast<std::uint8_t>(value));
        afterPixelChange();
    }

    void scaleAlpha() {
        if (!document_.isOpen() || !document_.texture().hasPixels()) return;
        wxTextEntryDialog dialog(this, "Non-negative scale factor", "Scale alpha", "1.0");
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        double value = 0.0;
        if (!dialog.GetValue().ToDouble(&value) || value < 0.0) {
            wxMessageBox("Alpha scale must be a non-negative number.", "Scale alpha", wxOK | wxICON_WARNING, this);
            return;
        }
        document_.scaleAlpha(value);
        afterPixelChange();
    }

    void invertAlpha() {
        if (!document_.isOpen()) return;
        document_.invertAlpha();
        afterPixelChange();
    }

    void flipHorizontal() {
        if (!document_.isOpen()) return;
        document_.flipHorizontal();
        afterPixelChange();
    }

    void flipVertical() {
        if (!document_.isOpen()) return;
        document_.flipVertical();
        afterPixelChange();
    }

    void afterPixelChange() {
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
    return owner_->openPath(wxpath::fromWx(filenames[0]));
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
