#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace neotpc::texture::internal_image {

struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::string encoding;
};

RgbaImage decodePng(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encodePngRgba(std::uint32_t width,
                                         std::uint32_t height,
                                         const std::vector<std::uint8_t>& rgba);

RgbaImage decodeJpeg(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encodeJpegRgb(std::uint32_t width,
                                         std::uint32_t height,
                                         const std::vector<std::uint8_t>& rgba,
                                         std::uint8_t quality);

} // namespace neotpc::texture::internal_image
