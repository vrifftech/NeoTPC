#include "BrowserWorkload.hpp"

#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <utility>

#include <wx/app.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

namespace neotpc::browser {
namespace {

struct PendingRead {
    std::vector<std::uint8_t> bytes;
    RetainedReadCallback callback;
    bool cancelled = false;
};

std::map<std::uint32_t, std::shared_ptr<PendingRead>>& pendingReads() {
    static std::map<std::uint32_t, std::shared_ptr<PendingRead>> value;
    return value;
}

std::uint32_t nextRequestId() {
    static std::uint32_t next = 0;
    auto& pending = pendingReads();
    for (std::uint64_t attempt = 0; attempt < std::numeric_limits<std::uint32_t>::max(); ++attempt) {
        ++next;
        if (next == 0) ++next;
        if (pending.find(next) == pending.end()) return next;
    }
    return 0;
}

void deliver(RetainedReadCallback callback, RetainedReadResult result) {
    if (!callback) return;
    if (wxTheApp != nullptr) {
        wxTheApp->CallAfter(
            [callback = std::move(callback), result = std::move(result)]() mutable {
                callback(std::move(result));
            });
        return;
    }
    callback(std::move(result));
}

#if defined(__EMSCRIPTEN__)

EM_JS(int, neotpc_begin_retained_read_js,
      (unsigned int requestId,
       unsigned int sessionId,
       unsigned int fileId,
       double byteCount,
       void* destination), {
    var state = Module.neoToolsBrowserFiles || null;
    if (!state || typeof state.readRetainedFileRange !== 'function') return 0;

    if (!Module.neoTpcRetainedReads) Module.neoTpcRetainedReads = new Map();
    var token = { cancelled: false };
    Module.neoTpcRetainedReads.set(requestId, token);

    function messageOf(error) {
      return error && error.message ? error.message : String(error || 'Unknown retained-file read error.');
    }

    function complete(error) {
      Module.neoTpcRetainedReads.delete(requestId);
      window.setTimeout(function() {
        try {
          Module.ccall(
            'neotpc_retained_read_completed',
            null,
            ['number', 'string'],
            [requestId, error || '']);
        } catch (callbackError) {
          console.error('[NeoTPC] Retained-file completion callback failed:', callbackError);
        }
      }, 0);
    }

    Promise.resolve().then(async function() {
      var total = Number(byteCount);
      var pointer = Number(destination);
      if (!Number.isSafeInteger(total) || total < 0) {
        throw new Error('Retained texture size is outside JavaScript\'s exact integer range.');
      }
      var chunkSize = 4 * 1024 * 1024;
      var offset = 0;
      while (offset < total) {
        if (token.cancelled) throw new Error('Retained texture read was cancelled.');
        var count = Math.min(chunkSize, total - offset);
        await state.readRetainedFileRange(
          sessionId, fileId, offset, count, pointer + offset);
        offset += count;
        // Return to the browser between large slices so cancellation, paint,
        // and progress events are not starved by directory-sized workloads.
        if (offset < total) {
          await new Promise(function(resolve) { window.setTimeout(resolve, 0); });
        }
      }
      if (token.cancelled) throw new Error('Retained texture read was cancelled.');
      complete('');
    }).catch(function(error) {
      complete(messageOf(error));
    });
    return 1;
});

EM_JS(void, neotpc_cancel_retained_read_js, (unsigned int requestId), {
    var reads = Module.neoTpcRetainedReads || null;
    var token = reads ? reads.get(requestId) : null;
    if (token) token.cancelled = true;
});

EM_JS(void, neotpc_cancel_scheduled_publish_js, (const char* pathPtr), {
    var state = Module.neoToolsBrowserFiles || null;
    var path = UTF8ToString(pathPtr || 0);
    if (state && path && typeof state.consumeWritablePath === 'function') {
      state.consumeWritablePath(path);
    }
});

#endif

} // namespace

std::uint32_t requestRetainedFileBytes(std::uint32_t sessionId,
                                       std::uint32_t fileId,
                                       std::uint64_t byteCount,
                                       std::size_t maximumBytes,
                                       RetainedReadCallback callback) {
    if (!callback) return 0;
    if (sessionId == 0 || fileId == 0) {
        deliver(std::move(callback), RetainedReadResult{{}, "Retained browser-file identity is invalid."});
        return 0;
    }
    if (byteCount > static_cast<std::uint64_t>(maximumBytes) ||
        byteCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        deliver(std::move(callback), RetainedReadResult{{}, "Selected texture exceeds the browser working-set limit."});
        return 0;
    }

#if defined(__EMSCRIPTEN__)
    auto pending = std::make_shared<PendingRead>();
    try {
        pending->bytes.resize(static_cast<std::size_t>(byteCount));
    } catch (const std::exception& error) {
        deliver(std::move(callback),
                RetainedReadResult{{}, std::string("Unable to allocate retained texture bytes: ") + error.what()});
        return 0;
    }
    pending->callback = std::move(callback);

    const std::uint32_t requestId = nextRequestId();
    if (requestId == 0) {
        deliver(std::move(pending->callback),
                RetainedReadResult{{}, "No retained-file read request identifiers are available."});
        return 0;
    }
    pendingReads().emplace(requestId, pending);
    if (neotpc_begin_retained_read_js(
            requestId,
            sessionId,
            fileId,
            static_cast<double>(byteCount),
            pending->bytes.empty() ? nullptr : pending->bytes.data()) == 0) {
        pendingReads().erase(requestId);
        deliver(std::move(pending->callback),
                RetainedReadResult{{}, "The retained browser-file bridge is unavailable."});
        return 0;
    }
    return requestId;
#else
    (void)byteCount;
    (void)maximumBytes;
    deliver(std::move(callback),
            RetainedReadResult{{}, "Retained browser-file reads are unavailable in this build."});
    return 0;
#endif
}

void cancelRetainedFileRead(std::uint32_t requestId) noexcept {
    if (requestId == 0) return;
    const auto found = pendingReads().find(requestId);
    if (found == pendingReads().end()) return;
    found->second->cancelled = true;
    found->second->callback = {};
#if defined(__EMSCRIPTEN__)
    neotpc_cancel_retained_read_js(requestId);
#endif
}

void cancelScheduledPublish(const std::filesystem::path& path) noexcept {
#if defined(__EMSCRIPTEN__)
    if (path.empty()) return;
    const std::string value = path.generic_string();
    neotpc_cancel_scheduled_publish_js(value.c_str());
#else
    (void)path;
#endif
}

#if defined(__EMSCRIPTEN__)
extern "C" EMSCRIPTEN_KEEPALIVE void neotpc_retained_read_completed(
    unsigned int requestId,
    const char* error) {
    auto& pending = pendingReads();
    const auto found = pending.find(requestId);
    if (found == pending.end()) return;

    std::shared_ptr<PendingRead> request = std::move(found->second);
    pending.erase(found);
    if (request->cancelled || !request->callback) return;

    RetainedReadResult result;
    if (error != nullptr && *error != '\0') {
        result.error = error;
    } else {
        result.bytes = std::move(request->bytes);
    }
    deliver(std::move(request->callback), std::move(result));
}
#endif

} // namespace neotpc::browser
