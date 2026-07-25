// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextureCanvas.hpp"

#include "NeoWxUi.hpp"

#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>

#include <algorithm>
#include <cmath>

namespace neotpc {

TextureCanvas::TextureCanvas(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(320, 260)));
    Bind(wxEVT_PAINT, &TextureCanvas::onPaint, this);
    Bind(wxEVT_SIZE, &TextureCanvas::onSize, this);
    Bind(wxEVT_MOUSEWHEEL, &TextureCanvas::onMouseWheel, this);
    Bind(wxEVT_LEFT_DOWN, &TextureCanvas::onLeftDown, this);
    Bind(wxEVT_LEFT_UP, &TextureCanvas::onLeftUp, this);
    Bind(wxEVT_MOTION, &TextureCanvas::onMotion, this);
    Bind(wxEVT_LEFT_DCLICK, &TextureCanvas::onDoubleClick, this);
}

void TextureCanvas::setImage(const wxImage& image) {
    const bool resetView = !image_.IsOk() || image_.GetWidth() != image.GetWidth() || image_.GetHeight() != image.GetHeight();
    image_ = wxBitmap(image);
    message_.clear();
    if (resetView) {
        fitMode_ = true;
        panOffset_ = {};
    }
    Refresh(false);
}

void TextureCanvas::clearImage(const wxString& message) {
    image_ = wxBitmap{};
    message_ = message.IsEmpty() ? wxString("No pixel data") : message;
    fitMode_ = true;
    zoom_ = 1.0;
    panOffset_ = {};
    Refresh(false);
}

void TextureCanvas::fitImage() {
    if (!hasImage()) return;
    fitMode_ = true;
    panOffset_ = {};
    Refresh(false);
}

void TextureCanvas::actualSize() {
    if (!hasImage()) return;
    fitMode_ = false;
    zoom_ = 1.0;
    panOffset_ = {};
    Refresh(false);
}

void TextureCanvas::zoomIn() { zoomBy(1.25); }
void TextureCanvas::zoomOut() { zoomBy(0.8); }

void TextureCanvas::setDarkMode(bool enabled) {
    darkMode_ = enabled;
    Refresh(false);
}

int TextureCanvas::zoomPercent() const noexcept {
    return static_cast<int>(std::lround(displayScale() * 100.0));
}

double TextureCanvas::displayScale() const {
    if (!hasImage()) return 1.0;
    if (!fitMode_) return zoom_;
    const wxSize client = GetClientSize();
    const int margin = FromDIP(24);
    const double availableWidth = std::max(1, client.GetWidth() - margin * 2);
    const double availableHeight = std::max(1, client.GetHeight() - margin * 2);
    return std::min({availableWidth / image_.GetWidth(), availableHeight / image_.GetHeight(), 1.0});
}

void TextureCanvas::onPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const auto palette = wxui::themePalette(darkMode_);
    dc.SetBackground(wxBrush(darkMode_ ? wxColour(20, 22, 26) : wxColour(58, 61, 66)));
    dc.Clear();

    if (!hasImage()) {
        dc.SetFont(GetFont());
        dc.SetTextForeground(darkMode_ ? palette.mutedText : wxColour(230, 230, 230));
        const wxSize extent = dc.GetTextExtent(message_);
        const wxSize client = GetClientSize();
        dc.DrawText(message_, std::max(FromDIP(12), (client.GetWidth() - extent.GetWidth()) / 2),
                    std::max(FromDIP(12), (client.GetHeight() - extent.GetHeight()) / 2));
        return;
    }

    const double scale = displayScale();
    const int width = std::max(1, static_cast<int>(std::lround(image_.GetWidth() * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(image_.GetHeight() * scale)));
    const wxSize client = GetClientSize();
    const int x = (client.GetWidth() - width) / 2 + (fitMode_ ? 0 : panOffset_.x);
    const int y = (client.GetHeight() - height) / 2 + (fitMode_ ? 0 : panOffset_.y);
    // Scale only the source region visible in this pane. Destination sizes
    // therefore remain close to the window size even at the 64x zoom limit.
    const int visibleLeft = std::max(0, x);
    const int visibleTop = std::max(0, y);
    const int visibleRight = std::min(client.GetWidth(), x + width);
    const int visibleBottom = std::min(client.GetHeight(), y + height);
    if (visibleRight > visibleLeft && visibleBottom > visibleTop) {
        const double scaleX = static_cast<double>(width) / image_.GetWidth();
        const double scaleY = static_cast<double>(height) / image_.GetHeight();
        const int sourceLeft = std::clamp(static_cast<int>(std::floor((visibleLeft - x) / scaleX)),
                                          0, image_.GetWidth() - 1);
        const int sourceTop = std::clamp(static_cast<int>(std::floor((visibleTop - y) / scaleY)),
                                         0, image_.GetHeight() - 1);
        const int sourceRight = std::clamp(static_cast<int>(std::ceil((visibleRight - x) / scaleX)),
                                           sourceLeft + 1, image_.GetWidth());
        const int sourceBottom = std::clamp(static_cast<int>(std::ceil((visibleBottom - y) / scaleY)),
                                            sourceTop + 1, image_.GetHeight());
        const int destinationLeft = x + static_cast<int>(std::lround(sourceLeft * scaleX));
        const int destinationTop = y + static_cast<int>(std::lround(sourceTop * scaleY));
        const int destinationRight = x + static_cast<int>(std::lround(sourceRight * scaleX));
        const int destinationBottom = y + static_cast<int>(std::lround(sourceBottom * scaleY));

        wxMemoryDC source;
        source.SelectObject(image_);
        dc.SetClippingRegion(0, 0, client.GetWidth(), client.GetHeight());
        dc.StretchBlit(destinationLeft, destinationTop,
                       std::max(1, destinationRight - destinationLeft),
                       std::max(1, destinationBottom - destinationTop),
                       &source, sourceLeft, sourceTop,
                       sourceRight - sourceLeft, sourceBottom - sourceTop,
                       wxCOPY, false);
        dc.DestroyClippingRegion();
        source.SelectObject(wxNullBitmap);
    }
    dc.SetPen(wxPen(darkMode_ ? wxColour(92, 96, 104) : wxColour(40, 42, 46)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(x - 1, y - 1, width + 2, height + 2);
}

void TextureCanvas::onSize(wxSizeEvent& event) {
    Refresh(false);
    event.Skip();
}

void TextureCanvas::onMouseWheel(wxMouseEvent& event) {
    if (!hasImage()) {
        event.Skip();
        return;
    }
    const int delta = event.GetWheelDelta();
    const int rotation = event.GetWheelRotation();
    if (delta == 0 || rotation == 0) return;
    const double steps = static_cast<double>(rotation) / delta;
    zoomBy(std::pow(1.2, steps));
}

void TextureCanvas::onLeftDown(wxMouseEvent& event) {
    if (!hasImage()) return;
    const double currentScale = displayScale();
    dragging_ = true;
    fitMode_ = false;
    zoom_ = currentScale;
    dragStart_ = event.GetPosition();
    dragPanStart_ = panOffset_;
    SetCursor(wxCursor(wxCURSOR_SIZING));
    if (!HasCapture()) CaptureMouse();
}

void TextureCanvas::onLeftUp(wxMouseEvent&) {
    if (!dragging_) return;
    dragging_ = false;
    SetCursor(wxNullCursor);
    if (HasCapture()) ReleaseMouse();
}

void TextureCanvas::onMotion(wxMouseEvent& event) {
    if (!dragging_ || !event.Dragging() || !event.LeftIsDown()) return;
    const wxPoint delta = event.GetPosition() - dragStart_;
    panOffset_ = dragPanStart_ + delta;
    Refresh(false);
}

void TextureCanvas::onDoubleClick(wxMouseEvent&) {
    if (!hasImage()) return;
    if (fitMode_) actualSize();
    else fitImage();
}

void TextureCanvas::zoomBy(double factor) {
    if (!hasImage()) return;
    if (fitMode_) zoom_ = displayScale();
    fitMode_ = false;
    zoom_ = std::clamp(zoom_ * factor, 0.01, 64.0);
    Refresh(false);
}

} // namespace neotpc
