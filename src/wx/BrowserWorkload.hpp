#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace neotpc::browser {

// The shared retained-file bridge exposes browser File objects without copying
// complete selections into MEMFS. This wrapper performs one bounded, chunked
// read asynchronously and delivers its result on the wx event queue.
struct RetainedReadResult {
    std::vector<std::uint8_t> bytes;
    std::string error;
};

using RetainedReadCallback = std::function<void(RetainedReadResult)>;

// Returns a request identifier, or zero when the request could not be started.
// maximumBytes is an application working-set ceiling in addition to the shared
// bridge's range checks.
std::uint32_t requestRetainedFileBytes(std::uint32_t sessionId,
                                       std::uint32_t fileId,
                                       std::uint64_t byteCount,
                                       std::size_t maximumBytes,
                                       RetainedReadCallback callback);

// Cancellation is cooperative. Any in-flight browser slice is allowed to
// settle, but no further chunks are requested and the C++ callback is dropped.
void cancelRetainedFileRead(std::uint32_t requestId) noexcept;

// Texture writers close and rename transactional files, which causes the
// compatibility bridge to schedule an automatic publish. Batch conversion
// explicitly publishes outputs in sequence, so it cancels that timer first.
void cancelScheduledPublish(const std::filesystem::path& path) noexcept;

} // namespace neotpc::browser
