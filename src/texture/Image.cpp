#include "texture/Image.hpp"
#include "texture/InternalImageCodecs.hpp"
#include "texture/FileUtil.hpp"
#include "texture/Error.hpp"
#include "texture/Operation.hpp"
#include "texture/ParserLimits.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <utility>

namespace neotpc::texture {
namespace {
using neoshared::texture::makeTextureLayer;
using neoshared::texture::textureFromRgba;
using neoshared::texture::composeTextureCanvas;

std::uint16_t readLE16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) throw TextureError("Unexpected end of file while reading uint16");
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t readLE32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw TextureError("Unexpected end of file while reading uint32");
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void appendLE16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void appendLE32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

std::uint8_t maskedChannelToByte(std::uint32_t pixel, std::uint32_t mask) {
    if (mask == 0) return 0;
    unsigned shift = 0;
    while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
    const std::uint32_t shiftedMask = mask >> shift;
    const std::uint32_t value = (pixel & mask) >> shift;
    return static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) * 255u + shiftedMask / 2u) /
                                     shiftedMask);
}

static bool looksLikePngHeader(const std::vector<std::uint8_t>& header) {
    static constexpr std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return header.size() >= 8 && std::equal(std::begin(sig), std::end(sig), header.begin());
}

static bool looksLikeJpegHeader(const std::vector<std::uint8_t>& header) {
    return header.size() >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF;
}

static bool looksLikeBmpHeader(const std::vector<std::uint8_t>& header) {
    if (header.size() < 54 || header[0] != 'B' || header[1] != 'M') return false;
    const std::uint32_t pixelOffset = static_cast<std::uint32_t>(header[10]) |
                                      (static_cast<std::uint32_t>(header[11]) << 8) |
                                      (static_cast<std::uint32_t>(header[12]) << 16) |
                                      (static_cast<std::uint32_t>(header[13]) << 24);
    const std::uint32_t dibSize = static_cast<std::uint32_t>(header[14]) |
                                  (static_cast<std::uint32_t>(header[15]) << 8) |
                                  (static_cast<std::uint32_t>(header[16]) << 16) |
                                  (static_cast<std::uint32_t>(header[17]) << 24);
    const std::uint16_t planes = static_cast<std::uint16_t>(header[26] | (header[27] << 8));
    const std::uint16_t bpp = static_cast<std::uint16_t>(header[28] | (header[29] << 8));
    const std::uint32_t compression = static_cast<std::uint32_t>(header[30]) |
                                      (static_cast<std::uint32_t>(header[31]) << 8) |
                                      (static_cast<std::uint32_t>(header[32]) << 16) |
                                      (static_cast<std::uint32_t>(header[33]) << 24);
    return pixelOffset >= 26 && dibSize >= 40 && planes == 1 && (compression == 0 || compression == 3) &&
           (bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
}

std::int32_t readLES32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::int32_t>(readLE32(bytes, offset));
}

TextureData readBmpTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& path,
                                        std::string txi) {
    if (!looksLikeBmpHeader(bytes)) throw TextureError("Invalid or unsupported BMP header");

    const std::uint32_t pixelOffset = readLE32(bytes, 10);
    const std::uint32_t dibSize = readLE32(bytes, 14);
    const std::int32_t signedWidth = readLES32(bytes, 18);
    const std::int32_t signedHeight = readLES32(bytes, 22);
    const std::uint16_t planes = readLE16(bytes, 26);
    const std::uint16_t bpp = readLE16(bytes, 28);
    const std::uint32_t compression = readLE32(bytes, 30);
    const std::uint32_t colorsUsed = readLE32(bytes, 46);

    if (dibSize < 40 || planes != 1 || signedWidth <= 0 || signedHeight == 0) {
        throw TextureError("Unsupported BMP DIB header");
    }
    if (compression != 0 && compression != 3) {
        throw TextureError("Only uncompressed and BITFIELDS BMP files are supported");
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        throw TextureError("Only 1/4/8/16/24/32-bit BMP files are supported");
    }

    const std::uint32_t width = static_cast<std::uint32_t>(signedWidth);
    const bool topDown = signedHeight < 0;
    const std::uint32_t height = static_cast<std::uint32_t>(topDown ? -static_cast<std::int64_t>(signedHeight) : signedHeight);
    if (width == 0 || height == 0 || width > 0x8000u || height > 0x8000u) {
        throw TextureError("BMP dimensions are invalid or too large");
    }

    std::uint32_t redMask = 0;
    std::uint32_t greenMask = 0;
    std::uint32_t blueMask = 0;
    std::uint32_t alphaMask = 0;
    if (compression == 3) {
        std::size_t maskOffset = 14 + dibSize;
        if (dibSize >= 52) maskOffset = 14 + 40;
        if (maskOffset + 12 > bytes.size() || maskOffset + 12 > pixelOffset) {
            throw TextureError("BMP BITFIELDS masks are truncated");
        }
        redMask = readLE32(bytes, maskOffset + 0);
        greenMask = readLE32(bytes, maskOffset + 4);
        blueMask = readLE32(bytes, maskOffset + 8);
        if (maskOffset + 16 <= bytes.size() && maskOffset + 16 <= pixelOffset) {
            alphaMask = readLE32(bytes, maskOffset + 12);
        }
    } else if (bpp == 16) {
        redMask = 0x7C00u;
        greenMask = 0x03E0u;
        blueMask = 0x001Fu;
    } else if (bpp == 32) {
        redMask = 0x00FF0000u;
        greenMask = 0x0000FF00u;
        blueMask = 0x000000FFu;
        alphaMask = 0xFF000000u;
    }

    std::vector<std::array<std::uint8_t, 4>> palette;
    if (bpp <= 8) {
        const std::uint32_t defaultCount = bpp == 1 ? 2u : (bpp == 4 ? 16u : 256u);
        const std::uint32_t paletteCount = colorsUsed != 0 ? colorsUsed : defaultCount;
        const std::size_t paletteOffset = 14 + dibSize;
        if (paletteOffset + static_cast<std::size_t>(paletteCount) * 4 > bytes.size() ||
            paletteOffset + static_cast<std::size_t>(paletteCount) * 4 > pixelOffset) {
            throw TextureError("BMP palette is truncated");
        }
        palette.reserve(paletteCount);
        for (std::uint32_t i = 0; i < paletteCount; ++i) {
            const std::size_t off = paletteOffset + static_cast<std::size_t>(i) * 4;
            palette.push_back({bytes[off + 2], bytes[off + 1], bytes[off + 0], 255});
        }
    }

    const std::uint64_t rowBits = static_cast<std::uint64_t>(width) * bpp;
    const std::size_t stride = static_cast<std::size_t>(((rowBits + 31u) / 32u) * 4u);
    if (pixelOffset > bytes.size() || static_cast<std::uint64_t>(pixelOffset) + static_cast<std::uint64_t>(stride) * height > bytes.size()) {
        throw TextureError("BMP pixel data is truncated");
    }

    TextureLayer layer = makeTextureLayer(width, height);
    for (std::uint32_t fileY = 0; fileY < height; ++fileY) {
        const std::uint32_t y = topDown ? fileY : (height - 1 - fileY);
        const std::size_t row = static_cast<std::size_t>(pixelOffset) + static_cast<std::size_t>(fileY) * stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t dst = (static_cast<std::size_t>(y) * width + x) * 4u;
            if (bpp <= 8) {
                std::uint8_t index = 0;
                if (bpp == 1) index = static_cast<std::uint8_t>((bytes[row + x / 8] >> (7 - (x % 8))) & 0x1u);
                else if (bpp == 4) index = static_cast<std::uint8_t>((bytes[row + x / 2] >> ((x % 2) ? 0 : 4)) & 0x0Fu);
                else index = bytes[row + x];
                if (index >= palette.size()) throw TextureError("BMP palette index is out of range");
                layer.rgba[dst + 0] = palette[index][0];
                layer.rgba[dst + 1] = palette[index][1];
                layer.rgba[dst + 2] = palette[index][2];
                layer.rgba[dst + 3] = 255;
            } else if (bpp == 24) {
                const std::size_t src = row + static_cast<std::size_t>(x) * 3u;
                layer.rgba[dst + 0] = bytes[src + 2];
                layer.rgba[dst + 1] = bytes[src + 1];
                layer.rgba[dst + 2] = bytes[src + 0];
                layer.rgba[dst + 3] = 255;
            } else if (bpp == 16) {
                const std::size_t src = row + static_cast<std::size_t>(x) * 2u;
                const std::uint32_t pixel = readLE16(bytes, src);
                layer.rgba[dst + 0] = maskedChannelToByte(pixel, redMask);
                layer.rgba[dst + 1] = maskedChannelToByte(pixel, greenMask);
                layer.rgba[dst + 2] = maskedChannelToByte(pixel, blueMask);
                layer.rgba[dst + 3] = alphaMask ? maskedChannelToByte(pixel, alphaMask) : 255;
            } else if (bpp == 32) {
                const std::size_t src = row + static_cast<std::size_t>(x) * 4u;
                const std::uint32_t pixel = readLE32(bytes, src);
                layer.rgba[dst + 0] = maskedChannelToByte(pixel, redMask);
                layer.rgba[dst + 1] = maskedChannelToByte(pixel, greenMask);
                layer.rgba[dst + 2] = maskedChannelToByte(pixel, blueMask);
                layer.rgba[dst + 3] = alphaMask ? maskedChannelToByte(pixel, alphaMask) : 255;
            }
        }
    }

    return textureFromRgba(TextureFileKind::Bmp, path, std::move(layer),
                                   std::to_string(bpp) + "-bit BMP", std::move(txi));
}

TextureData readPngTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& path,
                                        std::string txi) {
    if (!looksLikePngHeader(bytes)) throw TextureError("Invalid PNG header");
    auto image = internal_image::decodePng(bytes);
    TextureLayer layer = makeTextureLayer(image.width, image.height);
    layer.rgba = std::move(image.rgba);
    return textureFromRgba(TextureFileKind::Png, path, std::move(layer),
                                   image.encoding.empty() ? std::string("PNG via libspng") : image.encoding,
                                   std::move(txi));
}

TextureData readJpegTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                         const std::filesystem::path& path,
                                         std::string txi) {
    if (!looksLikeJpegHeader(bytes)) throw TextureError("Invalid JPEG header");
    auto image = internal_image::decodeJpeg(bytes);
    TextureLayer layer = makeTextureLayer(image.width, image.height);
    layer.rgba = std::move(image.rgba);
    return textureFromRgba(TextureFileKind::Jpeg, path, std::move(layer),
                                   image.encoding.empty() ? std::string("JPEG via libjpeg API") : image.encoding,
                                   std::move(txi));
}

static std::vector<std::uint8_t> encodePngTexture(const TextureData& texture) {
    if (!texture.hasPixels()) throw TextureError("Cannot write PNG without pixel data");
    const TextureLayer canvas = composeTextureCanvas(texture);
    return internal_image::encodePngRgba(canvas.width, canvas.height, canvas.rgba);
}

static std::vector<std::uint8_t> encodeJpegTexture(const TextureData& texture, const TextureSaveOptions& options) {
    if (!texture.hasPixels()) throw TextureError("Cannot write JPEG without pixel data");
    const TextureLayer canvas = composeTextureCanvas(texture);
    return internal_image::encodeJpegRgb(canvas.width,
                                                         canvas.height,
                                                         canvas.rgba,
                                                         static_cast<std::uint8_t>(std::max<int>(1, std::min<int>(100, options.jpegQuality))));
}

static std::vector<std::uint8_t> encodeBmpTexture(const TextureData& texture) {
    if (!texture.hasPixels()) throw TextureError("Cannot write BMP without pixel data");
    const TextureLayer canvas = composeTextureCanvas(texture);
    if (canvas.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        canvas.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        throw TextureError("BMP dimensions exceed signed 32-bit header limits");
    }

    const std::size_t stride = static_cast<std::size_t>(canvas.width) * 4u;
    const std::uint32_t pixelBytes = static_cast<std::uint32_t>(stride * canvas.height);
    const std::uint32_t pixelOffset = 14u + 40u;
    std::vector<std::uint8_t> out;
    out.reserve(pixelOffset + pixelBytes);
    out.push_back('B');
    out.push_back('M');
    appendLE32(out, pixelOffset + pixelBytes);
    appendLE16(out, 0);
    appendLE16(out, 0);
    appendLE32(out, pixelOffset);
    appendLE32(out, 40); // BITMAPINFOHEADER
    appendLE32(out, canvas.width);
    appendLE32(out, canvas.height); // positive height = bottom-up rows
    appendLE16(out, 1);
    appendLE16(out, 32);
    appendLE32(out, 0); // BI_RGB
    appendLE32(out, pixelBytes);
    appendLE32(out, 2835); // 72 DPI
    appendLE32(out, 2835);
    appendLE32(out, 0);
    appendLE32(out, 0);
    for (std::uint32_t fileY = 0; fileY < canvas.height; ++fileY) {
        const std::uint32_t y = canvas.height - 1 - fileY;
        for (std::uint32_t x = 0; x < canvas.width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * canvas.width + x) * 4u;
            out.push_back(canvas.rgba[src + 2]);
            out.push_back(canvas.rgba[src + 1]);
            out.push_back(canvas.rgba[src + 0]);
            out.push_back(canvas.rgba[src + 3]);
        }
    }
    return out;
}

} // namespace

TextureData loadTexture(const std::filesystem::path& path) {
    const auto bytes = readFileBytes(path);
    std::string txi;
    if (extensionLower(path) != "txi") {
        const auto gameKind = neoshared::texture::detectGameTextureKind(bytes);
        const bool raster = looksLikePngHeader(bytes) || looksLikeJpegHeader(bytes) || looksLikeBmpHeader(bytes);
        if (gameKind == TextureFileKind::Dds || gameKind == TextureFileKind::Tga || raster)
            txi = neoshared::texture::readTextureSidecar(path);
    }
    return loadTextureBytes(bytes, path, std::move(txi));
}

TextureData loadTextureBytes(const std::vector<std::uint8_t>& bytes,
                             const std::filesystem::path& path,
                             std::string txi) {
    checkOperation();
    // Explicit TXI documents take precedence over image signature detection.
    if (extensionLower(path) == "txi")
        return neoshared::texture::decodeTextureBytes(bytes, path, std::move(txi));
    if (neoshared::texture::detectGameTextureKind(bytes) == TextureFileKind::Dds)
        return neoshared::texture::decodeTextureBytes(bytes, path, std::move(txi));
    if (looksLikePngHeader(bytes)) return readPngTextureBytesInternal(bytes, path, std::move(txi));
    if (looksLikeJpegHeader(bytes)) return readJpegTextureBytesInternal(bytes, path, std::move(txi));
    if (looksLikeBmpHeader(bytes)) return readBmpTextureBytesInternal(bytes, path, std::move(txi));
    return neoshared::texture::decodeTextureBytes(bytes, path, std::move(txi));
}

EncodedTexture encodeTexture(const TextureData& input, const std::filesystem::path& output,
                             const TextureSaveOptions& options) {
    checkOperation();
    const auto ext = extensionLower(output);
    const bool png = ext == "png";
    const bool jpeg = ext == "jpg" || ext == "jpeg" || ext == "jpe";
    const bool bmp = ext == "bmp";
    if (!png && !jpeg && !bmp)
        return neoshared::texture::encodeTextureBytes(input, output, options);

    TextureData texture = input;
    if (texture.cubeMap && !parseTxiFeatures(texture.txi).cube)
        texture.txi = setTxiValue(texture.txi, "cube", "1");
    if (options.flipXOnSave) flipTextureHorizontal(texture);
    if (options.flipYOnSave) flipTextureVertical(texture);
    EncodedTexture result;
    if (!texture.txi.empty()) result.sidecar.emplace(texture.txi.begin(), texture.txi.end());
    if (png) result.image = encodePngTexture(texture);
    else if (jpeg) result.image = encodeJpegTexture(texture, options);
    else result.image = encodeBmpTexture(texture);
    checkOperation();
    return result;
}

void saveTexture(const TextureData& texture, const std::filesystem::path& output,
                 const TextureSaveOptions& options) {
    const auto encoded = encodeTexture(texture, output, options);
    saveEncodedTexture(encoded.image, encoded.sidecar, output);
}

std::string imageCodecSupportReport() {
    std::ostringstream out;
    out << "Texture image codec support:\n";
    out << "  TPC: storage-checked raw/grayscale/DXT1/DXT5 output; square animation frames; base-only rectangular static output; legacy/Xbox input remains readable\n";
    out << "  TXB: built-in read-only conversion support for Xbox swizzled BGRA/grayscale and DXT1/DXT5 textures\n";
    out << "  DDS: game 20-byte-header DXT1/DXT5 output (default for new files), or explicit standard DDS interchange; imported dialect is retained\n";
    out << "  TGA: origin-aware input, bottom-origin game output, raw cube-strip conversion\n";
    out << "  BMP: built-in read/write for 1/4/8-bit paletted and 16/24/32-bit truecolor BMP\n";
    out << "  PNG: provided by the external libspng dependency\n";
    out << "  JPEG/JPG: provided through the libjpeg API (vcpkg uses libjpeg-turbo); alpha is dropped on JPEG output\n";
    out << "  TXI: built-in read/write, sidecar handling, and TPC embedding\n";
    return out.str();
}

// Shared by the document and batch paths; these checks do not encode the image.
texture::TextureSaveOptions importedTextureOptions(const texture::TextureData& data) {
    texture::TextureSaveOptions o;
    o.compression = data.preferredCompression == texture::TextureCompression::SwizzledBgra
        ? texture::TextureCompression::Auto : data.preferredCompression;
    o.generateMipmaps = (data.kind == texture::TextureFileKind::Tpc || data.kind == texture::TextureFileKind::Dds ||
                         data.kind == texture::TextureFileKind::Txb) ? data.sourceMipMapCount > 1 : true;
    o.mipmapPolicy = o.generateMipmaps ? texture::MipmapPolicy::Preserve : texture::MipmapPolicy::BaseOnly;
    o.alphaBlending = data.alphaBlending;
    return o;
}

std::string textureOutputIssue(const TextureData& texture, const std::filesystem::path& output,
                               const TextureSaveOptions& o) {
    const auto kind = kindForExtension(output);
    if (kind == TextureFileKind::Unknown || kind == TextureFileKind::Txb)
        return "Choose a supported output format.";
    if (kind == TextureFileKind::Txi) return {};
    if (!texture.hasPixels()) return "TXI-only input has no pixels to encode.";
    const bool tpc = kind == TextureFileKind::Tpc;
    const bool gameDds = kind == TextureFileKind::Dds && o.ddsDialect != DdsDialect::Standard &&
        !(o.ddsDialect == DdsDialect::Auto && texture.ddsDialect == DdsDialect::Standard);
    if (tpc && o.compression == TextureCompression::Dxt3) return "TPC supports raw, DXT1 or DXT5, not DXT3. Choose a supported compression.";
    if (gameDds && o.compression != TextureCompression::Auto && o.compression != TextureCompression::Dxt1 && o.compression != TextureCompression::Dxt5)
        return "Game DDS requires DXT1 or DXT5. Choose that compression or Standard DDS.";
    if ((tpc || gameDds) && o.compression == TextureCompression::Dxt1 && texture.hasAlpha)
        return "Game DXT1 cannot preserve transparency. Choose DXT5 (or explicitly make the pixels opaque).";
    const auto f = parseTxiFeatures(texture.txi);
    const bool cube = texture.cubeMap || f.cube;
    const bool animation = texture.animated || f.procedureType == "cycle";
    const bool mips = o.generateMipmaps && o.mipmapPolicy != MipmapPolicy::BaseOnly;
    if (gameDds && (cube || animation)) return "Game DDS supports one static surface. Use a compatible TPC or standard DDS instead.";
    if (tpc && (cube || animation) && !mips) return "TPC cubes and animations require complete mip chains. Enable mipmaps.";
    if (tpc && !cube && !animation && texture.canvasWidth != texture.canvasHeight && mips)
        return "Rectangular static TPC requires Base only. Select that mipmap policy, or choose Game DDS for a static mip chain.";
    if (tpc && !mips && f.mipmapSpecified && f.mipmap)
        return "Base-only TPC conflicts with TXI mipmap 1. Explicitly change the directive to mipmap 0.";
    if (tpc && (cube || animation) && f.mipmapSpecified && !f.mipmap)
        return "The TXI disables mipmapping, but this TPC cube/animation requires it. Change mipmap 0 explicitly.";
    return {};
}

} // namespace neotpc::texture
