#pragma once
// One bounded operation at a time. Worker code never accesses a wx control.
// The dialog owns/join-waits the worker; no background task outlives its caller.
#include "texture/Error.hpp"
#include "texture/Operation.hpp"
#include "NeoWxUi.hpp"
#include <wx/progdlg.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#ifndef __EMSCRIPTEN__
#include <future>
#else
#include <emscripten/emscripten.h>
#endif

namespace neotpc {
struct TextureTaskProgress {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::string label;
    std::size_t complete=0, total=0;
    void set(std::size_t done, std::size_t count, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        complete=done; total=count; label=message;
    }
};

template<class F>
auto runTextureTask(wxWindow* owner, const wxString& title, F&& work)
    -> std::invoke_result_t<F, TextureTaskProgress&> {
    TextureTaskProgress state;
    std::unique_ptr<wxProgressDialog> progress;
    const auto started = std::chrono::steady_clock::now();
    auto ensureProgress = [&] {
        if (!progress && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(180))
            progress = std::make_unique<wxProgressDialog>(title, "Working...", 100, owner,
                wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_ELAPSED_TIME|wxPD_AUTO_HIDE);
    };
    auto update=[&] {
        ensureProgress();
        if (!progress) return;
        std::string label; std::size_t done=0,total=0;
        { std::lock_guard<std::mutex> lock(state.mutex); label=state.label; done=state.complete; total=state.total; }
        const auto message=label.empty()?title:wxui::toWx(label);
        // Never announce completion while the final encoder/commit is running.
        const bool keep=total ? progress->Update(static_cast<int>(std::min<std::size_t>(99,100*done/total)),message)
                              : progress->Pulse(message);
        if(!keep) state.cancelled.store(true);
    };
#ifndef __EMSCRIPTEN__
    auto future=std::async(std::launch::async,[&] {
        texture::OperationScope checkpoint([&] { if(state.cancelled.load()) throw texture::OperationCancelled(); });
        return std::invoke(work,state);
    });
    while(future.wait_for(std::chrono::milliseconds(25))!=std::future_status::ready) update();
    if constexpr(std::is_void_v<std::invoke_result_t<F,TextureTaskProgress&>>) {
        future.get(); if (progress) progress->Update(100,"Finished");
    } else {
        auto result=future.get(); if (progress) progress->Update(100,"Finished"); return result;
    }
#else
    // Asyncify/cooperative path: no pthread requirement and no worker touching wx.
    auto last=std::chrono::steady_clock::now();
    texture::OperationScope checkpoint([&] {
        if(std::chrono::steady_clock::now()-last>std::chrono::milliseconds(45)) {
            update(); emscripten_sleep(1); last=std::chrono::steady_clock::now();
        }
        if(state.cancelled.load()) throw texture::OperationCancelled();
    });
    if constexpr(std::is_void_v<std::invoke_result_t<F,TextureTaskProgress&>>) {
        std::invoke(work,state); if (progress) progress->Update(100,"Finished");
    } else {
        auto result=std::invoke(work,state); if (progress) progress->Update(100,"Finished"); return result;
    }
#endif
}

class TextureBusyGuard {
    bool& busy_;
public:
    explicit TextureBusyGuard(bool& busy):busy_(busy) { if(busy_) throw texture::TextureError("Another texture operation is running."); busy_=true; }
    ~TextureBusyGuard(){busy_=false;}
    TextureBusyGuard(const TextureBusyGuard&)=delete;
};
} // namespace neotpc
