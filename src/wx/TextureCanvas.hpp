#pragma once
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <functional>
#include <utility>
namespace neotpc {
class TextureCanvas final : public wxPanel {
public:
    struct View { bool fit=true; double zoom=1,centerX=.5,centerY=.5; };
    enum class Grid { None, Pixels, Blocks };
    explicit TextureCanvas(wxWindow* parent);
    void setImage(const wxImage& image);
    void clearImage(const wxString& message = {});
    void fitImage(); void actualSize(); void zoomIn(); void zoomOut();
    void setDarkMode(bool enabled);
    bool hasImage() const noexcept {return image_.IsOk();}
    int zoomPercent() const noexcept;
    void setView(const View& view); View view() const {return view_;}
    void setViewHandler(std::function<void(const View&)> handler){viewHandler_=std::move(handler);}
    void setPixelHandler(std::function<void(int,int)> handler){pixelHandler_=std::move(handler);}
    void setSmooth(bool value){smooth_=value;cache_=wxBitmap{};Refresh(false);}
    void setGrid(Grid value){grid_=value;Refresh(false);}
    bool smooth() const{return smooth_;} Grid grid() const{return grid_;}
private:
    void onPaint(wxPaintEvent&); void onSize(wxSizeEvent&);
    void onKeyDown(wxKeyEvent&);
    void onMouseWheel(wxMouseEvent&); void onLeftDown(wxMouseEvent&);void onLeftUp(wxMouseEvent&);
    void onMotion(wxMouseEvent&);void onDoubleClick(wxMouseEvent&);
    void zoomBy(double factor, const wxPoint& anchor);void changed();
    double displayScale() const;wxPoint origin(double scale) const;
    wxImage image_;wxBitmap cache_;wxRect cacheRect_;wxSize cacheSize_;
    wxString message_="Open or drop a texture to begin";
    View view_,dragView_;Grid grid_=Grid::None;bool smooth_=false,darkMode_=false,dragging_=false;
    wxPoint dragStart_;
    std::function<void(const View&)> viewHandler_;
    std::function<void(int,int)> pixelHandler_;
};
} // namespace neotpc
