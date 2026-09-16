#pragma once

#include "texture/Image.hpp"

#include <wx/panel.h>

#include <functional>
#include <utility>
#include <vector>

class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxSpinCtrlDouble;

namespace neotpc {
namespace layout { class WrappedCheckBox; }

class EncodingOptionsPanel final : public wxPanel {
public:
    explicit EncodingOptionsPanel(wxWindow* parent, bool resizeTopLevelOnExpand = false);

    void setOptions(const neotpc::texture::TextureSaveOptions& options);
    neotpc::texture::TextureSaveOptions options() const;
    void setTarget(neotpc::texture::TextureFileKind kind, neotpc::texture::DdsDialect source = neotpc::texture::DdsDialect::Auto, bool existingMipmaps = false);
    void setChangeHandler(std::function<void()> handler) { changeHandler_ = std::move(handler); }

private:
    void notifyChanged();
    void updateControls();
    void showRow(wxWindow* control, bool visible);
    std::vector<std::pair<wxWindow*, wxWindow*>> rows_;
    wxWindow* advanced_ = nullptr;

    wxChoice* compression_ = nullptr;
    wxChoice* ddsDialect_ = nullptr;
    wxChoice* dxtQuality_ = nullptr;
    wxChoice* dxtMetric_ = nullptr;
    layout::WrappedCheckBox* weightAlpha_ = nullptr;
    wxSpinCtrl* dxt1Threshold_ = nullptr;
    wxSpinCtrl* jpegQuality_ = nullptr;
    wxChoice* mipmaps_ = nullptr;
    wxChoice* mipAlpha_ = nullptr;
    wxChoice* mipColor_ = nullptr;
    neotpc::texture::TextureFileKind target_ = neotpc::texture::TextureFileKind::Tpc;
    neotpc::texture::DdsDialect sourceDialect_ = neotpc::texture::DdsDialect::Auto;
    layout::WrappedCheckBox* bicubic_ = nullptr;
    layout::WrappedCheckBox* flipX_ = nullptr;
    layout::WrappedCheckBox* flipY_ = nullptr;
    wxSpinCtrlDouble* alphaBlending_ = nullptr;
    std::function<void()> changeHandler_;
    bool loading_ = false;
    bool existingMipmaps_ = false;
    std::optional<float> originalHeaderFloat_;
    bool headerFloatEdited_ = false;
};

} // namespace neotpc
