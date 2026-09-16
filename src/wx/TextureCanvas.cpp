#include "TextureCanvas.hpp"
#include "NeoWxUi.hpp"
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cmath>
namespace neotpc {
TextureCanvas::TextureCanvas(wxWindow* parent):wxPanel(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_NONE|wxFULL_REPAINT_ON_RESIZE) {
    SetName("Texture pixel inspection canvas"); SetBackgroundStyle(wxBG_STYLE_PAINT);SetMinSize(FromDIP(wxSize(240,220)));
    Bind(wxEVT_PAINT,&TextureCanvas::onPaint,this);Bind(wxEVT_SIZE,&TextureCanvas::onSize,this);
    Bind(wxEVT_MOUSEWHEEL,&TextureCanvas::onMouseWheel,this);Bind(wxEVT_LEFT_DOWN,&TextureCanvas::onLeftDown,this);
    Bind(wxEVT_LEFT_UP,&TextureCanvas::onLeftUp,this);Bind(wxEVT_MOTION,&TextureCanvas::onMotion,this);
    Bind(wxEVT_LEFT_DCLICK,&TextureCanvas::onDoubleClick,this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&){dragging_=false;SetCursor(wxNullCursor);});
    SetToolTip("Wheel: zoom at pointer. Drag: pan. Double-click: fit / 100%. When focused: + / - zoom, 0 fits, 1 shows actual pixels, arrow keys pan.");
    Bind(wxEVT_KEY_DOWN, &TextureCanvas::onKeyDown, this);
    Bind(wxEVT_SET_FOCUS,[this](wxFocusEvent& e){Refresh(false);e.Skip();});
    Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& e){Refresh(false);e.Skip();});
}
void TextureCanvas::setImage(const wxImage& image){
    if (image_.IsOk() && image.IsOk() && image_.GetData() == image.GetData() &&
        image_.GetWidth() == image.GetWidth() && image_.GetHeight() == image.GetHeight()) return;
    const bool reset=!hasImage()||image_.GetWidth()!=image.GetWidth()||image_.GetHeight()!=image.GetHeight();
    image_=image;cache_=wxBitmap{};message_.clear();if(reset)view_=View{};Refresh(false);
}
void TextureCanvas::clearImage(const wxString& message){image_=wxImage{};cache_=wxBitmap{};message_=message.empty()?wxString("No pixel data"):message;Refresh(false);}
void TextureCanvas::setView(const View& view){view_=view;view_.zoom=std::clamp(view_.zoom,.01,64.0);Refresh(false);}
void TextureCanvas::changed(){Refresh(false);if(viewHandler_)viewHandler_(view_);}
void TextureCanvas::fitImage(){view_=View{};changed();}
void TextureCanvas::actualSize(){view_=View{false,1,.5,.5};changed();}
void TextureCanvas::zoomIn(){zoomBy(1.25,{GetClientSize().x/2,GetClientSize().y/2});}
void TextureCanvas::zoomOut(){zoomBy(.8,{GetClientSize().x/2,GetClientSize().y/2});}
void TextureCanvas::setDarkMode(bool enabled){darkMode_=enabled;Refresh(false);}
int TextureCanvas::zoomPercent()const noexcept{return static_cast<int>(std::lround(displayScale()*100));}
double TextureCanvas::displayScale()const{
    if(!hasImage())return 1;if(!view_.fit)return view_.zoom;
    const auto c=GetClientSize();const int margin=FromDIP(24);
    return std::min({1.,static_cast<double>(std::max(1,c.x-2*margin))/image_.GetWidth(),static_cast<double>(std::max(1,c.y-2*margin))/image_.GetHeight()});
}
wxPoint TextureCanvas::origin(double s)const{
    const auto c=GetClientSize();const double cx=view_.fit?.5:view_.centerX,cy=view_.fit?.5:view_.centerY;
    return {static_cast<int>(std::lround(c.x/2.0-cx*image_.GetWidth()*s)),static_cast<int>(std::lround(c.y/2.0-cy*image_.GetHeight()*s))};
}
void TextureCanvas::onPaint(wxPaintEvent&){
    wxAutoBufferedPaintDC dc(this);dc.SetBackground(wxBrush(darkMode_?wxColour(20,22,26):wxColour(58,61,66)));dc.Clear();
    const auto c=GetClientSize();dc.SetTextForeground(wxColour(225,225,230));dc.SetFont(GetFont());
    if(!hasImage()){dc.DrawText(message_,FromDIP(12),FromDIP(24));return;}
    const double scale=displayScale();const auto p=origin(scale);
    const int left=std::clamp(static_cast<int>(std::floor(-p.x/scale)),0,image_.GetWidth());
    const int top=std::clamp(static_cast<int>(std::floor(-p.y/scale)),0,image_.GetHeight());
    const int right=std::clamp(static_cast<int>(std::ceil((c.x-p.x)/scale)),0,image_.GetWidth());
    const int bottom=std::clamp(static_cast<int>(std::ceil((c.y-p.y)/scale)),0,image_.GetHeight());
    if(right>left&&bottom>top){
        const wxRect rect(left,top,right-left,bottom-top);
        const int x=p.x+static_cast<int>(std::lround(left*scale)),y=p.y+static_cast<int>(std::lround(top*scale));
        const wxSize size(std::max(1,static_cast<int>(std::lround(right*scale))-static_cast<int>(std::lround(left*scale))),
                          std::max(1,static_cast<int>(std::lround(bottom*scale))-static_cast<int>(std::lround(top*scale))));
        if(!cache_.IsOk()||cacheRect_!=rect||cacheSize_!=size){
            cache_=wxBitmap(image_.GetSubImage(rect).Scale(size.x,size.y,smooth_?wxIMAGE_QUALITY_BILINEAR:wxIMAGE_QUALITY_NEAREST));cacheRect_=rect;cacheSize_=size;
        }
        dc.SetClippingRegion(0,0,c.x,c.y);dc.DrawBitmap(cache_,x,y,false);
        const int step=grid_==Grid::Blocks?4:1;
        if(grid_!=Grid::None&&scale*step>=8){
            dc.SetPen(wxPen(wxColour(100,100,105),1));
            for(int i=(left/step)*step;i<=right;i+=step){const int xx=p.x+static_cast<int>(std::lround(i*scale));dc.DrawLine(xx,std::max(0,y),xx,std::min(c.y,y+size.y));}
            for(int i=(top/step)*step;i<=bottom;i+=step){const int yy=p.y+static_cast<int>(std::lround(i*scale));dc.DrawLine(std::max(0,x),yy,std::min(c.x,x+size.x),yy);}
        }
        dc.DestroyClippingRegion();
    }
    if(HasFocus()){dc.SetPen(wxPen(wxColour(120,170,215),FromDIP(1)));dc.SetBrush(*wxTRANSPARENT_BRUSH);dc.DrawRectangle(0,0,c.x,c.y);}
}
void TextureCanvas::onKeyDown(wxKeyEvent& event) {
    if (!hasImage() || event.CmdDown() || event.ControlDown() || event.AltDown()) { event.Skip(); return; }
    const int key = event.GetKeyCode();
    switch (key) {
    case '+': case '=': case WXK_NUMPAD_ADD: zoomIn(); return;
    case '-': case WXK_NUMPAD_SUBTRACT: zoomOut(); return;
    case '0': fitImage(); return;
    case '1': actualSize(); return;
    default: break;
    }
    if (key != WXK_LEFT && key != WXK_RIGHT && key != WXK_UP && key != WXK_DOWN) {
        event.Skip(); return;
    }
    const double scale = displayScale();
    if (view_.fit) { view_.fit = false; view_.zoom = scale; view_.centerX = .5; view_.centerY = .5; }
    const double step = FromDIP(event.ShiftDown() ? 128 : 32);
    if (key == WXK_LEFT) view_.centerX -= step / (scale * image_.GetWidth());
    if (key == WXK_RIGHT) view_.centerX += step / (scale * image_.GetWidth());
    if (key == WXK_UP) view_.centerY -= step / (scale * image_.GetHeight());
    if (key == WXK_DOWN) view_.centerY += step / (scale * image_.GetHeight());
    changed();
}

void TextureCanvas::onSize(wxSizeEvent&e){Refresh(false);e.Skip();}
void TextureCanvas::zoomBy(double factor,const wxPoint& anchor){
    if(!hasImage())return;const double old=displayScale();const auto p=origin(old);const auto c=GetClientSize();
    const double pixelX=(anchor.x-p.x)/old,pixelY=(anchor.y-p.y)/old;
    view_.fit=false;view_.zoom=std::clamp(old*factor,.01,64.);
    view_.centerX=(pixelX-(anchor.x-c.x/2.0)/view_.zoom)/image_.GetWidth();
    view_.centerY=(pixelY-(anchor.y-c.y/2.0)/view_.zoom)/image_.GetHeight();changed();
}
void TextureCanvas::onMouseWheel(wxMouseEvent&e){if(!hasImage()||!e.GetWheelDelta()){e.Skip();return;}zoomBy(std::pow(1.2,static_cast<double>(e.GetWheelRotation())/e.GetWheelDelta()),e.GetPosition());}
void TextureCanvas::onLeftDown(wxMouseEvent&e){if(!hasImage())return;SetFocus();view_.zoom=displayScale();if(view_.fit){view_.centerX=.5;view_.centerY=.5;}view_.fit=false;dragging_=true;dragView_=view_;dragStart_=e.GetPosition();SetCursor(wxCursor(wxCURSOR_SIZING));if(!HasCapture())CaptureMouse();}
void TextureCanvas::onLeftUp(wxMouseEvent&){dragging_=false;SetCursor(wxNullCursor);if(HasCapture())ReleaseMouse();}
void TextureCanvas::onMotion(wxMouseEvent&e){
    if(!hasImage())return;
    if(dragging_&&e.Dragging()&&e.LeftIsDown()){
        const auto delta=e.GetPosition()-dragStart_;view_.centerX=dragView_.centerX-delta.x/(displayScale()*image_.GetWidth());view_.centerY=dragView_.centerY-delta.y/(displayScale()*image_.GetHeight());changed();
    }
    if(pixelHandler_){const auto p=origin(displayScale());const int x=static_cast<int>(std::floor((e.GetX()-p.x)/displayScale()));const int y=static_cast<int>(std::floor((e.GetY()-p.y)/displayScale()));pixelHandler_(x,y);}
}
void TextureCanvas::onDoubleClick(wxMouseEvent&){dragging_=false;if(HasCapture())ReleaseMouse();SetCursor(wxNullCursor);if(view_.fit)actualSize();else fitImage();}
} // namespace neotpc
