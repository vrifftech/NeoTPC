#pragma once

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/panel.h>

class wxMouseEvent;
class wxPaintEvent;
class wxSizeEvent;

namespace neotpc {

class TextureCanvas final : public wxPanel {
public:
    explicit TextureCanvas(wxWindow* parent);

    void setImage(const wxImage& image);
    void clearImage(const wxString& message = {});
    void fitImage();
    void actualSize();
    void zoomIn();
    void zoomOut();
    void setDarkMode(bool enabled);
    bool hasImage() const noexcept { return image_.IsOk(); }
    int zoomPercent() const noexcept;

private:
    void onPaint(wxPaintEvent& event);
    void onSize(wxSizeEvent& event);
    void onMouseWheel(wxMouseEvent& event);
    void onLeftDown(wxMouseEvent& event);
    void onLeftUp(wxMouseEvent& event);
    void onMotion(wxMouseEvent& event);
    void onDoubleClick(wxMouseEvent& event);
    void zoomBy(double factor);
    double displayScale() const;

    wxBitmap image_;
    wxString message_ = "Open or drop a texture to begin";
    double zoom_ = 1.0;
    bool fitMode_ = true;
    bool darkMode_ = false;
    wxPoint panOffset_;
    wxPoint dragStart_;
    wxPoint dragPanStart_;
    bool dragging_ = false;
};

} // namespace neotpc
