#pragma once

#include "texture/Error.hpp"
#include <functional>
#include <utility>

namespace neotpc::texture {
class OperationCancelled final : public TextureError {
public:
    OperationCancelled() : TextureError("Operation cancelled; no unfinished output was committed") {}
};
// Installed by the caller for one synchronous operation on its worker thread.
// The callback must not access GUI objects on a native worker thread.
inline thread_local std::function<void()> operationCheckpoint;
class OperationScope final {
public:
    explicit OperationScope(std::function<void()> callback)
        : previous_(std::move(operationCheckpoint)) { operationCheckpoint = std::move(callback); }
    ~OperationScope() { operationCheckpoint = std::move(previous_); }
    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;
private:
    std::function<void()> previous_;
};
inline void checkOperation() { if (operationCheckpoint) operationCheckpoint(); }
} // namespace neotpc::texture
