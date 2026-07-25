// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace neotpc::texture::parser {

// Default memory ceilings for untrusted texture and sidecar input.
inline constexpr std::uint64_t kDefaultMaxInMemoryFileBytes = UINT64_C(512) * 1024 * 1024;
inline constexpr std::uint64_t kDefaultMaxDecodedBytes = UINT64_C(512) * 1024 * 1024;

inline std::uint64_t& inMemoryFileByteLimitStorage() noexcept {
    static thread_local std::uint64_t limit = kDefaultMaxInMemoryFileBytes;
    return limit;
}

inline std::uint64_t& decodedByteLimitStorage() noexcept {
    static thread_local std::uint64_t limit = kDefaultMaxDecodedBytes;
    return limit;
}

inline std::uint64_t maxInMemoryFileBytes() noexcept {
    return inMemoryFileByteLimitStorage();
}

inline std::uint64_t maxDecodedBytes() noexcept {
    return decodedByteLimitStorage();
}

// Temporarily narrows parser allocations on the current thread. This lets a
// caller apply a smaller working-set budget without weakening the normal
// process-wide safety ceilings or affecting concurrent loads.
class ScopedResourceLimits {
public:
    ScopedResourceLimits(std::uint64_t maxFileBytes, std::uint64_t maxDecodedImageBytes) noexcept
        : previousFileBytes_(inMemoryFileByteLimitStorage()),
          previousDecodedBytes_(decodedByteLimitStorage()) {
        inMemoryFileByteLimitStorage() =
            maxFileBytes < previousFileBytes_ ? maxFileBytes : previousFileBytes_;
        decodedByteLimitStorage() =
            maxDecodedImageBytes < previousDecodedBytes_ ? maxDecodedImageBytes : previousDecodedBytes_;
    }

    ~ScopedResourceLimits() {
        inMemoryFileByteLimitStorage() = previousFileBytes_;
        decodedByteLimitStorage() = previousDecodedBytes_;
    }

    ScopedResourceLimits(const ScopedResourceLimits&) = delete;
    ScopedResourceLimits& operator=(const ScopedResourceLimits&) = delete;

private:
    std::uint64_t previousFileBytes_;
    std::uint64_t previousDecodedBytes_;
};

inline bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
    result = left + right;
    return true;
}

inline bool checkedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) return false;
    result = left * right;
    return true;
}

inline bool rangeWithin(std::uint64_t containerSize,
                        std::uint64_t offset,
                        std::uint64_t length) noexcept {
    return offset <= containerSize && length <= containerSize - offset;
}

} // namespace neotpc::texture::parser
