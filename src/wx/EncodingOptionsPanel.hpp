#pragma once

#include "texture/Image.hpp"

#include <wx/panel.h>

#include <functional>
#include <utility>

class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxSpinCtrlDouble;

namespace neotpc {

class EncodingOptionsPanel final : public wxPanel {
public:
    explicit EncodingOptionsPanel(wxWindow* parent);

    void setOptions(const neotpc::texture::TextureSaveOptions& options);
    neotpc::texture::TextureSaveOptions options() const;
    void setChangeHandler(std::function<void()> handler) { changeHandler_ = std::move(handler); }

private:
    void notifyChanged();

    wxChoice* compression_ = nullptr;
    wxChoice* dxtQuality_ = nullptr;
    wxChoice* dxtMetric_ = nullptr;
    wxCheckBox* weightAlpha_ = nullptr;
    wxSpinCtrl* dxt1Threshold_ = nullptr;
    wxSpinCtrl* jpegQuality_ = nullptr;
    wxCheckBox* mipmaps_ = nullptr;
    wxCheckBox* bicubic_ = nullptr;
    wxCheckBox* flipX_ = nullptr;
    wxCheckBox* flipY_ = nullptr;
    wxSpinCtrlDouble* alphaBlending_ = nullptr;
    std::function<void()> changeHandler_;
    bool loading_ = false;
};

} // namespace neotpc
