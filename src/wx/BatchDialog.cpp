#include "BatchDialog.hpp"

#include "EncodingOptionsPanel.hpp"
#include "PathUtils.hpp"
#include "texture/BatchConverter.hpp"
#include "NeoWxUi.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filepicker.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <exception>
#include <filesystem>

namespace neotpc {
namespace {

enum : int {
    ID_CONVERT = wxID_HIGHEST + 470,
};

} // namespace

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
    inputDirectory_ = new wxDirPickerCtrl(this, wxID_ANY, initialDirectory, "Choose input folder",
                                          wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
    paths->Add(inputDirectory_, 1, wxEXPAND);
    paths->Add(new wxStaticText(this, wxID_ANY, "Output folder"), 0, wxALIGN_CENTER_VERTICAL);
    wxString output = initialDirectory;
    if (!output.IsEmpty()) {
        output += wxFileName::GetPathSeparator();
        output += "converted";
    }
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

    report_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(700, 190)),
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    report_->SetHint("The conversion report will appear here.");
    root->Add(report_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    auto* convert = new wxButton(this, ID_CONVERT, "Convert");
    auto* close = new wxButton(this, wxID_CLOSE, "Close");
    buttons->Add(convert, 0, wxRIGHT, FromDIP(8));
    buttons->Add(close);
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    SetSizer(root);
    SetMinSize(FromDIP(wxSize(660, 620)));
    SetInitialSize(FromDIP(wxSize(780, 760)));
    CentreOnParent();
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { onConvert(); }, ID_CONVERT);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
    wxui::applyTheme(this, darkMode);
}

void BatchDialog::onConvert() {
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
}

} // namespace neotpc
