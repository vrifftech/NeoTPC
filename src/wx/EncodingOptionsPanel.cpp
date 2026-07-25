// SPDX-License-Identifier: GPL-3.0-or-later
#include "EncodingOptionsPanel.hpp"

#include "NeoWxUi.hpp"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include <vector>

namespace neotpc {
namespace {

wxChoice* makeChoice(wxWindow* parent, const std::vector<wxString>& values) {
    auto* choice = new wxChoice(parent, wxID_ANY);
    for (const auto& value : values) choice->Append(value);
    if (!values.empty()) choice->SetSelection(0);
    return choice;
}

} // namespace

EncodingOptionsPanel::EncodingOptionsPanel(wxWindow* parent)
    : wxPanel(parent) {
    auto* outer = new wxStaticBoxSizer(wxVERTICAL, this, "Conversion settings");
    auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(1, 1);

    compression_ = makeChoice(this, {"auto", "none", "grey", "dxt1", "dxt3", "dxt5", "swizzled-bgra"});
    dxtQuality_ = makeChoice(this, {"fast", "normal", "high"});
    dxtMetric_ = makeChoice(this, {"perceptual", "uniform"});
    dxt1Threshold_ = new wxSpinCtrl(this, wxID_ANY, "128", wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0, 255, 128);
    jpegQuality_ = new wxSpinCtrl(this, wxID_ANY, "95", wxDefaultPosition, wxDefaultSize,
                                  wxSP_ARROW_KEYS, 1, 100, 95);
    alphaBlending_ = new wxSpinCtrlDouble(this, wxID_ANY, "1.0", wxDefaultPosition, wxDefaultSize,
                                          wxSP_ARROW_KEYS, -100000.0, 100000.0, 1.0, 0.05);
    alphaBlending_->SetDigits(4);

    auto addRow = [this, grid](const wxString& label, wxWindow* control) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(control, 1, wxEXPAND);
    };
    addRow("TPC/DDS compression", compression_);
    addRow("DXT quality", dxtQuality_);
    addRow("DXT metric", dxtMetric_);
    addRow("DXT1 alpha threshold", dxt1Threshold_);
    addRow("JPEG quality", jpegQuality_);
    addRow("TPC alpha blending", alphaBlending_);
    outer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));

    weightAlpha_ = new wxCheckBox(this, wxID_ANY, "Weight DXT color error by alpha");
    mipmaps_ = new wxCheckBox(this, wxID_ANY, "Generate mipmaps");
    bicubic_ = new wxCheckBox(this, wxID_ANY, "Bicubic mipmap downsampling");
    flipX_ = new wxCheckBox(this, wxID_ANY, "Flip horizontally on save");
    flipY_ = new wxCheckBox(this, wxID_ANY, "Flip vertically on save");
    for (auto* checkbox : {weightAlpha_, mipmaps_, bicubic_, flipX_, flipY_}) {
        outer->Add(checkbox, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        checkbox->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { notifyChanged(); });
    }

    compression_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { notifyChanged(); });
    dxtQuality_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { notifyChanged(); });
    dxtMetric_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { notifyChanged(); });
    dxt1Threshold_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { notifyChanged(); });
    jpegQuality_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { notifyChanged(); });
    alphaBlending_->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { notifyChanged(); });

    compression_->SetToolTip("Auto chooses from alpha/source metadata. DXT3 is DDS-only; swizzled-bgra writes power-of-two Xbox-style TPC 0x0C data.");
    dxt1Threshold_->SetToolTip("Pixels below this alpha become punch-through transparent in DXT1/BC1.");
    jpegQuality_->SetToolTip("JPEG output discards alpha.");
    SetSizer(outer);
    setOptions({});
}

void EncodingOptionsPanel::setOptions(const neotpc::texture::TextureSaveOptions& options) {
    loading_ = true;
    compression_->SetStringSelection(wxui::toWx(neotpc::texture::textureCompressionToString(options.compression)));
    dxtQuality_->SetStringSelection(wxui::toWx(neotpc::texture::dxtCompressionQualityToString(options.dxtQuality)));
    dxtMetric_->SetStringSelection(wxui::toWx(neotpc::texture::dxtErrorMetricToString(options.dxtMetric)));
    weightAlpha_->SetValue(options.weightColorByAlpha);
    dxt1Threshold_->SetValue(options.dxt1AlphaThreshold);
    jpegQuality_->SetValue(options.jpegQuality);
    mipmaps_->SetValue(options.generateMipmaps);
    bicubic_->SetValue(options.bicubicMipmaps);
    flipX_->SetValue(options.flipXOnSave);
    flipY_->SetValue(options.flipYOnSave);
    alphaBlending_->SetValue(options.alphaBlending);
    loading_ = false;
}

neotpc::texture::TextureSaveOptions EncodingOptionsPanel::options() const {
    neotpc::texture::TextureSaveOptions options;
    options.compression = neotpc::texture::textureCompressionFromString(wxui::toStd(compression_->GetStringSelection()));
    options.dxtQuality = neotpc::texture::dxtCompressionQualityFromString(wxui::toStd(dxtQuality_->GetStringSelection()));
    options.dxtMetric = neotpc::texture::dxtErrorMetricFromString(wxui::toStd(dxtMetric_->GetStringSelection()));
    options.weightColorByAlpha = weightAlpha_->GetValue();
    options.dxt1AlphaThreshold = static_cast<std::uint8_t>(dxt1Threshold_->GetValue());
    options.jpegQuality = static_cast<std::uint8_t>(jpegQuality_->GetValue());
    options.generateMipmaps = mipmaps_->GetValue();
    options.bicubicMipmaps = bicubic_->GetValue();
    options.flipXOnSave = flipX_->GetValue();
    options.flipYOnSave = flipY_->GetValue();
    options.alphaBlending = static_cast<float>(alphaBlending_->GetValue());
    return options;
}

void EncodingOptionsPanel::notifyChanged() {
    if (!loading_ && changeHandler_) changeHandler_();
}

} // namespace neotpc
