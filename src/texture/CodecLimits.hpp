#pragma once
#include "texture/Error.hpp"
#include "texture/ParserLimits.hpp"
#include <cstdint>
#include <limits>
#include <string>
namespace neotpc::texture::internal_image {
constexpr std::uint32_t kMaxImageDimension = 0x8000u;

inline std::size_t ensureDimensions(std::uint32_t width, std::uint32_t height, const char* codec) {
    if (width == 0 || height == 0 || width > kMaxImageDimension || height > kMaxImageDimension) {
        throw TextureError(std::string(codec) + " dimensions are invalid or too large");
    }
    std::uint64_t pixels = 0;
    std::uint64_t bytes = 0;
    if (!parser::checkedMultiply(width, height, pixels) ||
        !parser::checkedMultiply(pixels, UINT64_C(4), bytes) ||
        bytes > parser::maxDecodedBytes() ||
        bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw TextureError(std::string(codec) + " decoded image exceeds the parser memory limit");
    }
    return static_cast<std::size_t>(bytes);
}

}
