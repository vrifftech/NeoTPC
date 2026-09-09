#include "EncodingOptionsPanel.hpp"

#include "NeoWxUi.hpp"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include <vector>
#include <cmath>

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
    auto* outer = new wxStaticBoxSizer(wxVERTICAL, this, "Encoding for selected output");
    auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(1, 1);

    compression_ = makeChoice(this, {"auto", "none", "grey", "dxt1", "dxt3", "dxt5"});
    ddsDialect_ = makeChoice(this, {"Preserve source / game for new DDS", "Game DDS (BioWare)", "Standard DDS (interchange)"});
    dxtQuality_ = makeChoice(this, {"fast", "normal", "high"});
    dxtMetric_ = makeChoice(this, {"perceptual", "uniform"});
    dxt1Threshold_ = new wxSpinCtrl(this, wxID_ANY, "128", wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0, 255, 128);
    jpegQuality_ = new wxSpinCtrl(this, wxID_ANY, "95", wxDefaultPosition, wxDefaultSize,
                                  wxSP_ARROW_KEYS, 1, 100, 95);
    alphaBlending_ = new wxSpinCtrlDouble(this, wxID_ANY, "1.0", wxDefaultPosition, wxDefaultSize,
                                          wxSP_ARROW_KEYS, -100000.0, 100000.0, 1.0, 0.05);
    alphaBlending_->SetDigits(4);
    compression_->SetName("Output compression");
    ddsDialect_->SetName("DDS container");
    jpegQuality_->SetName("JPEG quality");
    dxt1Threshold_->SetName("Standard DDS BC1 threshold");
    alphaBlending_->SetName("Native texture header value");

    auto addRow = [this, grid](const wxString& label, wxWindow* control) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(control, 1, wxEXPAND);
    };
    mipmaps_ = makeChoice(this, {"Preserve existing / create if absent", "Rebuild from base image", "Base only"});
    mipAlpha_ = makeChoice(this, {"Independent mask / data", "Transparency (alpha-aware)"});
    mipColor_ = makeChoice(this, {"Stored values / data", "sRGB color (linear-light filter)"});
    mipmaps_->SetName("Mipmap policy");
    mipAlpha_->SetName("Mipmap alpha meaning");
    mipColor_->SetName("Mipmap color meaning");
    addRow("Mipmaps", mipmaps_);
    addRow("Mip alpha meaning", mipAlpha_);
    addRow("Mip color meaning", mipColor_);
    addRow("TPC/DDS compression", compression_);
    addRow("DDS container", ddsDialect_);
    addRow("DXT quality", dxtQuality_);
    addRow("DXT metric", dxtMetric_);
    addRow("DXT1 alpha threshold", dxt1Threshold_);
    addRow("JPEG quality", jpegQuality_);
    addRow("TPC/game DDS header float", alphaBlending_);
    outer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));

    weightAlpha_ = new wxCheckBox(this, wxID_ANY, "Weight DXT color error by alpha");
    bicubic_ = new wxCheckBox(this, wxID_ANY, "Bicubic mipmap downsampling");
    flipX_ = new wxCheckBox(this, wxID_ANY, "Flip horizontally on save");
    flipY_ = new wxCheckBox(this, wxID_ANY, "Flip vertically on save");
    for (auto* checkbox : {weightAlpha_, bicubic_, flipX_, flipY_}) {
        outer->Add(checkbox, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        checkbox->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { notifyChanged(); });
    }

    for (auto* choice : {mipmaps_, mipAlpha_, mipColor_})
        choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { notifyChanged(); });
    ddsDialect_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { notifyChanged(); });
    compression_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { notifyChanged(); });
    dxtQuality_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { notifyChanged(); });
    dxtMetric_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { notifyChanged(); });
    dxt1Threshold_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { notifyChanged(); });
    jpegQuality_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { notifyChanged(); });
    alphaBlending_->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
        if (!loading_) headerFloatEdited_ = true;
        notifyChanged();
    });
    alphaBlending_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!loading_) { headerFloatEdited_ = true; notifyChanged(); }
    });

    compression_->SetToolTip("DXT3 is standard-DDS-only. Xbox swizzled textures remain readable; new desktop output uses raw, DXT1 or DXT5.");
    ddsDialect_->SetToolTip("Ordinary Save preserves the loaded container. Game DDS uses the 20-byte BioWare header; standard DDS is for other tools, not these game loaders.");
    mipmaps_->SetToolTip("Preserve keeps supplied levels; creates a chain if absent. Rebuild replaces all lower levels. Base only removes them. Game TPC rectangles need base only + mipmap 0; cubes and animation need a complete chain.");
    mipAlpha_->SetToolTip("Choose Transparency only when alpha represents opacity. Mask/data channels must stay independent. Applies when generating or rebuilding levels, never to untouched levels.");
    mipColor_->SetToolTip("sRGB filtering is for color artwork, not normal/height maps or independent data. No automatic reinterpretation of imported images.");
    alphaBlending_->SetToolTip("Stored TPC/game DDS header value; this is separate from pixel transparency.");
    dxt1Threshold_->SetToolTip("Standard DDS BC1 only. The inspected native TPC/game DDS paths use opaque RGB BC1; use DXT5 for transparency there.");
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
    mipmaps_->SetSelection(options.generateMipmaps ? static_cast<int>(options.mipmapPolicy) : 2);
    mipAlpha_->SetSelection(static_cast<int>(options.mipmapAlpha));
    mipColor_->SetSelection(static_cast<int>(options.mipmapColor));
    bicubic_->SetValue(options.bicubicMipmaps);
    flipX_->SetValue(options.flipXOnSave);
    flipY_->SetValue(options.flipYOnSave);
    originalHeaderFloat_ = options.alphaBlending;
    headerFloatEdited_ = false;
    const float displayed = options.alphaBlending.value_or(1.0f);
    alphaBlending_->SetValue(std::isfinite(displayed) ? displayed : 1.0f);
    ddsDialect_->SetSelection(static_cast<int>(options.ddsDialect));
    loading_ = false;
    updateControls();
}

neotpc::texture::TextureSaveOptions EncodingOptionsPanel::options() const {
    neotpc::texture::TextureSaveOptions options;
    options.ddsDialect = static_cast<neotpc::texture::DdsDialect>(ddsDialect_->GetSelection());
    options.compression = neotpc::texture::textureCompressionFromString(wxui::toStd(compression_->GetStringSelection()));
    options.dxtQuality = neotpc::texture::dxtCompressionQualityFromString(wxui::toStd(dxtQuality_->GetStringSelection()));
    options.dxtMetric = neotpc::texture::dxtErrorMetricFromString(wxui::toStd(dxtMetric_->GetStringSelection()));
    options.weightColorByAlpha = weightAlpha_->GetValue();
    options.dxt1AlphaThreshold = static_cast<std::uint8_t>(dxt1Threshold_->GetValue());
    options.jpegQuality = static_cast<std::uint8_t>(jpegQuality_->GetValue());
    options.mipmapPolicy = static_cast<neotpc::texture::MipmapPolicy>(mipmaps_->GetSelection());
    options.generateMipmaps = options.mipmapPolicy != neotpc::texture::MipmapPolicy::BaseOnly;
    options.mipmapAlpha = static_cast<neotpc::texture::MipmapAlpha>(mipAlpha_->GetSelection());
    options.mipmapColor = static_cast<neotpc::texture::MipmapColor>(mipColor_->GetSelection());
    options.bicubicMipmaps = bicubic_->GetValue();
    options.flipXOnSave = flipX_->GetValue();
    options.flipYOnSave = flipY_->GetValue();
    // Display precision/clamping must not silently rewrite an imported float.
    options.alphaBlending = headerFloatEdited_
        ? std::optional<float>(static_cast<float>(alphaBlending_->GetValue())) : originalHeaderFloat_;
    return options;
}

void EncodingOptionsPanel::setTarget(neotpc::texture::TextureFileKind kind, neotpc::texture::DdsDialect source, bool existingMipmaps) {
    target_ = kind;
    sourceDialect_ = source;
    existingMipmaps_ = existingMipmaps;
    updateControls();
}

void EncodingOptionsPanel::updateControls() {
    using namespace neotpc::texture;
    const bool dds = target_ == TextureFileKind::Dds;
    const bool container = dds || target_ == TextureFileKind::Tpc || target_ == TextureFileKind::Txb;
    const auto dialect = static_cast<DdsDialect>(ddsDialect_->GetSelection());
    const bool standard = dds && (dialect == DdsDialect::Standard ||
        (dialect == DdsDialect::Auto && sourceDialect_ == DdsDialect::Standard));
    const auto compression = textureCompressionFromString(wxui::toStd(compression_->GetStringSelection()));
    const bool dxt = container && compression != TextureCompression::None && compression != TextureCompression::Gray;
    compression_->Enable(container);
    ddsDialect_->Enable(dds);
    dxtQuality_->Enable(dxt); dxtMetric_->Enable(dxt); weightAlpha_->Enable(dxt);
    dxt1Threshold_->Enable(standard && (compression == TextureCompression::Auto || compression == TextureCompression::Dxt1));
    jpegQuality_->Enable(target_ == TextureFileKind::Jpeg);
    alphaBlending_->Enable(container && !standard);
    mipmaps_->Enable(container);
    const bool filter = container && (mipmaps_->GetSelection() == 1 || (mipmaps_->GetSelection() == 0 && !existingMipmaps_));
    bicubic_->Enable(filter); mipAlpha_->Enable(filter); mipColor_->Enable(filter);
    flipX_->Enable(target_ != TextureFileKind::Txi); flipY_->Enable(target_ != TextureFileKind::Txi);
}

void EncodingOptionsPanel::notifyChanged() {
    updateControls();
    if (!loading_ && changeHandler_) changeHandler_();
}

} // namespace neotpc
