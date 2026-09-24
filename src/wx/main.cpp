#include "TextureEditorPanel.hpp"
#include "PathUtils.hpp"
#include "NeoSettings.hpp"
#include "NeoWxUi.hpp"
#include "tpc_icon.xpm"

#include <wx/app.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/sizer.h>

namespace {

class NeoTpcFrame final : public wxFrame {
public:
    NeoTpcFrame() : wxFrame(nullptr, wxID_ANY, "NeoTPC") {
        wxIconBundle icons;
#if defined(__WXMSW__)
        wxIcon native("neotpc", wxBITMAP_TYPE_ICO_RESOURCE);
        if (native.IsOk()) icons.AddIcon(native);
#endif
        wxIcon fallback(tpc_icon);
        if (fallback.IsOk()) icons.AddIcon(fallback);
        SetIcons(icons);

        neomodules::Context context;
        context.titleChanged = [this](const wxString& title) { SetTitle(title); };
        context.closeRequested = [this] { Close(); };
        panel_ = neotpc::ui::createEditorPanel(this, std::move(context));
        SetMenuBar(panel_->takeMenus().release());
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(panel_, 1, wxEXPAND);
        SetSizer(layout);

        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            if (!neomodules::routeCommand({panel_}, event)) event.Skip();
        });
        Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& event) {
            neomodules::routeMenuOpen({panel_}, event);
            event.Skip();
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
            if (!panel_->canClose()) {
                if (event.CanVeto()) event.Veto();
                return;
            }
            settings_.saveWindowPlacement(*this);
            event.Skip();
        });

        wxui::configureResponsiveWindow(*this, wxSize(1280, 820), wxSize(720, 480));
        settings_.restoreWindowPlacement(*this);
    }

    ~NeoTpcFrame() override { DestroyChildren(); }

    void openStartup(const std::filesystem::path& path) {
        try { panel_->openFile(path); }
        catch (const std::exception& error) { wxui::showError(this, error); }
    }

private:
    neotpc::ui::TextureEditorPanel* panel_{};
    neosettings::AppSettings settings_{"NeoTPC"};
};

class NeoTpcApp final : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        SetAppName("NeoTPC");
        SetVendorName("Neo Tools");
        wxInitAllImageHandlers();
        auto* frame = new NeoTpcFrame;
        frame->Show(true);
        if (argc > 1) {
            const auto path = neotpc::wxpath::fromWx(wxString(argv[1]));
            frame->CallAfter([frame, path]() { frame->openStartup(path); });
        }
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoTpcApp);
