#include "texture/Image.hpp"
#include "texture/InternalImageCodecs.hpp"
#include "texture/Txi.hpp"

#include "texture/FileUtil.hpp"
#include "texture/Error.hpp"
#include "texture/ParserLimits.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <csetjmp>
#include <cstdlib>
#include <exception>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace neotpc::texture {
namespace {

constexpr std::uint8_t kTpcEncodingGray = 0x01;
constexpr std::uint8_t kTpcEncodingRgb = 0x02;
constexpr std::uint8_t kTpcEncodingRgba = 0x04;
constexpr std::uint8_t kTpcEncodingSwizzledBgra = 0x0C;
constexpr std::uint8_t kTxbEncodingBgra = 0x04;
constexpr std::uint8_t kTxbEncodingGray = 0x09;
constexpr std::uint8_t kTxbEncodingDxt1 = 0x0A;
constexpr std::uint8_t kTxbEncodingDxt5 = 0x0C;
constexpr std::uint64_t kMaxTextureAnimationFrames = 65536;

constexpr std::array<CubeFace, 6> kAllCubeFaces{{
    CubeFace::PositiveX,
    CubeFace::NegativeX,
    CubeFace::PositiveY,
    CubeFace::NegativeY,
    CubeFace::PositiveZ,
    CubeFace::NegativeZ,
}};

std::uint32_t ddsCubeFaceBit(CubeFace face) {
    switch (face) {
    case CubeFace::PositiveX: return 0x00000400u;
    case CubeFace::NegativeX: return 0x00000800u;
    case CubeFace::PositiveY: return 0x00001000u;
    case CubeFace::NegativeY: return 0x00002000u;
    case CubeFace::PositiveZ: return 0x00004000u;
    case CubeFace::NegativeZ: return 0x00008000u;
    }
    return 0;
}

std::vector<CubeFace> allCubeFaces() {
    return {kAllCubeFaces.begin(), kAllCubeFaces.end()};
}

std::size_t checkedRgbaByteCount(std::uint64_t width, std::uint64_t height, const char* context) {
    if (width == 0 || height == 0) throw TextureError(std::string(context) + " has zero dimensions");
    if (width >= UINT64_C(0x8000) || height >= UINT64_C(0x8000)) {
        throw TextureError(std::string(context) + " dimensions exceed Odyssey limits");
    }
    std::uint64_t pixels = 0;
    std::uint64_t bytes = 0;
    if (!parser::checkedMultiply(width, height, pixels) ||
        !parser::checkedMultiply(pixels, UINT64_C(4), bytes) ||
        bytes > parser::maxDecodedBytes() ||
        bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw TextureError(std::string(context) + " exceeds the decoded-image memory limit");
    }
    return static_cast<std::size_t>(bytes);
}

std::uint64_t checkedMipChainDecodedByteCount(std::uint32_t width,
                                              std::uint32_t height,
                                              std::uint32_t mipCount,
                                              std::uint32_t layerCount,
                                              const char* context) {
    std::uint64_t oneLayer = 0;
    for (std::uint32_t mip = 0; mip < mipCount; ++mip) {
        std::uint64_t next = 0;
        if (!parser::checkedAdd(oneLayer, checkedRgbaByteCount(width, height, context), next)) {
            throw TextureError(std::string(context) + " mipmap memory size overflows");
        }
        oneLayer = next;
        width = std::max<std::uint32_t>(1, width / 2);
        height = std::max<std::uint32_t>(1, height / 2);
    }
    std::uint64_t total = 0;
    if (!parser::checkedMultiply(oneLayer, layerCount, total) || total > parser::maxDecodedBytes()) {
        throw TextureError(std::string(context) + " exceeds the decoded-image memory limit");
    }
    return total;
}

std::uint64_t checkedAnimationFrameCount(const TxiFeatures& features) {
    if (features.numX == 0 || features.numY == 0) {
        throw TextureError("TXI animation grid dimensions must be positive");
    }
    std::uint64_t frameCount = 0;
    if (!parser::checkedMultiply(features.numX, features.numY, frameCount) ||
        frameCount == 0 || frameCount > kMaxTextureAnimationFrames) {
        throw TextureError("TXI animation grid exceeds the limit of 65536 frames");
    }
    return frameCount;
}

std::uint32_t checkedDimensionProduct(std::uint32_t left, std::uint32_t right, const char* context) {
    std::uint64_t product = 0;
    if (!parser::checkedMultiply(left, right, product) || product > std::numeric_limits<std::uint32_t>::max()) {
        throw TextureError(std::string(context) + " dimension overflows");
    }
    return static_cast<std::uint32_t>(product);
}

void validateLayerStorage(const TextureLayer& layer, const char* context) {
    const auto expected = checkedRgbaByteCount(layer.width, layer.height, context);
    if (layer.rgba.size() != expected) {
        throw TextureError(std::string(context) + " pixel buffer size does not match dimensions");
    }
    std::uint32_t expectedWidth = layer.width;
    std::uint32_t expectedHeight = layer.height;
    for (const auto& mip : layer.mipmaps) {
        expectedWidth = std::max<std::uint32_t>(1, expectedWidth / 2);
        expectedHeight = std::max<std::uint32_t>(1, expectedHeight / 2);
        if (mip.width != expectedWidth || mip.height != expectedHeight) {
            throw TextureError(std::string(context) + " mipmap dimensions do not form a valid chain");
        }
        if (!mip.mipmaps.empty()) {
            throw TextureError(std::string(context) + " contains a nested mipmap chain");
        }
        const auto mipExpected = checkedRgbaByteCount(mip.width, mip.height, context);
        if (mip.rgba.size() != mipExpected) {
            throw TextureError(std::string(context) + " mipmap pixel buffer size does not match dimensions");
        }
    }
}

void copyRgbaRowChecked(const TextureLayer& source,
                        std::size_t sourceOffset,
                        TextureLayer& destination,
                        std::size_t destinationOffset,
                        std::size_t byteCount) {
    if (sourceOffset > source.rgba.size() || byteCount > source.rgba.size() - sourceOffset ||
        destinationOffset > destination.rgba.size() || byteCount > destination.rgba.size() - destinationOffset) {
        throw TextureError("Texture row copy exceeds a source or destination buffer");
    }
    std::copy_n(source.rgba.begin() + static_cast<std::ptrdiff_t>(sourceOffset), byteCount,
                destination.rgba.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
}

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

float readLEFloat(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const std::uint32_t raw = readLE32(bytes, offset);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
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

void writeLE16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
    if (offset + 2 > out.size()) throw TextureError("Internal error writing uint16");
    out[offset] = static_cast<std::uint8_t>(value & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void writeLE32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    if (offset + 4 > out.size()) throw TextureError("Internal error writing uint32");
    out[offset] = static_cast<std::uint8_t>(value & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

void writeLEFloat(std::vector<std::uint8_t>& out, std::size_t offset, float value) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(value));
    writeLE32(out, offset, raw);
}

std::array<std::uint8_t, 4> unpack565(std::uint16_t value, std::uint8_t alpha = 255) {
    const std::uint8_t r5 = static_cast<std::uint8_t>((value >> 11) & 0x1F);
    const std::uint8_t g6 = static_cast<std::uint8_t>((value >> 5) & 0x3F);
    const std::uint8_t b5 = static_cast<std::uint8_t>(value & 0x1F);
    return {static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)),
            static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)),
            static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2)),
            alpha};
}

std::size_t dxt1Size(std::uint32_t width, std::uint32_t height) {
    return std::max<std::size_t>(8, ((width + 3) / 4) * ((height + 3) / 4) * 8);
}

std::size_t dxt5Size(std::uint32_t width, std::uint32_t height) {
    return std::max<std::size_t>(16, ((width + 3) / 4) * ((height + 3) / 4) * 16);
}

std::size_t bytesForEncoding(TextureCompression compression, std::uint32_t width, std::uint32_t height, bool hasAlpha) {
    switch (compression) {
    case TextureCompression::Dxt1: return dxt1Size(width, height);
    case TextureCompression::Dxt3:
    case TextureCompression::Dxt5: return dxt5Size(width, height);
    case TextureCompression::Gray: return static_cast<std::size_t>(width) * height;
    case TextureCompression::None: return static_cast<std::size_t>(width) * height * (hasAlpha ? 4 : 3);
    case TextureCompression::SwizzledBgra: return static_cast<std::size_t>(width) * height * 4;
    case TextureCompression::Auto: break;
    }
    return static_cast<std::size_t>(width) * height * 4;
}

std::string trim(std::string value) {
    auto first = value.begin();
    while (first != value.end() && std::isspace(static_cast<unsigned char>(*first))) ++first;
    auto last = value.end();
    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1)))) --last;
    return std::string(first, last);
}

std::string sanitizeTxiPayload(std::string value) {
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

bool iequals(const std::string& a, const std::string& b) {
    return asciiLower(a) == asciiLower(b);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    if (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        // Preserve text-ish behavior without manufacturing an extra key line.
    }
    return lines;
}

std::string firstTokenLower(const std::string& line, std::string* restOut = nullptr) {
    std::string stripped = trim(line);
    if (stripped.empty() || stripped[0] == '#' || stripped[0] == ';') {
        if (restOut) *restOut = {};
        return {};
    }
    std::istringstream input(stripped);
    std::string key;
    input >> key;
    std::string rest;
    std::getline(input, rest);
    if (restOut) *restOut = trim(rest);
    return asciiLower(key);
}

std::optional<std::uint32_t> parseU32Loose(const std::string& value) {
    const auto normalized = trim(value);
    if (normalized.empty() || normalized.front() == '+' || normalized.front() == '-') return std::nullopt;
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(normalized, &consumed, 0);
        if (consumed == normalized.size() && parsed <= std::numeric_limits<std::uint32_t>::max()) {
            return static_cast<std::uint32_t>(parsed);
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<double> parseDoubleLoose(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const auto trimmed = trim(value);
        const auto parsed = std::stod(trimmed, &consumed);
        if (consumed == trimmed.size() && std::isfinite(parsed)) return parsed;
    } catch (...) {
    }
    return std::nullopt;
}

bool parseTruthy(const std::string& value) {
    const std::string lower = asciiLower(trim(value));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "y" || lower == "t" || lower == "on";
}

bool parseFalsy(const std::string& value) {
    const std::string lower = asciiLower(trim(value));
    return lower == "0" || lower == "false" || lower == "no" || lower == "n" || lower == "f" || lower == "off";
}

std::uint8_t clampByte(double value) {
    return static_cast<std::uint8_t>(std::max(0.0, std::min(255.0, std::round(value))));
}

bool layerHasAlpha(const TextureLayer& layer) {
    const auto imageHasAlpha = [](const TextureLayer& image) {
        for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
            if (image.rgba[i] != 255) return true;
        }
        return false;
    };
    if (imageHasAlpha(layer)) return true;
    for (const auto& mip : layer.mipmaps) if (imageHasAlpha(mip)) return true;
    return false;
}

void refreshHasAlpha(TextureData& texture) {
    texture.hasAlpha = false;
    for (const auto& layer : texture.layers) {
        if (layerHasAlpha(layer)) {
            texture.hasAlpha = true;
            return;
        }
    }
}

TextureLayer makeLayer(std::uint32_t width, std::uint32_t height) {
    TextureLayer layer;
    layer.width = width;
    layer.height = height;
    layer.rgba.assign(checkedRgbaByteCount(width, height, "Texture layer"), 0);
    return layer;
}

TextureLayer cropLayer(const TextureLayer& source, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) {
    validateLayerStorage(source, "Texture crop source");
    if (x > source.width || width > source.width - x || y > source.height || height > source.height - y) {
        throw TextureError("Texture layer crop is outside the source image");
    }
    TextureLayer out = makeLayer(width, height);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::size_t srcOffset = (static_cast<std::size_t>(y + row) * source.width + x) * 4;
        const std::size_t dstOffset = static_cast<std::size_t>(row) * width * 4;
        copyRgbaRowChecked(source, srcOffset, out, dstOffset, static_cast<std::size_t>(width) * 4);
    }
    return out;
}

TextureLayer composeCanvas(const TextureData& texture) {
    if (texture.layers.empty()) {
        return {};
    }
    if (texture.layers.size() == 1) {
        validateLayerStorage(texture.layers.front(), "Texture layer");
        return texture.layers.front();
    }

    const TxiFeatures features = parseTxiFeatures(texture.txi);
    if (texture.animated && features.numX > 0 && features.numY > 0) {
        const auto& first = texture.layers.front();
        validateLayerStorage(first, "Texture animation frame");
        const auto frameCount = checkedAnimationFrameCount(features);
        if (texture.layers.size() != frameCount) {
            throw TextureError("Texture animation layer count does not match the TXI grid");
        }
        for (const auto& layer : texture.layers) {
            validateLayerStorage(layer, "Texture animation frame");
            if (layer.width != first.width || layer.height != first.height) {
                throw TextureError("Texture animation frames have mismatched dimensions");
            }
        }

        const auto canvasWidth = checkedDimensionProduct(first.width, features.numX, "TXI animation canvas width");
        const auto canvasHeight = checkedDimensionProduct(first.height, features.numY, "TXI animation canvas height");
        TextureLayer canvas = makeLayer(canvasWidth, canvasHeight);
        std::size_t index = 0;
        for (std::uint32_t yCell = 0; yCell < features.numY; ++yCell) {
            for (std::uint32_t xCell = 0; xCell < features.numX; ++xCell, ++index) {
                const auto& layer = texture.layers[index];
                for (std::uint32_t y = 0; y < layer.height; ++y) {
                    const std::uint64_t dstY = static_cast<std::uint64_t>(yCell) * first.height + y;
                    const std::uint64_t dstX = static_cast<std::uint64_t>(xCell) * first.width;
                    const std::uint64_t dstOffset64 = (dstY * canvas.width + dstX) * UINT64_C(4);
                    if (dstOffset64 > std::numeric_limits<std::size_t>::max()) {
                        throw TextureError("Texture animation copy offset overflows");
                    }
                    const auto dstOffset = static_cast<std::size_t>(dstOffset64);
                    const std::size_t srcOffset = static_cast<std::size_t>(y) * layer.width * 4;
                    copyRgbaRowChecked(layer, srcOffset, canvas, dstOffset, static_cast<std::size_t>(layer.width) * 4);
                }
            }
        }
        return canvas;
    }

    const auto& first = texture.layers.front();
    validateLayerStorage(first, "Texture layer");
    if (texture.layers.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw TextureError("Texture layer count exceeds the supported range");
    }
    const auto canvasHeight = checkedDimensionProduct(first.height,
                                                       static_cast<std::uint32_t>(texture.layers.size()),
                                                       "Texture canvas height");
    TextureLayer canvas = makeLayer(first.width, canvasHeight);
    for (std::size_t layerIndex = 0; layerIndex < texture.layers.size(); ++layerIndex) {
        const auto& layer = texture.layers[layerIndex];
        if (layer.width != first.width || layer.height != first.height) {
            throw TextureError("Cannot compose texture layers with mismatched dimensions");
        }
        validateLayerStorage(layer, "Texture layer");
        for (std::uint32_t y = 0; y < layer.height; ++y) {
            const std::size_t srcOffset = static_cast<std::size_t>(y) * layer.width * 4;
            const std::size_t dstOffset = (static_cast<std::size_t>(layerIndex) * layer.height + y) * canvas.width * 4;
            copyRgbaRowChecked(layer, srcOffset, canvas, dstOffset, static_cast<std::size_t>(layer.width) * 4);
        }
    }
    return canvas;
}

void applyTxiLayout(TextureData& texture, bool inferCubeFromAspect = false) {
    if (texture.layers.size() != 1 || texture.layers.front().rgba.empty()) {
        return;
    }
    const TxiFeatures features = parseTxiFeatures(texture.txi);
    const TextureLayer canvas = texture.layers.front();
    validateLayerStorage(canvas, "Texture source canvas");
    texture.canvasWidth = canvas.width;
    texture.canvasHeight = canvas.height;

    const bool explicitAnimation = asciiLower(features.procedureType) == "cycle" &&
                                   features.numX > 0 && features.numY > 0 && features.fps > 0.0;
    const bool inferredCube = inferCubeFromAspect && !explicitAnimation && canvas.width > 0 &&
                              canvas.height == static_cast<std::uint64_t>(canvas.width) * 6u;
    if ((features.cube || inferredCube) && canvas.width > 0) {
        const auto requiredHeight = checkedDimensionProduct(canvas.width, 6, "TXI cube canvas height");
        if (canvas.height == requiredHeight) {
            texture.layers.clear();
            texture.layers.reserve(6);
            for (std::uint32_t i = 0; i < 6; ++i) {
                texture.layers.push_back(cropLayer(canvas,
                                                   0,
                                                   checkedDimensionProduct(i, canvas.width, "TXI cube face offset"),
                                                   canvas.width,
                                                   canvas.width));
            }
            texture.cubeMap = true;
            texture.cubeFaces = allCubeFaces();
            texture.animated = false;
            texture.notes += inferredCube && !features.cube
                ? "A 6:1 vertical strip was inferred as a six-face cubemap. "
                : "TXI cube 1 split the source image into six vertical square layers. ";
            return;
        }
    }

    if (explicitAnimation) {
        const auto frameCount = checkedAnimationFrameCount(features);
        const std::uint32_t layerWidth = features.defaultWidth ? features.defaultWidth : canvas.width / features.numX;
        const std::uint32_t layerHeight = features.defaultHeight ? features.defaultHeight : canvas.height / features.numY;
        if (layerWidth == 0 || layerHeight == 0 || layerWidth > canvas.width || layerHeight > canvas.height) {
            throw TextureError("TXI animation frame dimensions are invalid for the source image");
        }
        const auto requiredWidth = checkedDimensionProduct(layerWidth, features.numX, "TXI animation frame grid width");
        const auto requiredHeight = checkedDimensionProduct(layerHeight, features.numY, "TXI animation frame grid height");
        if (requiredWidth > canvas.width || requiredHeight > canvas.height) {
            throw TextureError("TXI animation grid does not fit within the source image");
        }

        texture.layers.clear();
        texture.layers.reserve(static_cast<std::size_t>(frameCount));
        for (std::uint32_t yCell = 0; yCell < features.numY; ++yCell) {
            for (std::uint32_t xCell = 0; xCell < features.numX; ++xCell) {
                texture.layers.push_back(cropLayer(canvas,
                                                   checkedDimensionProduct(xCell, layerWidth, "TXI animation frame X offset"),
                                                   checkedDimensionProduct(yCell, layerHeight, "TXI animation frame Y offset"),
                                                   layerWidth,
                                                   layerHeight));
            }
        }
        texture.animated = texture.layers.size() > 1;
        texture.cubeMap = false;
        texture.cubeFaces.clear();
        if (texture.animated) {
            texture.notes += "TXI proceduretype cycle split the source image into animation frames. ";
        }
    }
}

TextureCompression chooseAutoCompression(const TextureData& texture, const TextureSaveOptions& options) {
    if (options.compression != TextureCompression::Auto) {
        return options.compression;
    }
    if (texture.preferredCompression == TextureCompression::SwizzledBgra) {
        return TextureCompression::SwizzledBgra;
    }
    const TxiFeatures features = parseTxiFeatures(texture.txi);
    if (features.isBumpMap && features.compressTextureSpecified && !features.compressTexture &&
        !iequals(features.procedureType, "cycle")) {
        return TextureCompression::None;
    }
    if (!texture.layers.empty()) {
        bool gray = true;
        const auto& rgba = texture.layers.front().rgba;
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
            if (rgba[i] != rgba[i + 1] || rgba[i] != rgba[i + 2]) {
                gray = false;
                break;
            }
        }
        if (gray && features.isBumpMap) {
            return TextureCompression::Gray;
        }
    }
    return texture.hasAlpha ? TextureCompression::Dxt5 : TextureCompression::Dxt1;
}

TextureLayer flipLayerVerticalCopy(TextureLayer layer) {
    if (layer.width == 0 || layer.height == 0) return layer;
    const std::size_t pitch = static_cast<std::size_t>(layer.width) * 4;
    std::vector<std::uint8_t> row(pitch);
    for (std::uint32_t y = 0; y < layer.height / 2; ++y) {
        const std::uint32_t opposite = layer.height - 1 - y;
        auto a = layer.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * pitch);
        auto b = layer.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(opposite) * pitch);
        std::copy_n(a, pitch, row.begin());
        std::copy_n(b, pitch, a);
        std::copy_n(row.begin(), pitch, b);
    }
    for (auto& mip : layer.mipmaps) mip = flipLayerVerticalCopy(std::move(mip));
    return layer;
}

TextureLayer flipLayerHorizontalCopy(TextureLayer layer) {
    for (std::uint32_t y = 0; y < layer.height; ++y) {
        for (std::uint32_t x = 0; x < layer.width / 2; ++x) {
            const std::size_t a = (static_cast<std::size_t>(y) * layer.width + x) * 4;
            const std::size_t b = (static_cast<std::size_t>(y) * layer.width + (layer.width - 1 - x)) * 4;
            for (std::size_t c = 0; c < 4; ++c) std::swap(layer.rgba[a + c], layer.rgba[b + c]);
        }
    }
    for (auto& mip : layer.mipmaps) mip = flipLayerHorizontalCopy(std::move(mip));
    return layer;
}

bool isPowerOfTwo(std::uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::uint32_t swizzledPixelOffset(std::uint32_t x,
                                  std::uint32_t y,
                                  std::uint32_t width,
                                  std::uint32_t height) {
    std::uint32_t widthBits = 0;
    std::uint32_t heightBits = 0;
    for (std::uint32_t value = width; value > 1; value >>= 1) ++widthBits;
    for (std::uint32_t value = height; value > 1; value >>= 1) ++heightBits;
    std::uint32_t offset = 0;
    std::uint32_t shift = 0;
    while (widthBits != 0 || heightBits != 0) {
        if (widthBits != 0) {
            offset |= (x & 1u) << shift++;
            x >>= 1;
            --widthBits;
        }
        if (heightBits != 0) {
            offset |= (y & 1u) << shift++;
            y >>= 1;
            --heightBits;
        }
    }
    return offset;
}

TextureLayer rotateLayer90Copy(TextureLayer layer, unsigned times) {
    times %= 4;
    if (times == 0) return layer;
    TextureLayer rotated = makeLayer((times & 1u) ? layer.height : layer.width,
                                     (times & 1u) ? layer.width : layer.height);
    for (std::uint32_t y = 0; y < layer.height; ++y) {
        for (std::uint32_t x = 0; x < layer.width; ++x) {
            std::uint32_t dx = x;
            std::uint32_t dy = y;
            if (times == 1) {
                dx = layer.height - 1 - y;
                dy = x;
            } else if (times == 2) {
                dx = layer.width - 1 - x;
                dy = layer.height - 1 - y;
            } else {
                dx = y;
                dy = layer.width - 1 - x;
            }
            const std::size_t source = (static_cast<std::size_t>(y) * layer.width + x) * 4;
            const std::size_t destination = (static_cast<std::size_t>(dy) * rotated.width + dx) * 4;
            std::copy_n(layer.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4,
                        rotated.rgba.begin() + static_cast<std::ptrdiff_t>(destination));
        }
    }
    rotated.mipmaps.reserve(layer.mipmaps.size());
    for (auto& mip : layer.mipmaps) rotated.mipmaps.push_back(rotateLayer90Copy(std::move(mip), times));
    return rotated;
}

void normalizeTpcCubeMap(std::vector<TextureLayer>& layers) {
    if (layers.size() != 6) throw TextureError("TPC cubemap must contain exactly six faces");
    std::swap(layers[0], layers[1]);
    // NeoTPC stores top-left-oriented pixels, which reverses the clockwise
    // convention used by the original bottom-left renderer.
    static constexpr unsigned rotations[6] = {3, 1, 0, 2, 2, 0};
    for (std::size_t index = 0; index < layers.size(); ++index) {
        layers[index] = rotateLayer90Copy(std::move(layers[index]), rotations[index]);
    }
}

void denormalizeTpcCubeMap(std::vector<TextureLayer>& layers) {
    if (layers.size() != 6) throw TextureError("TPC cubemap must contain exactly six faces");
    static constexpr unsigned inverseRotations[6] = {1, 3, 0, 2, 2, 0};
    for (std::size_t index = 0; index < layers.size(); ++index) {
        layers[index] = rotateLayer90Copy(std::move(layers[index]), inverseRotations[index]);
    }
    std::swap(layers[0], layers[1]);
}

std::vector<TextureLayer> withSaveFlips(const std::vector<TextureLayer>& layers, const TextureSaveOptions& options) {
    std::vector<TextureLayer> out = layers;
    for (auto& layer : out) {
        if (options.flipXOnSave) layer = flipLayerHorizontalCopy(std::move(layer));
        if (options.flipYOnSave) layer = flipLayerVerticalCopy(std::move(layer));
    }
    return out;
}

std::array<std::uint8_t, 4> pixelAtClamped(const TextureLayer& layer, int x, int y) {
    x = std::max(0, std::min<int>(x, static_cast<int>(layer.width) - 1));
    y = std::max(0, std::min<int>(y, static_cast<int>(layer.height) - 1));
    const std::size_t offset = (static_cast<std::size_t>(y) * layer.width + static_cast<std::uint32_t>(x)) * 4;
    return {layer.rgba[offset], layer.rgba[offset + 1], layer.rgba[offset + 2], layer.rgba[offset + 3]};
}

double cubicInterpolate(double p0, double p1, double p2, double p3, double t) {
    return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
}

TextureLayer downsampleLayer(const TextureLayer& source, bool bicubic) {
    const std::uint32_t dstW = std::max<std::uint32_t>(1, source.width / 2);
    const std::uint32_t dstH = std::max<std::uint32_t>(1, source.height / 2);
    TextureLayer out = makeLayer(dstW, dstH);

    for (std::uint32_t y = 0; y < dstH; ++y) {
        for (std::uint32_t x = 0; x < dstW; ++x) {
            const std::size_t dst = (static_cast<std::size_t>(y) * dstW + x) * 4;
            if (!bicubic) {
                const std::uint32_t sx = x * 2;
                const std::uint32_t sy = y * 2;
                for (std::size_t c = 0; c < 4; ++c) {
                    unsigned sum = 0;
                    unsigned count = 0;
                    for (std::uint32_t yy = sy; yy < std::min(source.height, sy + 2); ++yy) {
                        for (std::uint32_t xx = sx; xx < std::min(source.width, sx + 2); ++xx) {
                            sum += source.rgba[(static_cast<std::size_t>(yy) * source.width + xx) * 4 + c];
                            ++count;
                        }
                    }
                    out.rgba[dst + c] = static_cast<std::uint8_t>((sum + count / 2) / std::max(1u, count));
                }
            } else {
                const double srcX = (static_cast<double>(x) + 0.5) * source.width / dstW - 0.5;
                const double srcY = (static_cast<double>(y) + 0.5) * source.height / dstH - 0.5;
                const int ix = static_cast<int>(std::floor(srcX));
                const int iy = static_cast<int>(std::floor(srcY));
                const double tx = srcX - ix;
                const double ty = srcY - iy;
                for (std::size_t c = 0; c < 4; ++c) {
                    double rows[4] = {};
                    for (int row = -1; row <= 2; ++row) {
                        double vals[4] = {};
                        for (int col = -1; col <= 2; ++col) {
                            vals[col + 1] = pixelAtClamped(source, ix + col, iy + row)[c];
                        }
                        rows[row + 1] = cubicInterpolate(vals[0], vals[1], vals[2], vals[3], tx);
                    }
                    out.rgba[dst + c] = clampByte(cubicInterpolate(rows[0], rows[1], rows[2], rows[3], ty));
                }
            }
        }
    }
    return out;
}

std::vector<TextureLayer> generateMipmaps(TextureLayer layer, bool bicubic, bool includeOnlyBase = false) {
    std::vector<TextureLayer> mipmaps;
    layer.mipmaps.clear();
    mipmaps.push_back(std::move(layer));
    if (includeOnlyBase) return mipmaps;
    while (mipmaps.back().width > 1 || mipmaps.back().height > 1) {
        mipmaps.push_back(downsampleLayer(mipmaps.back(), bicubic));
    }
    return mipmaps;
}

std::array<std::array<std::uint8_t, 4>, 4> makeDxtColorPalette(std::uint16_t c0,
                                                               std::uint16_t c1,
                                                               bool fourColorOnly) {
    std::array<std::array<std::uint8_t, 4>, 4> colors{};
    colors[0] = unpack565(c0);
    colors[1] = unpack565(c1);
    if (fourColorOnly || c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            colors[2][i] = static_cast<std::uint8_t>((2 * colors[0][i] + colors[1][i] + 1) / 3);
            colors[3][i] = static_cast<std::uint8_t>((colors[0][i] + 2 * colors[1][i] + 1) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            colors[2][i] = static_cast<std::uint8_t>((colors[0][i] + colors[1][i] + 1) / 2);
            colors[3][i] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }
    return colors;
}

void decodeDxtColorBlock(const std::uint8_t* block, std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                         std::uint32_t xBase, std::uint32_t yBase, bool fourColorOnly) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(block[0] | (block[1] << 8));
    const std::uint16_t c1 = static_cast<std::uint16_t>(block[2] | (block[3] << 8));
    const auto colors = makeDxtColorPalette(c0, c1, fourColorOnly);
    const std::uint32_t indices = static_cast<std::uint32_t>(block[4]) |
                                  (static_cast<std::uint32_t>(block[5]) << 8) |
                                  (static_cast<std::uint32_t>(block[6]) << 16) |
                                  (static_cast<std::uint32_t>(block[7]) << 24);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            if (xBase + x >= width || yBase + y >= height) continue;
            const std::uint32_t index = (indices >> (2 * (4 * y + x))) & 0x3u;
            const std::size_t dst = (static_cast<std::size_t>(yBase + y) * width + (xBase + x)) * 4;
            rgba[dst + 0] = colors[index][0];
            rgba[dst + 1] = colors[index][1];
            rgba[dst + 2] = colors[index][2];
            rgba[dst + 3] = colors[index][3];
        }
    }
}

TextureLayer decodeDxt1(const std::uint8_t* data, std::size_t size, std::uint32_t width, std::uint32_t height) {
    TextureLayer layer = makeLayer(width, height);
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    std::size_t offset = 0;
    for (std::uint32_t by = 0; by < blocksY; ++by) {
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            if (offset + 8 > size) throw TextureError("DXT1 payload is truncated");
            decodeDxtColorBlock(data + offset, layer.rgba.data(), width, height, bx * 4, by * 4, false);
            offset += 8;
        }
    }
    return layer;
}

TextureLayer decodeDxt3(const std::uint8_t* data, std::size_t size, std::uint32_t width, std::uint32_t height) {
    TextureLayer layer = makeLayer(width, height);
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    std::size_t offset = 0;
    for (std::uint32_t by = 0; by < blocksY; ++by) {
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            if (offset + 16 > size) throw TextureError("DXT3 payload is truncated");
            decodeDxtColorBlock(data + offset + 8, layer.rgba.data(), width, height, bx * 4, by * 4, true);
            for (std::uint32_t py = 0; py < 4; ++py) {
                const std::uint16_t rowAlpha = static_cast<std::uint16_t>(data[offset + py * 2] | (data[offset + py * 2 + 1] << 8));
                for (std::uint32_t px = 0; px < 4; ++px) {
                    if (bx * 4 + px >= width || by * 4 + py >= height) continue;
                    const std::uint8_t a4 = static_cast<std::uint8_t>((rowAlpha >> (px * 4)) & 0xF);
                    layer.rgba[(static_cast<std::size_t>(by * 4 + py) * width + (bx * 4 + px)) * 4 + 3] = static_cast<std::uint8_t>((a4 << 4) | a4);
                }
            }
            offset += 16;
        }
    }
    return layer;
}

TextureLayer decodeDxt5(const std::uint8_t* data, std::size_t size, std::uint32_t width, std::uint32_t height) {
    TextureLayer layer = makeLayer(width, height);
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    std::size_t offset = 0;
    for (std::uint32_t by = 0; by < blocksY; ++by) {
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            if (offset + 16 > size) throw TextureError("DXT5 payload is truncated");
            decodeDxtColorBlock(data + offset + 8, layer.rgba.data(), width, height, bx * 4, by * 4, true);
            std::array<std::uint8_t, 8> alpha{};
            alpha[0] = data[offset];
            alpha[1] = data[offset + 1];
            if (alpha[0] > alpha[1]) {
                for (int i = 2; i < 8; ++i) {
                    alpha[i] = static_cast<std::uint8_t>(((8 - i) * alpha[0] + (i - 1) * alpha[1] + 3) / 7);
                }
            } else {
                for (int i = 2; i < 6; ++i) {
                    alpha[i] = static_cast<std::uint8_t>(((6 - i) * alpha[0] + (i - 1) * alpha[1] + 2) / 5);
                }
                alpha[6] = 0;
                alpha[7] = 255;
            }
            std::uint64_t alphaBits = 0;
            for (int i = 0; i < 6; ++i) alphaBits |= static_cast<std::uint64_t>(data[offset + 2 + i]) << (8 * i);
            for (std::uint32_t py = 0; py < 4; ++py) {
                for (std::uint32_t px = 0; px < 4; ++px) {
                    if (bx * 4 + px >= width || by * 4 + py >= height) continue;
                    const std::uint32_t aIndex = static_cast<std::uint32_t>((alphaBits >> (3 * (4 * py + px))) & 0x7);
                    layer.rgba[(static_cast<std::size_t>(by * 4 + py) * width + (bx * 4 + px)) * 4 + 3] = alpha[aIndex];
                }
            }
            offset += 16;
        }
    }
    return layer;
}

std::array<std::uint8_t, 4> blockPixel(const TextureLayer& layer, std::uint32_t x, std::uint32_t y) {
    x = std::min(x, layer.width - 1);
    y = std::min(y, layer.height - 1);
    const std::size_t offset = (static_cast<std::size_t>(y) * layer.width + x) * 4;
    return {layer.rgba[offset], layer.rgba[offset + 1], layer.rgba[offset + 2], layer.rgba[offset + 3]};
}

struct DxtColorFit {
    std::uint16_t c0 = 0;
    std::uint16_t c1 = 0;
    std::uint32_t indices = 0;
    std::uint64_t error = std::numeric_limits<std::uint64_t>::max();
};

struct DxtBlockOptions {
    DxtCompressionQuality quality = DxtCompressionQuality::High;
    DxtErrorMetric metric = DxtErrorMetric::Perceptual;
    bool weightColorByAlpha = false;
    std::uint8_t alphaThreshold = 128;
};

std::uint64_t colorDistance(const std::array<std::uint8_t, 4>& p,
                            const std::array<std::uint8_t, 4>& c,
                            DxtErrorMetric metric) {
    const int dr = static_cast<int>(p[0]) - static_cast<int>(c[0]);
    const int dg = static_cast<int>(p[1]) - static_cast<int>(c[1]);
    const int db = static_cast<int>(p[2]) - static_cast<int>(c[2]);
    if (metric == DxtErrorMetric::Uniform) {
        return static_cast<std::uint64_t>(dr * dr + dg * dg + db * db);
    }
    // Slight green bias matches human perception better than a raw RGB SSE.
    return static_cast<std::uint64_t>(3 * dr * dr + 4 * dg * dg + 2 * db * db);
}

std::uint16_t pack565FromDouble(const std::array<double, 3>& c) {
    auto quantize = [](double value, int maxCode) {
        value = std::max(0.0, std::min(255.0, value));
        return std::max(0, std::min(maxCode, static_cast<int>(std::lround(value * maxCode / 255.0))));
    };
    const int r = quantize(c[0], 31);
    const int g = quantize(c[1], 63);
    const int b = quantize(c[2], 31);
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}

std::array<int, 3> unpack565Components(std::uint16_t value) {
    return {static_cast<int>((value >> 11) & 0x1F),
            static_cast<int>((value >> 5) & 0x3F),
            static_cast<int>(value & 0x1F)};
}

std::uint16_t pack565Components(const std::array<int, 3>& c) {
    const int r = std::max(0, std::min(31, c[0]));
    const int g = std::max(0, std::min(63, c[1]));
    const int b = std::max(0, std::min(31, c[2]));
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}

void enforceColorOrder(std::uint16_t& c0, std::uint16_t& c1, bool alphaMode, bool fourColorOnly) {
    if (fourColorOnly) {
        if (c0 < c1) std::swap(c0, c1);
    } else if (alphaMode) {
        if (c0 > c1) std::swap(c0, c1);
    } else if (c0 < c1) {
        std::swap(c0, c1);
    }
}

DxtColorFit evaluateColorFit(const std::array<std::array<std::uint8_t, 4>, 16>& pixels,
                             std::uint16_t c0,
                             std::uint16_t c1,
                             bool alphaMode,
                             bool fourColorOnly,
                             const DxtBlockOptions& options) {
    enforceColorOrder(c0, c1, alphaMode, fourColorOnly);
    const auto palette = makeDxtColorPalette(c0, c1, fourColorOnly);
    const unsigned usable = alphaMode ? 3u : ((fourColorOnly || c0 > c1) ? 4u : 3u);
    DxtColorFit fit;
    fit.c0 = c0;
    fit.c1 = c1;
    fit.error = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const auto& p = pixels[i];
        unsigned best = 0;
        std::uint64_t bestDist = std::numeric_limits<std::uint64_t>::max();
        if (alphaMode && p[3] < options.alphaThreshold) {
            best = 3;
            bestDist = static_cast<std::uint64_t>(p[3]) * p[3];
        } else {
            for (unsigned j = 0; j < usable; ++j) {
                std::uint64_t dist = colorDistance(p, palette[j], options.metric);
                if (options.weightColorByAlpha) {
                    // Transparent texels often carry arbitrary RGB. Keep a tiny
                    // contribution so they are not completely unconstrained.
                    dist = (dist * static_cast<unsigned>(std::max<std::uint8_t>(1, p[3]))) / 255u;
                }
                if (alphaMode) {
                    const int da = static_cast<int>(p[3]) - 255;
                    dist += static_cast<std::uint64_t>(da * da);
                }
                if (dist < bestDist) {
                    bestDist = dist;
                    best = j;
                }
            }
        }
        fit.error += bestDist;
        fit.indices |= (best & 0x3u) << (2 * i);
    }
    return fit;
}

std::pair<std::uint16_t, std::uint16_t> refineColorEndpoints(const std::array<std::array<std::uint8_t, 4>, 16>& pixels,
                                                             std::uint16_t c0,
                                                             std::uint16_t c1,
                                                             bool alphaMode,
                                                             bool fourColorOnly,
                                                             const DxtBlockOptions& options,
                                                             int iterations) {
    DxtColorFit best = evaluateColorFit(pixels, c0, c1, alphaMode, fourColorOnly, options);
    std::uint16_t current0 = best.c0;
    std::uint16_t current1 = best.c1;
    for (int it = 0; it < iterations; ++it) {
        const DxtColorFit fit = evaluateColorFit(pixels, current0, current1, alphaMode, fourColorOnly, options);
        if (fit.error < best.error) best = fit;
        double ata00 = 0.0, ata01 = 0.0, ata11 = 0.0;
        std::array<double, 3> atb0{0.0, 0.0, 0.0}, atb1{0.0, 0.0, 0.0};
        unsigned used = 0;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const unsigned idx = (fit.indices >> (2 * i)) & 0x3u;
            if (alphaMode && idx == 3) continue;
            double w0 = 0.0, w1 = 0.0;
            if (idx == 0) { w0 = 1.0; }
            else if (idx == 1) { w1 = 1.0; }
            else if (idx == 2 && alphaMode) { w0 = 0.5; w1 = 0.5; }
            else if (idx == 2) { w0 = 2.0 / 3.0; w1 = 1.0 / 3.0; }
            else { w0 = 1.0 / 3.0; w1 = 2.0 / 3.0; }
            const double sampleWeight = options.weightColorByAlpha
                ? std::max(1.0 / 255.0, static_cast<double>(pixels[i][3]) / 255.0)
                : 1.0;
            ata00 += sampleWeight * w0 * w0;
            ata01 += sampleWeight * w0 * w1;
            ata11 += sampleWeight * w1 * w1;
            for (int c = 0; c < 3; ++c) {
                atb0[c] += sampleWeight * w0 * pixels[i][c];
                atb1[c] += sampleWeight * w1 * pixels[i][c];
            }
            ++used;
        }
        if (used == 0) break;
        const double det = ata00 * ata11 - ata01 * ata01;
        if (std::abs(det) < 1e-9) break;
        std::array<double, 3> e0{}, e1{};
        for (int c = 0; c < 3; ++c) {
            e0[c] = (atb0[c] * ata11 - atb1[c] * ata01) / det;
            e1[c] = (ata00 * atb1[c] - ata01 * atb0[c]) / det;
        }
        std::uint16_t next0 = pack565FromDouble(e0);
        std::uint16_t next1 = pack565FromDouble(e1);
        enforceColorOrder(next0, next1, alphaMode, fourColorOnly);
        if (next0 == current0 && next1 == current1) break;
        current0 = next0;
        current1 = next1;
    }
    const DxtColorFit finalFit = evaluateColorFit(pixels, current0, current1, alphaMode, fourColorOnly, options);
    if (finalFit.error < best.error) return {finalFit.c0, finalFit.c1};
    return {best.c0, best.c1};
}

DxtColorFit localSearchColorEndpoints(const std::array<std::array<std::uint8_t, 4>, 16>& pixels,
                                      std::uint16_t c0,
                                      std::uint16_t c1,
                                      bool alphaMode,
                                      bool fourColorOnly,
                                      const DxtBlockOptions& options) {
    DxtColorFit best = evaluateColorFit(pixels, c0, c1, alphaMode, fourColorOnly, options);
    auto c0c = unpack565Components(best.c0);
    auto c1c = unpack565Components(best.c1);
    const std::array<int, 3> maxComponent{31, 63, 31};

    auto tryCandidate = [&](const std::array<int, 3>& n0, const std::array<int, 3>& n1) -> bool {
        std::uint16_t next0 = pack565Components(n0);
        std::uint16_t next1 = pack565Components(n1);
        enforceColorOrder(next0, next1, alphaMode, fourColorOnly);
        const DxtColorFit candidate = evaluateColorFit(pixels, next0, next1, alphaMode, fourColorOnly, options);
        if (candidate.error < best.error) {
            best = candidate;
            c0c = unpack565Components(best.c0);
            c1c = unpack565Components(best.c1);
            return true;
        }
        return false;
    };

    const std::vector<int> steps = options.quality == DxtCompressionQuality::High
        ? std::vector<int>{4, 2, 1}
        : std::vector<int>{1};
    for (int step : steps) {
        bool improved = true;
        unsigned passes = 0;
        while (improved && passes++ < 24) {
            improved = false;
            for (int endpoint = 0; endpoint < 2; ++endpoint) {
                for (int channel = 0; channel < 3; ++channel) {
                    for (int sign : {-1, 1}) {
                        auto n0 = c0c;
                        auto n1 = c1c;
                        auto& target = endpoint == 0 ? n0 : n1;
                        target[channel] = std::max(0, std::min(maxComponent[channel], target[channel] + sign * step));
                        if (tryCandidate(n0, n1)) improved = true;
                    }
                }
            }
        }
    }
    return best;
}

std::array<std::array<std::uint8_t, 4>, 16> collectBlockPixels(const TextureLayer& layer, std::uint32_t xBase, std::uint32_t yBase) {
    std::array<std::array<std::uint8_t, 4>, 16> pixels{};
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) pixels[4 * y + x] = blockPixel(layer, xBase + x, yBase + y);
    }
    return pixels;
}

DxtColorFit fitColorBlock(const TextureLayer& layer,
                          std::uint32_t xBase,
                          std::uint32_t yBase,
                          bool alphaMode,
                          bool fourColorOnly,
                          const DxtBlockOptions& options) {
    const auto pixels = collectBlockPixels(layer, xBase, yBase);
    std::vector<std::array<double, 3>> pts;
    pts.reserve(16);
    for (const auto& p : pixels) {
        if (alphaMode && p[3] < options.alphaThreshold) continue;
        pts.push_back({static_cast<double>(p[0]), static_cast<double>(p[1]), static_cast<double>(p[2])});
    }
    if (pts.empty()) return evaluateColorFit(pixels, 0, 0, true, false, options);

    std::array<double, 3> mean{0, 0, 0}, mn{255, 255, 255}, mx{0, 0, 0};
    for (const auto& p : pts) {
        for (int c = 0; c < 3; ++c) {
            mean[c] += p[c];
            mn[c] = std::min(mn[c], p[c]);
            mx[c] = std::max(mx[c], p[c]);
        }
    }
    for (double& v : mean) v /= static_cast<double>(pts.size());

    std::array<double, 3> axis{mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
    double cov[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const auto& p : pts) {
        const std::array<double, 3> d{p[0] - mean[0], p[1] - mean[1], p[2] - mean[2]};
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) cov[r][c] += d[r] * d[c];
    }
    double norm = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (norm < 1e-9) axis = {1.0, 1.0, 1.0};
    else for (double& v : axis) v /= norm;
    const int powerIterations = options.quality == DxtCompressionQuality::Fast ? 0 : (options.quality == DxtCompressionQuality::Normal ? 4 : 8);
    for (int i = 0; i < powerIterations; ++i) {
        std::array<double, 3> next{
            cov[0][0] * axis[0] + cov[0][1] * axis[1] + cov[0][2] * axis[2],
            cov[1][0] * axis[0] + cov[1][1] * axis[1] + cov[1][2] * axis[2],
            cov[2][0] * axis[0] + cov[2][1] * axis[1] + cov[2][2] * axis[2]};
        norm = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (norm < 1e-9) break;
        axis = {next[0] / norm, next[1] / norm, next[2] / norm};
    }

    double minP = std::numeric_limits<double>::infinity();
    double maxP = -std::numeric_limits<double>::infinity();
    std::array<double, 3> axisMin = pts.front(), axisMax = pts.front();
    for (const auto& p : pts) {
        const double proj = p[0] * axis[0] + p[1] * axis[1] + p[2] * axis[2];
        if (proj < minP) { minP = proj; axisMin = p; }
        if (proj > maxP) { maxP = proj; axisMax = p; }
    }

    std::array<double, 3> farA = pts.front(), farB = pts.front();
    double farDist = -1.0;
    if (options.quality != DxtCompressionQuality::Fast) {
        for (const auto& a : pts) for (const auto& b : pts) {
            const double dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
            const double d = dr * dr + dg * dg + db * db;
            if (d > farDist) { farDist = d; farA = a; farB = b; }
        }
    }

    std::vector<std::pair<std::uint16_t, std::uint16_t>> candidates;
    auto add = [&](std::array<double, 3> a, std::array<double, 3> b) {
        std::uint16_t ca = pack565FromDouble(a), cb = pack565FromDouble(b);
        enforceColorOrder(ca, cb, alphaMode, fourColorOnly);
        if (std::find(candidates.begin(), candidates.end(), std::make_pair(ca, cb)) == candidates.end()) {
            candidates.emplace_back(ca, cb);
        }
    };
    add(axisMax, axisMin);
    add(mx, mn);
    if (options.quality != DxtCompressionQuality::Fast) add(farA, farB);
    if (options.quality == DxtCompressionQuality::High) {
        add(mean, mean);
        add(axisMax, mean);
        add(mean, axisMin);
    }

    DxtColorFit best;
    const int refineIterations = options.quality == DxtCompressionQuality::Fast ? 0 : (options.quality == DxtCompressionQuality::Normal ? 3 : 8);
    for (auto [c0, c1] : candidates) {
        const auto refined = refineColorEndpoints(pixels, c0, c1, alphaMode, fourColorOnly, options, refineIterations);
        DxtColorFit fit = evaluateColorFit(pixels, refined.first, refined.second, alphaMode, fourColorOnly, options);
        if (options.quality == DxtCompressionQuality::High) {
            fit = localSearchColorEndpoints(pixels, fit.c0, fit.c1, alphaMode, fourColorOnly, options);
        }
        if (fit.error < best.error) best = fit;
    }
    return best;
}

void encodeDxtColorBlock(const TextureLayer& layer,
                         std::uint32_t xBase,
                         std::uint32_t yBase,
                         std::vector<std::uint8_t>& out,
                         bool allowDxt1Alpha,
                         bool fourColorOnly,
                         const DxtBlockOptions& options) {
    bool alphaMode = false;
    if (allowDxt1Alpha) {
        for (std::uint32_t y = 0; y < 4 && !alphaMode; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                if (blockPixel(layer, xBase + x, yBase + y)[3] < options.alphaThreshold) { alphaMode = true; break; }
            }
        }
    }
    const auto fit = fitColorBlock(layer, xBase, yBase, alphaMode, fourColorOnly, options);
    appendLE16(out, fit.c0);
    appendLE16(out, fit.c1);
    appendLE32(out, fit.indices);
}

std::vector<std::uint8_t> encodeDxt1(const TextureLayer& layer, const TextureSaveOptions& saveOptions) {
    DxtBlockOptions options;
    options.quality = saveOptions.dxtQuality;
    options.metric = saveOptions.dxtMetric;
    options.weightColorByAlpha = saveOptions.weightColorByAlpha;
    options.alphaThreshold = saveOptions.dxt1AlphaThreshold;
    std::vector<std::uint8_t> out;
    out.reserve(dxt1Size(layer.width, layer.height));
    for (std::uint32_t y = 0; y < layer.height; y += 4) {
        for (std::uint32_t x = 0; x < layer.width; x += 4) {
            encodeDxtColorBlock(layer, x, y, out, true, false, options);
        }
    }
    return out;
}

std::vector<std::uint8_t> encodeDxt3(const TextureLayer& layer, const TextureSaveOptions& saveOptions) {
    DxtBlockOptions options;
    options.quality = saveOptions.dxtQuality;
    options.metric = saveOptions.dxtMetric;
    options.weightColorByAlpha = saveOptions.weightColorByAlpha;
    options.alphaThreshold = saveOptions.dxt1AlphaThreshold;
    std::vector<std::uint8_t> out;
    out.reserve(dxt5Size(layer.width, layer.height));
    for (std::uint32_t yBase = 0; yBase < layer.height; yBase += 4) {
        for (std::uint32_t xBase = 0; xBase < layer.width; xBase += 4) {
            for (std::uint32_t y = 0; y < 4; ++y) {
                std::uint16_t row = 0;
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const auto alpha = blockPixel(layer, xBase + x, yBase + y)[3];
                    const auto alpha4 = static_cast<std::uint16_t>((static_cast<unsigned>(alpha) + 8u) / 17u);
                    row |= static_cast<std::uint16_t>(alpha4 << (x * 4));
                }
                appendLE16(out, row);
            }
            encodeDxtColorBlock(layer, xBase, yBase, out, false, true, options);
        }
    }
    return out;
}

std::array<std::uint8_t, 8> makeAlphaPalette(std::uint8_t a0, std::uint8_t a1) {
    std::array<std::uint8_t, 8> a{};
    a[0] = a0;
    a[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i) a[i] = static_cast<std::uint8_t>(((8 - i) * a0 + (i - 1) * a1 + 3) / 7);
    } else {
        for (int i = 2; i < 6; ++i) a[i] = static_cast<std::uint8_t>(((6 - i) * a0 + (i - 1) * a1 + 2) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    return a;
}

struct AlphaFit { std::uint8_t a0 = 255, a1 = 0; std::uint64_t bits = 0; std::uint64_t error = std::numeric_limits<std::uint64_t>::max(); };

AlphaFit evaluateAlphaFit(const std::array<std::uint8_t, 16>& values, std::uint8_t a0, std::uint8_t a1) {
    const auto palette = makeAlphaPalette(a0, a1);
    AlphaFit fit;
    fit.a0 = a0;
    fit.a1 = a1;
    fit.error = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        unsigned best = 0, bestD = std::numeric_limits<unsigned>::max();
        for (unsigned j = 0; j < 8; ++j) {
            const int d = static_cast<int>(values[i]) - static_cast<int>(palette[j]);
            const unsigned e = static_cast<unsigned>(d * d);
            if (e < bestD) { bestD = e; best = j; }
        }
        fit.error += bestD;
        fit.bits |= static_cast<std::uint64_t>(best & 7u) << (3 * i);
    }
    return fit;
}

bool alphaSelectorWeights(unsigned selector, bool eightAlphaMode, double& w0, double& w1) {
    w0 = 0.0;
    w1 = 0.0;
    if (selector == 0) { w0 = 1.0; return true; }
    if (selector == 1) { w1 = 1.0; return true; }
    if (eightAlphaMode) {
        if (selector < 2 || selector > 7) return false;
        w0 = static_cast<double>(8 - static_cast<int>(selector)) / 7.0;
        w1 = static_cast<double>(static_cast<int>(selector) - 1) / 7.0;
        return true;
    }
    if (selector >= 2 && selector <= 5) {
        w0 = static_cast<double>(6 - static_cast<int>(selector)) / 5.0;
        w1 = static_cast<double>(static_cast<int>(selector) - 1) / 5.0;
        return true;
    }
    // Selectors 6 and 7 are fixed 0/255 in six-alpha mode; they do not
    // contribute to the least-squares endpoint solve.
    return false;
}

std::pair<std::uint8_t, std::uint8_t> solveAlphaEndpoints(const std::array<std::uint8_t, 16>& values,
                                                          std::uint64_t selectorBits,
                                                          bool eightAlphaMode,
                                                          std::uint8_t fallback0,
                                                          std::uint8_t fallback1) {
    double ata00 = 0.0, ata01 = 0.0, ata11 = 0.0, atb0 = 0.0, atb1 = 0.0;
    unsigned used = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const unsigned selector = static_cast<unsigned>((selectorBits >> (3 * i)) & 0x7u);
        double w0 = 0.0, w1 = 0.0;
        if (!alphaSelectorWeights(selector, eightAlphaMode, w0, w1)) continue;
        ata00 += w0 * w0;
        ata01 += w0 * w1;
        ata11 += w1 * w1;
        atb0 += w0 * static_cast<double>(values[i]);
        atb1 += w1 * static_cast<double>(values[i]);
        ++used;
    }
    if (used == 0) return {fallback0, fallback1};
    const double det = ata00 * ata11 - ata01 * ata01;
    if (std::abs(det) < 1e-9) return {fallback0, fallback1};

    std::uint8_t a0 = clampByte((atb0 * ata11 - atb1 * ata01) / det);
    std::uint8_t a1 = clampByte((ata00 * atb1 - ata01 * atb0) / det);
    if (eightAlphaMode) {
        if (a0 <= a1) {
            if (a0 == a1) {
                if (a0 < 255) ++a0;
                else if (a1 > 0) --a1;
            } else {
                std::swap(a0, a1);
            }
        }
    } else if (a0 > a1) {
        std::swap(a0, a1);
    }
    return {a0, a1};
}

AlphaFit refineAlphaFit(const std::array<std::uint8_t, 16>& values,
                        std::uint8_t a0,
                        std::uint8_t a1,
                        DxtCompressionQuality quality) {
    AlphaFit best = evaluateAlphaFit(values, a0, a1);
    AlphaFit current = best;
    const bool eightAlphaMode = a0 > a1;
    const int iterations = quality == DxtCompressionQuality::High ? 8 : 4;
    for (int it = 0; it < iterations; ++it) {
        const auto solved = solveAlphaEndpoints(values, current.bits, eightAlphaMode, current.a0, current.a1);
        if (solved.first == current.a0 && solved.second == current.a1) break;
        const AlphaFit next = evaluateAlphaFit(values, solved.first, solved.second);
        if (next.error < best.error) best = next;
        if (next.error >= current.error) break;
        current = next;
    }

    if (quality == DxtCompressionQuality::High) {
        for (int step : {8, 4, 2, 1}) {
            bool improved = true;
            unsigned passes = 0;
            while (improved && passes++ < 24) {
                improved = false;
                for (int endpoint = 0; endpoint < 2; ++endpoint) {
                    for (int sign : {-1, 1}) {
                        int next0 = static_cast<int>(best.a0);
                        int next1 = static_cast<int>(best.a1);
                        if (endpoint == 0) next0 += sign * step;
                        else next1 += sign * step;
                        next0 = std::max(0, std::min(255, next0));
                        next1 = std::max(0, std::min(255, next1));
                        const AlphaFit candidate = evaluateAlphaFit(values,
                                                                    static_cast<std::uint8_t>(next0),
                                                                    static_cast<std::uint8_t>(next1));
                        if (candidate.error < best.error) {
                            best = candidate;
                            improved = true;
                        }
                    }
                }
            }
        }
    }
    return best;
}

AlphaFit fitAlphaBlock(const TextureLayer& layer, std::uint32_t xBase, std::uint32_t yBase, DxtCompressionQuality quality) {
    std::array<std::uint8_t, 16> values{};
    std::uint8_t mn = 255, mx = 0, mnInner = 255, mxInner = 0;
    bool hasInner = false;
    for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < 4; ++x) {
        const std::uint8_t alpha = blockPixel(layer, xBase + x, yBase + y)[3];
        values[4 * y + x] = alpha;
        mn = std::min(mn, alpha);
        mx = std::max(mx, alpha);
        if (alpha != 0 && alpha != 255) { mnInner = std::min(mnInner, alpha); mxInner = std::max(mxInner, alpha); hasInner = true; }
    }

    std::vector<std::pair<std::uint8_t, std::uint8_t>> candidates{{mx, mn}, {255, 0}, {mn, mx}};
    if (hasInner && quality != DxtCompressionQuality::Fast) {
        // Six-alpha mode reserves exact 0 and 255 palette entries, which is
        // useful for textures mixing cutout pixels with soft transparency.
        candidates.emplace_back(mnInner, mxInner);
        candidates.emplace_back(mxInner, mnInner);
        candidates.emplace_back(mxInner, mn);
        candidates.emplace_back(mx, mnInner);
    }

    AlphaFit best;
    for (auto [a0, a1] : candidates) {
        AlphaFit fit = quality == DxtCompressionQuality::Fast
            ? evaluateAlphaFit(values, a0, a1)
            : refineAlphaFit(values, a0, a1, quality);
        if (fit.error < best.error) best = fit;
    }
    return best;
}

std::vector<std::uint8_t> encodeDxt5(const TextureLayer& layer, const TextureSaveOptions& saveOptions) {
    DxtBlockOptions options;
    options.quality = saveOptions.dxtQuality;
    options.metric = saveOptions.dxtMetric;
    options.weightColorByAlpha = saveOptions.weightColorByAlpha;
    options.alphaThreshold = saveOptions.dxt1AlphaThreshold;
    std::vector<std::uint8_t> out;
    out.reserve(dxt5Size(layer.width, layer.height));
    for (std::uint32_t yBase = 0; yBase < layer.height; yBase += 4) {
        for (std::uint32_t xBase = 0; xBase < layer.width; xBase += 4) {
            const AlphaFit alpha = fitAlphaBlock(layer, xBase, yBase, saveOptions.dxtQuality);
            out.push_back(alpha.a0);
            out.push_back(alpha.a1);
            for (int i = 0; i < 6; ++i) out.push_back(static_cast<std::uint8_t>((alpha.bits >> (8 * i)) & 0xFF));
            encodeDxtColorBlock(layer, xBase, yBase, out, false, true, options);
        }
    }
    return out;
}

std::vector<std::uint8_t> encodeLayerRaw(const TextureLayer& layer,
                                         TextureCompression compression,
                                         bool hasAlpha,
                                         const TextureSaveOptions& options) {
    std::vector<std::uint8_t> out;
    if (compression == TextureCompression::Dxt1) return encodeDxt1(layer, options);
    if (compression == TextureCompression::Dxt3) return encodeDxt3(layer, options);
    if (compression == TextureCompression::Dxt5) return encodeDxt5(layer, options);
    if (compression == TextureCompression::Gray) {
        out.reserve(static_cast<std::size_t>(layer.width) * layer.height);
        for (std::size_t i = 0; i + 3 < layer.rgba.size(); i += 4) {
            out.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(layer.rgba[i]) + layer.rgba[i + 1] + layer.rgba[i + 2]) / 3));
        }
        return out;
    }
    if (compression == TextureCompression::SwizzledBgra) {
        const std::size_t pixels = static_cast<std::size_t>(layer.width) * layer.height;
        out.assign(pixels * 4, 0);
        const bool swizzle = isPowerOfTwo(layer.width) && isPowerOfTwo(layer.height);
        for (std::uint32_t y = 0; y < layer.height; ++y) {
            for (std::uint32_t x = 0; x < layer.width; ++x) {
                const std::size_t source = (static_cast<std::size_t>(y) * layer.width + x) * 4;
                const std::size_t pixelOffset = swizzle
                    ? static_cast<std::size_t>(swizzledPixelOffset(x, y, layer.width, layer.height))
                    : static_cast<std::size_t>(y) * layer.width + x;
                if (pixelOffset >= pixels) throw TextureError("TPC swizzle produced an invalid pixel offset");
                const std::size_t destination = pixelOffset * 4;
                out[destination + 0] = layer.rgba[source + 2];
                out[destination + 1] = layer.rgba[source + 1];
                out[destination + 2] = layer.rgba[source + 0];
                out[destination + 3] = layer.rgba[source + 3];
            }
        }
        return out;
    }
    out.reserve(static_cast<std::size_t>(layer.width) * layer.height * (hasAlpha ? 4 : 3));
    for (std::size_t i = 0; i + 3 < layer.rgba.size(); i += 4) {
        out.push_back(layer.rgba[i + 0]);
        out.push_back(layer.rgba[i + 1]);
        out.push_back(layer.rgba[i + 2]);
        if (hasAlpha) out.push_back(layer.rgba[i + 3]);
    }
    return out;
}

TextureLayer decodeRawLayer(const std::uint8_t* data, std::size_t size, std::uint32_t width, std::uint32_t height,
                            std::uint8_t encoding, bool tpcFileOrientation) {
    TextureLayer layer = makeLayer(width, height);
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (encoding == kTpcEncodingGray) {
        if (size < pixels) throw TextureError("TPC grayscale payload is truncated");
        for (std::size_t i = 0; i < pixels; ++i) {
            layer.rgba[i * 4 + 0] = data[i];
            layer.rgba[i * 4 + 1] = data[i];
            layer.rgba[i * 4 + 2] = data[i];
            layer.rgba[i * 4 + 3] = 255;
        }
    } else if (encoding == kTpcEncodingRgb) {
        if (size < pixels * 3) throw TextureError("TPC RGB payload is truncated");
        for (std::size_t i = 0; i < pixels; ++i) {
            layer.rgba[i * 4 + 0] = data[i * 3 + 0];
            layer.rgba[i * 4 + 1] = data[i * 3 + 1];
            layer.rgba[i * 4 + 2] = data[i * 3 + 2];
            layer.rgba[i * 4 + 3] = 255;
        }
    } else if (encoding == kTpcEncodingRgba) {
        if (size < pixels * 4) throw TextureError("TPC RGBA payload is truncated");
        std::copy_n(data, pixels * 4, layer.rgba.begin());
    } else if (encoding == kTpcEncodingSwizzledBgra) {
        if (size < pixels * 4) throw TextureError("TPC BGRA payload is truncated");
        const bool swizzled = isPowerOfTwo(width) && isPowerOfTwo(height);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t destinationPixel = static_cast<std::size_t>(y) * width + x;
                const std::size_t sourcePixel = swizzled
                    ? static_cast<std::size_t>(swizzledPixelOffset(x, y, width, height))
                    : destinationPixel;
                if (sourcePixel >= pixels) throw TextureError("TPC swizzle references a pixel outside the payload");
                layer.rgba[destinationPixel * 4 + 0] = data[sourcePixel * 4 + 2];
                layer.rgba[destinationPixel * 4 + 1] = data[sourcePixel * 4 + 1];
                layer.rgba[destinationPixel * 4 + 2] = data[sourcePixel * 4 + 0];
                layer.rgba[destinationPixel * 4 + 3] = data[sourcePixel * 4 + 3];
            }
        }
    } else {
        throw TextureError("Unknown TPC raw encoding: " + std::to_string(encoding));
    }
    if (tpcFileOrientation) {
        layer = flipLayerVerticalCopy(std::move(layer));
    }
    return layer;
}

std::string readTextSidecar(const std::filesystem::path& texturePath) {
    auto sidecar = texturePath;
    sidecar.replace_extension(".txi");
    if (std::filesystem::exists(sidecar)) {
        const auto bytes = readFileBytes(sidecar);
        return sanitizeTxiPayload(std::string(bytes.begin(), bytes.end()));
    }
    return {};
}

bool textureKindUsesTxiSidecar(const std::string& extension) {
    return extension == "tga" || extension == "dds" || extension == "png" ||
           extension == "jpg" || extension == "jpeg" || extension == "jpe" ||
           extension == "bmp";
}

std::filesystem::path createTextureTransactionDirectory(const std::filesystem::path& parent) {
    static std::atomic<std::uint64_t> sequence{0};
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = parent / (".neotpc-texture-txn-" + std::to_string(now) + "-" + std::to_string(id));
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec)) return candidate;
        if (ec && ec != std::errc::file_exists) {
            throw TextureError("Unable to create texture transaction directory: " + ec.message());
        }
    }
    throw TextureError("Unable to allocate a unique texture transaction directory");
}

void removePathNoThrow(const std::filesystem::path& path) noexcept {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

template <typename ImageWriter>
void commitTextureAndSidecar(const TextureData& texture,
                             const std::filesystem::path& output,
                             bool writeSidecar,
                             ImageWriter&& imageWriter) {
    if (output.empty()) throw TextureError("Texture output path is empty");
    const auto parent = output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) throw TextureError("Unable to create texture output directory: " + ec.message());

    auto sidecar = output;
    if (writeSidecar) sidecar.replace_extension(".txi");
    const auto transaction = createTextureTransactionDirectory(parent);
    auto stagedImage = transaction / "staged-image";
    stagedImage.replace_extension(output.extension());
    const auto stagedSidecar = transaction / "staged-sidecar.txi";
    const auto imageBackup = transaction / "backup-image";
    const auto sidecarBackup = transaction / "backup-sidecar";

    struct Replacement {
        std::filesystem::path target;
        std::filesystem::path staged;
        std::filesystem::path backup;
        bool hadOriginal = false;
        bool installed = false;
    };
    std::vector<Replacement> replacements;
    replacements.push_back({output, stagedImage, imageBackup});
    if (writeSidecar) {
        replacements.push_back({sidecar, texture.txi.empty() ? std::filesystem::path{} : stagedSidecar, sidecarBackup});
    }

    try {
        imageWriter(stagedImage);
        if (writeSidecar && !texture.txi.empty()) {
            writeFileBytes(stagedSidecar,
                           std::vector<std::uint8_t>(texture.txi.begin(), texture.txi.end()));
        }

        // Move all existing outputs to rollback slots before installing any
        // replacement.  An empty staged sidecar means transactional deletion.
        for (auto& replacement : replacements) {
            ec.clear();
            if (!std::filesystem::exists(replacement.target, ec)) {
                if (ec) throw TextureError("Unable to inspect texture output target: " + ec.message());
                continue;
            }
            if (!std::filesystem::is_regular_file(replacement.target, ec) || ec) {
                throw TextureError("Texture output target is not a replaceable regular file: " + pathToUtf8(replacement.target));
            }
            std::filesystem::rename(replacement.target, replacement.backup, ec);
            if (ec) throw TextureError("Unable to back up texture output before replacement: " + ec.message());
            replacement.hadOriginal = true;
        }

        for (auto& replacement : replacements) {
            if (replacement.staged.empty()) continue;
            ec.clear();
            std::filesystem::rename(replacement.staged, replacement.target, ec);
            if (ec) throw TextureError("Unable to install staged texture output: " + ec.message());
            replacement.installed = true;
        }

        removePathNoThrow(transaction);
    } catch (...) {
        const auto originalError = std::current_exception();
        bool restorationFailed = false;
        for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
            if (it->installed) removePathNoThrow(it->target);
            if (it->hadOriginal) {
                ec.clear();
                std::filesystem::rename(it->backup, it->target, ec);
                if (ec) restorationFailed = true;
                else it->hadOriginal = false;
            }
        }
        if (!restorationFailed) removePathNoThrow(transaction);
        if (restorationFailed) {
            throw TextureError("Texture save failed and one or more original files could not be restored; rollback data remains in " +
                               pathToUtf8(transaction));
        }
        std::rethrow_exception(originalError);
    }
}

void validateLayer(const TextureLayer& layer) {
    validateLayerStorage(layer, "Texture layer");
}

std::string normalizeTxiFooter(std::string txi) {
    txi = trim(txi);
    if (txi.empty()) return {};
    std::string out;
    for (char ch : txi) {
        if (ch == '\r') continue;
        if (ch == '\n') out += "\r\n";
        else out.push_back(ch);
    }
    if (out.size() < 2 || out.substr(out.size() - 2) != "\r\n") out += "\r\n";
    return out;
}

std::uint32_t mipCountForLayer(const TextureLayer& layer, bool generate) {
    if (!generate) return 1;
    if (!layer.mipmaps.empty()) return static_cast<std::uint32_t>(1 + layer.mipmaps.size());
    std::uint32_t count = 1;
    std::uint32_t w = layer.width;
    std::uint32_t h = layer.height;
    while (w > 1 || h > 1) {
        w = std::max<std::uint32_t>(1, w / 2);
        h = std::max<std::uint32_t>(1, h / 2);
        ++count;
    }
    return count;
}

std::vector<std::vector<TextureLayer>> makeMipChainPerLayer(const std::vector<TextureLayer>& layers,
                                                            bool generate,
                                                            bool bicubic,
                                                            bool tpcFileOrientation) {
    std::vector<std::vector<TextureLayer>> chains;
    chains.reserve(layers.size());
    for (TextureLayer layer : layers) {
        validateLayer(layer);
        std::vector<TextureLayer> chain;
        if (!generate) {
            layer.mipmaps.clear();
            chain.push_back(std::move(layer));
        } else if (!layer.mipmaps.empty()) {
            auto stored = std::move(layer.mipmaps);
            layer.mipmaps.clear();
            chain.push_back(std::move(layer));
            for (auto& mip : stored) {
                mip.mipmaps.clear();
                chain.push_back(std::move(mip));
            }
        } else {
            chain = generateMipmaps(std::move(layer), bicubic, false);
        }
        if (tpcFileOrientation) {
            for (auto& mip : chain) mip = flipLayerVerticalCopy(std::move(mip));
        }
        chains.push_back(std::move(chain));
    }
    return chains;
}

} // namespace

std::string textureCompressionToString(TextureCompression compression) {
    switch (compression) {
    case TextureCompression::Auto: return "auto";
    case TextureCompression::None: return "none";
    case TextureCompression::Gray: return "grey";
    case TextureCompression::Dxt1: return "dxt1";
    case TextureCompression::Dxt3: return "dxt3";
    case TextureCompression::Dxt5: return "dxt5";
    case TextureCompression::SwizzledBgra: return "swizzled-bgra";
    }
    return "auto";
}

TextureCompression textureCompressionFromString(const std::string& value) {
    const std::string lower = asciiLower(trim(value));
    if (lower == "auto") return TextureCompression::Auto;
    if (lower == "none" || lower == "raw" || lower == "rgba" || lower == "rgb") return TextureCompression::None;
    if (lower == "grey" || lower == "gray" || lower == "r8") return TextureCompression::Gray;
    if (lower == "dxt1") return TextureCompression::Dxt1;
    if (lower == "dxt3") return TextureCompression::Dxt3;
    if (lower == "dxt5") return TextureCompression::Dxt5;
    if (lower == "swizzled-bgra" || lower == "swizzled" || lower == "xbox-bgra" || lower == "bgra0c") {
        return TextureCompression::SwizzledBgra;
    }
    throw TextureError("Unknown texture compression: " + value);
}


std::string dxtCompressionQualityToString(DxtCompressionQuality quality) {
    switch (quality) {
    case DxtCompressionQuality::Fast: return "fast";
    case DxtCompressionQuality::Normal: return "normal";
    case DxtCompressionQuality::High: return "high";
    }
    return "high";
}

DxtCompressionQuality dxtCompressionQualityFromString(const std::string& value) {
    const std::string lower = asciiLower(trim(value));
    if (lower == "fast" || lower == "range" || lower == "rangefit") return DxtCompressionQuality::Fast;
    if (lower == "normal" || lower == "cluster" || lower == "clusterfit") return DxtCompressionQuality::Normal;
    if (lower == "high" || lower == "best" || lower == "iterative" || lower == "iterativeclusterfit") return DxtCompressionQuality::High;
    throw TextureError("Unknown DXT quality: " + value);
}

std::string dxtErrorMetricToString(DxtErrorMetric metric) {
    switch (metric) {
    case DxtErrorMetric::Perceptual: return "perceptual";
    case DxtErrorMetric::Uniform: return "uniform";
    }
    return "perceptual";
}

DxtErrorMetric dxtErrorMetricFromString(const std::string& value) {
    const std::string lower = asciiLower(trim(value));
    if (lower == "perceptual" || lower == "weighted") return DxtErrorMetric::Perceptual;
    if (lower == "uniform" || lower == "linear") return DxtErrorMetric::Uniform;
    throw TextureError("Unknown DXT error metric: " + value);
}

std::string textureFileKindToString(TextureFileKind kind) {
    switch (kind) {
    case TextureFileKind::Tga: return "TGA";
    case TextureFileKind::Tpc: return "TPC";
    case TextureFileKind::Txb: return "TXB";
    case TextureFileKind::Dds: return "DDS";
    case TextureFileKind::Png: return "PNG";
    case TextureFileKind::Jpeg: return "JPEG";
    case TextureFileKind::Bmp: return "BMP";
    case TextureFileKind::Txi: return "TXI";
    case TextureFileKind::Unknown: break;
    }
    return "unknown";
}

std::string cubeFaceToString(CubeFace face) {
    switch (face) {
    case CubeFace::PositiveX: return "+X";
    case CubeFace::NegativeX: return "-X";
    case CubeFace::PositiveY: return "+Y";
    case CubeFace::NegativeY: return "-Y";
    case CubeFace::PositiveZ: return "+Z";
    case CubeFace::NegativeZ: return "-Z";
    }
    return "unknown";
}

TxiFeatures parseTxiFeatures(const std::string& txi) {
    TxiFeatures features;
    for (const auto& line : splitLines(txi)) {
        std::string rest;
        const std::string key = firstTokenLower(line, &rest);
        if (key.empty()) continue;
        if (key == "cube") {
            if (auto v = parseU32Loose(rest)) features.cube = *v != 0;
            else features.cube = parseTruthy(rest);
        } else if (key == "proceduretype") {
            features.procedureType = asciiLower(trim(rest));
        } else if (key == "numx") {
            if (auto v = parseU32Loose(rest)) features.numX = *v;
        } else if (key == "numy") {
            if (auto v = parseU32Loose(rest)) features.numY = *v;
        } else if (key == "defaultwidth") {
            if (auto v = parseU32Loose(rest)) features.defaultWidth = *v;
        } else if (key == "defaultheight") {
            if (auto v = parseU32Loose(rest)) features.defaultHeight = *v;
        } else if (key == "fps") {
            if (auto v = parseDoubleLoose(rest)) features.fps = *v;
        } else if (key == "isbumpmap") {
            if (auto v = parseU32Loose(rest)) features.isBumpMap = *v != 0;
            else features.isBumpMap = parseTruthy(rest);
        } else if (key == "compresstexture") {
            features.compressTextureSpecified = true;
            if (auto v = parseU32Loose(rest)) features.compressTexture = *v != 0;
            else features.compressTexture = !parseFalsy(rest);
        }
    }
    return features;
}

std::optional<std::string> getTxiValue(const std::string& txi, const std::string& key) {
    const std::string wanted = asciiLower(key);
    for (const auto& line : splitLines(txi)) {
        std::string rest;
        const std::string current = firstTokenLower(line, &rest);
        if (current == wanted) return rest;
    }
    return std::nullopt;
}

std::string setTxiValue(const std::string& txi, const std::string& key, const std::string& value) {
    const std::string trimmedKey = trim(key);
    if (trimmedKey.empty()) throw TextureError("TXI key cannot be empty");
    const auto directive = findTxiDirective(trimmedKey);
    if (!directive) {
        throw TextureError("Unknown TXI key: " + trimmedKey + ". Edit the TXI source view directly to preserve custom/untraced directives.");
    }
    if (directive->valueKind == TxiDirectiveValueKind::ValueToken) {
        throw TextureError(directive->name + " is a TXI value token, not a standalone key");
    }
    if (directive->valueKind == TxiDirectiveValueKind::CoordinateBlockCount) {
        throw TextureError(directive->name + " begins a counted coordinate block; edit the TXI source view so its record rows stay together");
    }
    const std::string normalizedValue = trim(value);
    const std::string oneLine = directive->name + " " + normalizedValue + "\n";
    for (const auto& issue : validateTxiText(oneLine)) {
        if (issue.severity == TxiIssueSeverity::Error) {
            throw TextureError(issue.message);
        }
    }

    const std::string wanted = asciiLower(directive->name);
    std::ostringstream out;
    bool replaced = false;
    for (const auto& line : splitLines(txi)) {
        std::string rest;
        const std::string current = firstTokenLower(line, &rest);
        if (!replaced && current == wanted) {
            out << directive->name << ' ' << normalizedValue << '\n';
            replaced = true;
        } else {
            out << line << '\n';
        }
    }
    if (!replaced) {
        out << directive->name << ' ' << normalizedValue << '\n';
    }
    return out.str();
}

static bool looksLikeTgaHeader(const std::vector<std::uint8_t>& header) {
    if (header.size() < 18) return false;
    const std::uint8_t imageType = header[2];
    const std::uint16_t width = static_cast<std::uint16_t>(header[12] | (header[13] << 8));
    const std::uint16_t height = static_cast<std::uint16_t>(header[14] | (header[15] << 8));
    const std::uint8_t bpp = header[16];
    const bool typeOk = imageType == 1 || imageType == 2 || imageType == 3 || imageType == 9 || imageType == 10 || imageType == 11;
    const bool bppOk = bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32;
    return typeOk && bppOk && width > 0 && height > 0;
}

static bool looksLikeTpcHeader(const std::vector<std::uint8_t>& header) {
    if (header.size() < 14) return false;
    const std::uint16_t width = static_cast<std::uint16_t>(header[8] | (header[9] << 8));
    const std::uint16_t height = static_cast<std::uint16_t>(header[10] | (header[11] << 8));
    const std::uint8_t encoding = header[12];
    const std::uint8_t mipmaps = header[13];
    return width > 0 && height > 0 && width < 0x8000 && height < 0x8000 &&
           (encoding == kTpcEncodingGray || encoding == kTpcEncodingRgb || encoding == kTpcEncodingRgba || encoding == kTpcEncodingSwizzledBgra) &&
           mipmaps < 32;
}

static bool looksLikeTxbHeader(const std::vector<std::uint8_t>& header) {
    if (header.size() < 14) return false;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(header[0]) |
                                   (static_cast<std::uint32_t>(header[1]) << 8) |
                                   (static_cast<std::uint32_t>(header[2]) << 16) |
                                   (static_cast<std::uint32_t>(header[3]) << 24);
    const std::uint32_t width = static_cast<std::uint32_t>(header[8] | (header[9] << 8));
    const std::uint32_t height = static_cast<std::uint32_t>(header[10] | (header[11] << 8));
    const std::uint8_t encoding = header[12];
    const std::uint8_t mipmaps = header[13] == 0 ? 1 : header[13];
    if (dataSize == 0 || width == 0 || height == 0 || width >= 0x8000 || height >= 0x8000 || mipmaps >= 32 ||
        (encoding != kTxbEncodingBgra && encoding != kTxbEncodingGray &&
         encoding != kTxbEncodingDxt1 && encoding != kTxbEncodingDxt5)) {
        return false;
    }
    std::uint64_t expected = 0;
    std::uint32_t mipWidth = width;
    std::uint32_t mipHeight = height;
    for (std::uint8_t mip = 0; mip < mipmaps; ++mip) {
        std::uint64_t size = 0;
        if (encoding == kTxbEncodingDxt1) size = dxt1Size(mipWidth, mipHeight);
        else if (encoding == kTxbEncodingDxt5) size = dxt5Size(mipWidth, mipHeight);
        else size = static_cast<std::uint64_t>(mipWidth) * mipHeight * (encoding == kTxbEncodingGray ? 1u : 4u);
        if (!parser::checkedAdd(expected, size, expected)) return false;
        mipWidth = std::max<std::uint32_t>(1, mipWidth / 2);
        mipHeight = std::max<std::uint32_t>(1, mipHeight / 2);
    }
    return expected == dataSize;
}

static bool looksLikeDdsHeader(const std::vector<std::uint8_t>& header) {
    if (header.size() >= 4 && header[0] == 'D' && header[1] == 'D' && header[2] == 'S' && header[3] == ' ') return true;
    if (header.size() >= 20) {
        const std::uint32_t width = static_cast<std::uint32_t>(header[0]) |
                                    (static_cast<std::uint32_t>(header[1]) << 8) |
                                    (static_cast<std::uint32_t>(header[2]) << 16) |
                                    (static_cast<std::uint32_t>(header[3]) << 24);
        const std::uint32_t height = static_cast<std::uint32_t>(header[4]) |
                                     (static_cast<std::uint32_t>(header[5]) << 8) |
                                     (static_cast<std::uint32_t>(header[6]) << 16) |
                                     (static_cast<std::uint32_t>(header[7]) << 24);
        const std::uint32_t bpp = static_cast<std::uint32_t>(header[8]) |
                                  (static_cast<std::uint32_t>(header[9]) << 8) |
                                  (static_cast<std::uint32_t>(header[10]) << 16) |
                                  (static_cast<std::uint32_t>(header[11]) << 24);
        return width > 0 && height > 0 && width < 0x8000 && height < 0x8000 && (bpp == 3 || bpp == 4);
    }
    return false;
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

std::uint8_t maskedChannelToByte(std::uint32_t pixel, std::uint32_t mask) {
    if (mask == 0) return 0;
    unsigned shift = 0;
    while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
    const std::uint32_t shiftedMask = mask >> shift;
    const std::uint32_t value = (pixel & mask) >> shift;
    return static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) * 255u + shiftedMask / 2u) /
                                     shiftedMask);
}

TextureData textureFromDecodedLayer(TextureFileKind kind,
                                    const std::filesystem::path& path,
                                    TextureLayer layer,
                                    std::string sourceEncoding,
                                    std::optional<std::string> txiOverride = std::nullopt) {
    TextureData texture;
    texture.kind = kind;
    texture.sourcePath = path;
    texture.canvasWidth = layer.width;
    texture.canvasHeight = layer.height;
    texture.layers.push_back(std::move(layer));
    texture.txi = txiOverride ? sanitizeTxiPayload(std::move(*txiOverride)) : readTextSidecar(path);
    texture.sourceEncoding = std::move(sourceEncoding);
    refreshHasAlpha(texture);
    applyTxiLayout(texture);
    return texture;
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

    TextureLayer layer = makeLayer(width, height);
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

    return textureFromDecodedLayer(TextureFileKind::Bmp, path, std::move(layer),
                                   std::to_string(bpp) + "-bit BMP", std::move(txi));
}

TextureData readPngTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& path,
                                        std::string txi) {
    if (!looksLikePngHeader(bytes)) throw TextureError("Invalid PNG header");
    auto image = internal_image::decodePng(bytes);
    TextureLayer layer = makeLayer(image.width, image.height);
    layer.rgba = std::move(image.rgba);
    return textureFromDecodedLayer(TextureFileKind::Png, path, std::move(layer),
                                   image.encoding.empty() ? std::string("PNG via libspng") : image.encoding,
                                   std::move(txi));
}

TextureData readJpegTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                         const std::filesystem::path& path,
                                         std::string txi) {
    if (!looksLikeJpegHeader(bytes)) throw TextureError("Invalid JPEG header");
    auto image = internal_image::decodeJpeg(bytes);
    TextureLayer layer = makeLayer(image.width, image.height);
    layer.rgba = std::move(image.rgba);
    return textureFromDecodedLayer(TextureFileKind::Jpeg, path, std::move(layer),
                                   image.encoding.empty() ? std::string("JPEG via libjpeg API") : image.encoding,
                                   std::move(txi));
}

TextureData readTgaTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& path,
                                        std::string txi) {
    if (!looksLikeTgaHeader(bytes)) throw TextureError("Invalid or unsupported TGA header");
    const std::uint8_t idLength = bytes[0];
    const std::uint8_t colorMapType = bytes[1];
    const std::uint8_t imageType = bytes[2];
    const std::uint16_t colorMapLength = readLE16(bytes, 5);
    const std::uint8_t colorMapSize = bytes[7];
    const std::uint16_t width = readLE16(bytes, 12);
    const std::uint16_t height = readLE16(bytes, 14);
    const std::uint8_t pixelSize = bytes[16];
    const std::uint8_t flags = bytes[17];
    std::size_t offset = 18 + idLength;
    if (offset > bytes.size()) throw TextureError("TGA id field exceeds file size");

    std::vector<std::array<std::uint8_t, 4>> palette;
    if (colorMapType != 0) {
        if (colorMapSize != 24 && colorMapSize != 32) throw TextureError("Only 24/32-bit TGA palettes are supported");
        const std::size_t entryBytes = colorMapSize / 8;
        std::uint64_t paletteBytes = 0;
        if (!parser::checkedMultiply(colorMapLength, entryBytes, paletteBytes) ||
            !parser::rangeWithin(bytes.size(), offset, paletteBytes)) {
            throw TextureError("TGA palette exceeds file size");
        }
        palette.reserve(colorMapLength);
        for (std::uint16_t i = 0; i < colorMapLength; ++i) {
            const std::uint8_t b = bytes[offset++];
            const std::uint8_t g = bytes[offset++];
            const std::uint8_t r = bytes[offset++];
            const std::uint8_t a = entryBytes == 4 ? bytes[offset++] : 255;
            palette.push_back({r, g, b, a});
        }
    }

    const bool rle = imageType == 9 || imageType == 10 || imageType == 11;
    const bool indexed = imageType == 1 || imageType == 9;
    const bool gray = imageType == 3 || imageType == 11;
    const std::size_t pixelBytes = std::max<std::size_t>(1, pixelSize / 8);
    const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(width) * height;
    if (pixelCount64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw TextureError("TGA pixel count exceeds the platform limit");
    }
    const auto pixelCount = static_cast<std::size_t>(pixelCount64);

    // Allocate only the final contiguous RGBA image, after applying the shared
    // decoded-byte budget.  Do not create one heap-backed vector per pixel.
    TextureLayer layer = makeLayer(width, height);
    const std::uint8_t origin = static_cast<std::uint8_t>((flags & 0x30) >> 4);
    const bool originRight = origin == 1 || origin == 3;
    const bool originTop = origin == 2 || origin == 3;

    auto readOneRawPixel = [&]() -> std::array<std::uint8_t, 4> {
        if (!parser::rangeWithin(bytes.size(), offset, pixelBytes)) {
            throw TextureError("TGA pixel data is truncated");
        }
        std::array<std::uint8_t, 4> pixel{};
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), pixelBytes, pixel.begin());
        offset += pixelBytes;
        return pixel;
    };

    auto storePixel = [&](std::size_t linearIndex, const std::array<std::uint8_t, 4>& pixel) {
        if (linearIndex >= pixelCount) throw TextureError("TGA packet exceeds the declared pixel count");
        const std::uint32_t sx = static_cast<std::uint32_t>(linearIndex % width);
        const std::uint32_t sy = static_cast<std::uint32_t>(linearIndex / width);
        const std::uint32_t x = originRight ? (width - 1 - sx) : sx;
        const std::uint32_t y = originTop ? sy : (height - 1 - sy);
        const std::size_t dst = (static_cast<std::size_t>(y) * width + x) * 4;
        if (!parser::rangeWithin(layer.rgba.size(), dst, UINT64_C(4))) {
            throw TextureError("TGA pixel destination exceeds the decoded image buffer");
        }
        if (indexed) {
            const std::size_t index = pixel[0];
            if (index >= palette.size()) throw TextureError("TGA palette index is out of range");
            std::copy(palette[index].begin(), palette[index].end(),
                      layer.rgba.begin() + static_cast<std::ptrdiff_t>(dst));
        } else if (gray) {
            layer.rgba[dst + 0] = pixel[0];
            layer.rgba[dst + 1] = pixel[0];
            layer.rgba[dst + 2] = pixel[0];
            layer.rgba[dst + 3] = 255;
        } else if (pixelSize == 16) {
            const std::uint16_t raw = static_cast<std::uint16_t>(pixel[0] | (pixel[1] << 8));
            layer.rgba[dst + 0] = static_cast<std::uint8_t>(((raw >> 10) & 0x1F) << 3);
            layer.rgba[dst + 1] = static_cast<std::uint8_t>(((raw >> 5) & 0x1F) << 3);
            layer.rgba[dst + 2] = static_cast<std::uint8_t>((raw & 0x1F) << 3);
            layer.rgba[dst + 3] = (raw & 0x8000) ? 255 : 0;
        } else {
            layer.rgba[dst + 0] = pixel[2];
            layer.rgba[dst + 1] = pixel[1];
            layer.rgba[dst + 2] = pixel[0];
            layer.rgba[dst + 3] = pixelSize == 32 ? pixel[3] : 255;
        }
    };

    if (!rle) {
        std::uint64_t payloadBytes = 0;
        if (!parser::checkedMultiply(pixelCount64, pixelBytes, payloadBytes) ||
            !parser::rangeWithin(bytes.size(), offset, payloadBytes)) {
            throw TextureError("TGA pixel data is truncated");
        }
        for (std::size_t i = 0; i < pixelCount; ++i) storePixel(i, readOneRawPixel());
    } else {
        std::size_t decoded = 0;
        while (decoded < pixelCount) {
            if (offset >= bytes.size()) throw TextureError("TGA RLE packet is truncated");
            const std::uint8_t code = bytes[offset++];
            const std::size_t count = (code & 0x7F) + 1;
            if (count > pixelCount - decoded) {
                throw TextureError("TGA RLE packet exceeds the declared pixel count");
            }
            if (code & 0x80) {
                const auto pixel = readOneRawPixel();
                for (std::size_t i = 0; i < count; ++i) storePixel(decoded++, pixel);
            } else {
                for (std::size_t i = 0; i < count; ++i) storePixel(decoded++, readOneRawPixel());
            }
        }
    }

    TextureData texture;
    texture.kind = TextureFileKind::Tga;
    texture.sourcePath = path;
    texture.canvasWidth = width;
    texture.canvasHeight = height;
    texture.layers.push_back(std::move(layer));
    texture.txi = std::move(txi);
    texture.sourceEncoding = std::to_string(pixelSize) + "-bit TGA" + (rle ? " RLE" : "");
    refreshHasAlpha(texture);
    applyTxiLayout(texture, true);
    return texture;
}

static void writeTgaTexture(const TextureData& texture, const std::filesystem::path& output) {
    if (!texture.hasPixels()) throw TextureError("Cannot write TGA without pixel data");
    const TextureLayer canvas = composeCanvas(texture);
    validateLayer(canvas);
    if (canvas.width > 0xFFFF || canvas.height > 0xFFFF) throw TextureError("TGA dimensions exceed 16-bit header limits");
    std::vector<std::uint8_t> out(18, 0);
    out[2] = 2; // uncompressed true color
    writeLE16(out, 12, static_cast<std::uint16_t>(canvas.width));
    writeLE16(out, 14, static_cast<std::uint16_t>(canvas.height));
    out[16] = 32;
    out[17] = 0x28; // 8 alpha bits, origin top-left
    out.reserve(18 + canvas.rgba.size());
    for (std::uint32_t y = 0; y < canvas.height; ++y) {
        for (std::uint32_t x = 0; x < canvas.width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * canvas.width + x) * 4;
            out.push_back(canvas.rgba[src + 2]);
            out.push_back(canvas.rgba[src + 1]);
            out.push_back(canvas.rgba[src + 0]);
            out.push_back(canvas.rgba[src + 3]);
        }
    }
    writeFileBytes(output, out);
}

static void writePngTexture(const TextureData& texture, const std::filesystem::path& output) {
    if (!texture.hasPixels()) throw TextureError("Cannot write PNG without pixel data");
    const TextureLayer canvas = composeCanvas(texture);
    validateLayer(canvas);
    writeFileBytes(output, internal_image::encodePngRgba(canvas.width, canvas.height, canvas.rgba));
}

static void writeJpegTexture(const TextureData& texture,
                      const std::filesystem::path& output,
                      const TextureSaveOptions& options) {
    if (!texture.hasPixels()) throw TextureError("Cannot write JPEG without pixel data");
    const TextureLayer canvas = composeCanvas(texture);
    validateLayer(canvas);
    writeFileBytes(output, internal_image::encodeJpegRgb(canvas.width,
                                                         canvas.height,
                                                         canvas.rgba,
                                                         static_cast<std::uint8_t>(std::max<int>(1, std::min<int>(100, options.jpegQuality)))));
}

static void writeBmpTexture(const TextureData& texture, const std::filesystem::path& output) {
    if (!texture.hasPixels()) throw TextureError("Cannot write BMP without pixel data");
    const TextureLayer canvas = composeCanvas(texture);
    validateLayer(canvas);
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
    writeFileBytes(output, out);
}

TextureData readTpcTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& path) {
    if (bytes.size() < 128 || !looksLikeTpcHeader(bytes)) throw TextureError("Invalid or unsupported TPC header");
    const std::uint32_t headerDataSize = readLE32(bytes, 0);
    const float alphaBlending = readLEFloat(bytes, 4);
    std::uint32_t headerWidth = readLE16(bytes, 8);
    std::uint32_t headerHeight = readLE16(bytes, 10);
    const std::uint8_t encoding = bytes[12];
    std::uint8_t mipMapCount = bytes[13] == 0 ? 1 : bytes[13];
    const bool uncompressed = headerDataSize == 0;

    std::uint32_t layerCount = 1;
    std::uint32_t layerWidth = headerWidth;
    std::uint32_t layerHeight = headerHeight;
    bool cubeMap = false;
    const std::size_t candidateCubeBaseSize = encoding == kTpcEncodingRgb
        ? dxt1Size(headerWidth, headerWidth)
        : encoding == kTpcEncodingRgba ? dxt5Size(headerWidth, headerWidth) : 0;
    if (!uncompressed && headerWidth != 0 && headerHeight / headerWidth == 6 &&
        headerHeight % headerWidth == 0 && candidateCubeBaseSize == headerDataSize) {
        cubeMap = true;
        layerCount = 6;
        layerHeight = headerHeight / 6;
    }

    TextureCompression compression = TextureCompression::None;
    std::string encodingName;
    if (uncompressed) {
        if (encoding == kTpcEncodingGray) {
            compression = TextureCompression::Gray;
            encodingName = "raw grayscale";
        } else if (encoding == kTpcEncodingSwizzledBgra) {
            compression = TextureCompression::SwizzledBgra;
            encodingName = "Xbox swizzled BGRA";
        } else {
            compression = TextureCompression::None;
            encodingName = encoding == kTpcEncodingRgba ? "raw RGBA" : "raw RGB";
        }
    } else if (encoding == kTpcEncodingRgb) {
        compression = TextureCompression::Dxt1;
        encodingName = "DXT1";
    } else if (encoding == kTpcEncodingRgba) {
        compression = TextureCompression::Dxt5;
        encodingName = "DXT5";
    } else if (encoding == kTpcEncodingGray) {
        compression = TextureCompression::Gray;
        encodingName = "grayscale payload";
    } else {
        throw TextureError("Unknown compressed TPC encoding: " + std::to_string(encoding));
    }

    auto mipSize = [&](std::uint32_t w, std::uint32_t h) -> std::size_t {
        if (!uncompressed && compression == TextureCompression::Dxt1) return dxt1Size(w, h);
        if (!uncompressed && compression == TextureCompression::Dxt5) return dxt5Size(w, h);
        if (encoding == kTpcEncodingGray) return static_cast<std::size_t>(w) * h;
        if (encoding == kTpcEncodingRgb) return static_cast<std::size_t>(w) * h * 3;
        return static_cast<std::size_t>(w) * h * 4;
    };

    std::size_t initialPayloadSize = 0;
    if (!uncompressed) {
        initialPayloadSize = headerDataSize;
        if (!cubeMap) {
            std::uint32_t w = layerWidth;
            std::uint32_t h = layerHeight;
            for (std::uint8_t i = 1; i < mipMapCount; ++i) {
                w = std::max<std::uint32_t>(1, w / 2);
                h = std::max<std::uint32_t>(1, h / 2);
                initialPayloadSize += mipSize(w, h);
            }
        } else {
            std::size_t oneLayer = headerDataSize;
            std::uint32_t w = layerWidth;
            std::uint32_t h = layerHeight;
            for (std::uint8_t i = 1; i < mipMapCount; ++i) {
                w = std::max<std::uint32_t>(1, w / 2);
                h = std::max<std::uint32_t>(1, h / 2);
                oneLayer += mipSize(w, h);
            }
            initialPayloadSize = oneLayer * layerCount;
        }
    } else {
        std::size_t oneLayer = mipSize(layerWidth, layerHeight);
        std::uint32_t w = layerWidth;
        std::uint32_t h = layerHeight;
        for (std::uint8_t i = 1; i < mipMapCount; ++i) {
            w = std::max<std::uint32_t>(1, w / 2);
            h = std::max<std::uint32_t>(1, h / 2);
            oneLayer += mipSize(w, h);
        }
        initialPayloadSize = oneLayer * layerCount;
    }

    std::size_t txiOffset = 128 + initialPayloadSize;
    if (txiOffset > bytes.size()) {
        // Some hand-authored TPCs have inconsistent mip counts; clamp to the payload.
        txiOffset = bytes.size();
    }
    std::string txi;
    if (txiOffset < bytes.size()) {
        txi = sanitizeTxiPayload(std::string(bytes.begin() + static_cast<std::ptrdiff_t>(txiOffset), bytes.end()));
    }

    const TxiFeatures features = parseTxiFeatures(txi);
    bool animated = false;
    if (!cubeMap && iequals(features.procedureType, "cycle") && features.numX > 0 && features.numY > 0 && features.fps > 0.0) {
        animated = true;
        const auto frameCount = checkedAnimationFrameCount(features);
        layerCount = static_cast<std::uint32_t>(frameCount);
        layerWidth = features.defaultWidth ? features.defaultWidth : headerWidth / features.numX;
        layerHeight = features.defaultHeight ? features.defaultHeight : headerHeight / features.numY;
        if (layerWidth == 0 || layerHeight == 0) throw TextureError("TPC animation TXI produced zero-size frames");
        if (checkedDimensionProduct(layerWidth, features.numX, "TPC animation grid width") > headerWidth ||
            checkedDimensionProduct(layerHeight, features.numY, "TPC animation grid height") > headerHeight) {
            throw TextureError("TPC animation grid does not fit within the header dimensions");
        }
        if (headerDataSize > 0) {
            TextureLayer frameDimensions;
            frameDimensions.width = layerWidth;
            frameDimensions.height = layerHeight;
            mipMapCount = static_cast<std::uint8_t>(
                std::min<std::uint32_t>(255u, mipCountForLayer(frameDimensions, true)));
        }
    }

    std::uint64_t oneLayerDecodedBytes = 0;
    std::uint32_t budgetWidth = layerWidth;
    std::uint32_t budgetHeight = layerHeight;
    for (std::uint8_t mip = 0; mip < mipMapCount; ++mip) {
        std::uint64_t next = 0;
        if (!parser::checkedAdd(oneLayerDecodedBytes,
                                checkedRgbaByteCount(budgetWidth, budgetHeight, "TPC mipmap"), next)) {
            throw TextureError("TPC decoded mipmaps exceed the parser memory limit");
        }
        oneLayerDecodedBytes = next;
        budgetWidth = std::max<std::uint32_t>(1, budgetWidth / 2);
        budgetHeight = std::max<std::uint32_t>(1, budgetHeight / 2);
    }
    std::uint64_t totalDecodedBytes = 0;
    if (!parser::checkedMultiply(oneLayerDecodedBytes, layerCount, totalDecodedBytes) ||
        totalDecodedBytes > parser::maxDecodedBytes()) {
        throw TextureError("TPC decoded layers exceed the parser memory limit");
    }

    TextureData texture;
    texture.kind = TextureFileKind::Tpc;
    texture.sourcePath = path;
    texture.canvasWidth = headerWidth;
    texture.canvasHeight = headerHeight;
    texture.txi = txi;
    texture.alphaBlending = alphaBlending;
    texture.preferredCompression = compression;
    texture.compressed = !uncompressed && (compression == TextureCompression::Dxt1 || compression == TextureCompression::Dxt5);
    texture.cubeMap = cubeMap;
    texture.animated = animated;
    texture.sourceMipMapCount = mipMapCount;
    texture.sourceEncoding = encodingName;

    std::size_t offset = 128;
    const std::size_t payloadLimit = txiOffset;
    for (std::uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
        std::uint32_t w = layerWidth;
        std::uint32_t h = layerHeight;
        TextureLayer layer;
        bool haveBase = false;
        for (std::uint8_t mip = 0; mip < mipMapCount; ++mip) {
            const std::size_t size = mipSize(w, h);
            if (offset + size > payloadLimit) {
                if (mip == 0) throw TextureError("TPC payload is truncated before layer data");
                break;
            }
            TextureLayer decoded;
            if (!uncompressed && compression == TextureCompression::Dxt1) {
                decoded = flipLayerVerticalCopy(decodeDxt1(bytes.data() + offset, size, w, h));
            } else if (!uncompressed && compression == TextureCompression::Dxt5) {
                decoded = flipLayerVerticalCopy(decodeDxt5(bytes.data() + offset, size, w, h));
            } else {
                decoded = decodeRawLayer(bytes.data() + offset, size, w, h, encoding, true);
            }
            if (!haveBase) {
                layer = std::move(decoded);
                haveBase = true;
            } else {
                layer.mipmaps.push_back(std::move(decoded));
            }
            offset += size;
            w = std::max<std::uint32_t>(1, w / 2);
            h = std::max<std::uint32_t>(1, h / 2);
        }
        if (!haveBase) throw TextureError("TPC payload does not contain a base mipmap");
        texture.layers.push_back(std::move(layer));
    }
    if (cubeMap) {
        normalizeTpcCubeMap(texture.layers);
        texture.cubeFaces = allCubeFaces();
    }
    refreshHasAlpha(texture);
    return texture;
}

TextureLayer decodeTxbRawLayer(const std::uint8_t* data,
                               std::size_t size,
                               std::uint32_t width,
                               std::uint32_t height,
                               std::uint8_t encoding) {
    const bool gray = encoding == kTxbEncodingGray;
    const std::size_t bytesPerPixel = gray ? 1u : 4u;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    if (size < pixelCount * bytesPerPixel) throw TextureError("TXB raw payload is truncated");
    const bool swizzled = isPowerOfTwo(width) && isPowerOfTwo(height);
    TextureLayer layer = makeLayer(width, height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t destinationPixel = static_cast<std::size_t>(y) * width + x;
            const std::size_t sourcePixel = swizzled
                ? static_cast<std::size_t>(swizzledPixelOffset(x, y, width, height))
                : destinationPixel;
            if (sourcePixel >= pixelCount) throw TextureError("TXB swizzle references a pixel outside the payload");
            if (gray) {
                const auto value = data[sourcePixel];
                layer.rgba[destinationPixel * 4 + 0] = value;
                layer.rgba[destinationPixel * 4 + 1] = value;
                layer.rgba[destinationPixel * 4 + 2] = value;
                layer.rgba[destinationPixel * 4 + 3] = 255;
            } else {
                const std::size_t source = sourcePixel * 4;
                layer.rgba[destinationPixel * 4 + 0] = data[source + 2];
                layer.rgba[destinationPixel * 4 + 1] = data[source + 1];
                layer.rgba[destinationPixel * 4 + 2] = data[source + 0];
                layer.rgba[destinationPixel * 4 + 3] = data[source + 3];
            }
        }
    }
    return flipLayerVerticalCopy(std::move(layer));
}

TextureData readTxbTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& path) {
    if (bytes.size() < 128 || !looksLikeTxbHeader(bytes)) throw TextureError("Invalid or unsupported TXB header");
    const std::uint32_t dataSize = readLE32(bytes, 0);
    const std::uint32_t width = readLE16(bytes, 8);
    const std::uint32_t height = readLE16(bytes, 10);
    const std::uint8_t encoding = bytes[12];
    const std::uint8_t mipMapCount = bytes[13] == 0 ? 1 : bytes[13];
    if (!parser::rangeWithin(bytes.size(), 128, dataSize)) throw TextureError("TXB texture payload is truncated");
    (void)checkedMipChainDecodedByteCount(width, height, mipMapCount, 1, "TXB mipmaps");

    TextureData texture;
    texture.kind = TextureFileKind::Txb;
    texture.sourcePath = path;
    texture.canvasWidth = width;
    texture.canvasHeight = height;
    texture.sourceMipMapCount = mipMapCount;
    texture.alphaBlending = readLEFloat(bytes, 4);
    texture.compressed = encoding == kTxbEncodingDxt1 || encoding == kTxbEncodingDxt5;
    if (encoding == kTxbEncodingBgra) {
        texture.preferredCompression = TextureCompression::SwizzledBgra;
        texture.sourceEncoding = "Xbox TXB swizzled BGRA";
    } else if (encoding == kTxbEncodingGray) {
        texture.preferredCompression = TextureCompression::Gray;
        texture.sourceEncoding = "Xbox TXB swizzled grayscale";
    } else if (encoding == kTxbEncodingDxt1) {
        texture.preferredCompression = TextureCompression::Dxt1;
        texture.sourceEncoding = "Xbox TXB DXT1";
    } else {
        texture.preferredCompression = TextureCompression::Dxt5;
        texture.sourceEncoding = "Xbox TXB DXT5";
    }

    TextureLayer base;
    std::size_t offset = 128;
    std::uint32_t mipWidth = width;
    std::uint32_t mipHeight = height;
    for (std::uint8_t mip = 0; mip < mipMapCount; ++mip) {
        std::size_t mipSize = 0;
        if (encoding == kTxbEncodingDxt1) mipSize = dxt1Size(mipWidth, mipHeight);
        else if (encoding == kTxbEncodingDxt5) mipSize = dxt5Size(mipWidth, mipHeight);
        else mipSize = static_cast<std::size_t>(mipWidth) * mipHeight * (encoding == kTxbEncodingGray ? 1u : 4u);
        if (!parser::rangeWithin(static_cast<std::size_t>(128) + dataSize, offset, mipSize)) {
            throw TextureError("TXB mipmap payload is truncated");
        }
        TextureLayer decoded;
        if (encoding == kTxbEncodingDxt1) {
            decoded = flipLayerVerticalCopy(decodeDxt1(bytes.data() + offset, mipSize, mipWidth, mipHeight));
        } else if (encoding == kTxbEncodingDxt5) {
            decoded = flipLayerVerticalCopy(decodeDxt5(bytes.data() + offset, mipSize, mipWidth, mipHeight));
        } else {
            decoded = decodeTxbRawLayer(bytes.data() + offset, mipSize, mipWidth, mipHeight, encoding);
        }
        if (mip == 0) base = std::move(decoded);
        else base.mipmaps.push_back(std::move(decoded));
        offset += mipSize;
        mipWidth = std::max<std::uint32_t>(1, mipWidth / 2);
        mipHeight = std::max<std::uint32_t>(1, mipHeight / 2);
    }
    texture.layers.push_back(std::move(base));
    const std::size_t txiOffset = static_cast<std::size_t>(128) + dataSize;
    if (txiOffset < bytes.size()) {
        texture.txi = sanitizeTxiPayload(std::string(bytes.begin() + static_cast<std::ptrdiff_t>(txiOffset), bytes.end()));
    }
    texture.notes = "Xbox TXB input is supported for viewing and conversion; NeoTPC does not write TXB containers. ";
    refreshHasAlpha(texture);
    return texture;
}

static void writeTpcTexture(const TextureData& input, const std::filesystem::path& output, const TextureSaveOptions& options) {
    if (!input.hasPixels()) throw TextureError("Cannot write TPC without pixel data");
    TextureData texture = input;
    refreshHasAlpha(texture);
    std::vector<TextureLayer> layers = withSaveFlips(texture.layers, options);
    if (layers.empty()) throw TextureError("No texture layers to write");
    const bool hasAlpha = std::any_of(layers.begin(), layers.end(), layerHasAlpha);
    TextureCompression compression = chooseAutoCompression(texture, options);
    if (compression == TextureCompression::Dxt1 && hasAlpha) {
        // Match tga2tpc's automatic spirit: do not throw away alpha unless user explicitly forced DXT1.
        if (options.compression == TextureCompression::Auto) compression = TextureCompression::Dxt5;
    }

    const TxiFeatures features = parseTxiFeatures(texture.txi);
    const bool animated = texture.animated || (iequals(features.procedureType, "cycle") && features.numX > 0 &&
                                                features.numY > 0 && features.fps > 0.0 && layers.size() > 1);
    const bool cube = texture.cubeMap || (features.cube && layers.size() == 6);
    if (compression == TextureCompression::Dxt3) {
        throw TextureError("DXT3 is a DDS encoding; Odyssey TPC supports DXT1 or DXT5 compression");
    }
    if ((animated || cube) && compression != TextureCompression::Dxt1 && compression != TextureCompression::Dxt5) {
        throw TextureError("TPC animated/cube textures require DXT compression in this implementation");
    }
    if (compression == TextureCompression::SwizzledBgra &&
        std::any_of(layers.begin(), layers.end(), [](const TextureLayer& layer) {
            return !isPowerOfTwo(layer.width) || !isPowerOfTwo(layer.height);
        })) {
        throw TextureError("Xbox swizzled BGRA TPC output requires power-of-two layer dimensions");
    }
    if (cube) denormalizeTpcCubeMap(layers);

    const auto& first = layers.front();
    for (const auto& layer : layers) {
        validateLayer(layer);
        if (layer.width != first.width || layer.height != first.height) {
            throw TextureError("TPC layers must have matching dimensions");
        }
    }
    if (animated) {
        const auto frameCount = checkedAnimationFrameCount(features);
        if (layers.size() != frameCount) {
            throw TextureError("TPC animation layer count does not match the TXI grid");
        }
        for (auto& layer : layers) {
            TextureLayer frameDimensions;
            frameDimensions.width = layer.width;
            frameDimensions.height = layer.height;
            const auto requiredMipCount = mipCountForLayer(frameDimensions, true);
            if (layer.mipmaps.size() + 1 != requiredMipCount) layer.mipmaps.clear();
        }
    }

    // Animated TPC stores a sentinel mip count in the header; readers derive a
    // complete chain from each frame's dimensions, so animation output must
    // always carry that full chain even when ordinary mip generation is off.
    const bool generate = (options.generateMipmaps || animated) && compression != TextureCompression::Gray;
    auto mipChains = makeMipChainPerLayer(layers, generate, options.bicubicMipmaps, true);
    const std::uint8_t mipMapCount = static_cast<std::uint8_t>(std::min<std::size_t>(255, mipChains.front().size()));

    auto encodeMip = [&](const TextureLayer& layer) -> std::vector<std::uint8_t> {
        return encodeLayerRaw(layer, compression, hasAlpha, options);
    };

    std::uint32_t headerWidth = first.width;
    std::uint32_t headerHeight = first.height;
    if (cube) {
        headerHeight = checkedDimensionProduct(first.width, 6, "TPC cube height");
    } else if (animated && features.numX > 0 && features.numY > 0) {
        headerWidth = checkedDimensionProduct(first.width, features.numX, "TPC animation width");
        headerHeight = checkedDimensionProduct(first.height, features.numY, "TPC animation height");
    } else if (texture.canvasWidth > 0 && texture.canvasHeight > 0 && layers.size() == 1) {
        headerWidth = texture.canvasWidth;
        headerHeight = texture.canvasHeight;
    }
    if (headerWidth > 0xFFFF || headerHeight > 0xFFFF) throw TextureError("TPC dimensions exceed 16-bit header limits");

    std::uint8_t tpcEncoding = kTpcEncodingRgba;
    std::uint32_t headerDataSize = 0;
    if (compression == TextureCompression::Dxt1) {
        tpcEncoding = kTpcEncodingRgb;
        headerDataSize = static_cast<std::uint32_t>(dxt1Size(first.width, first.height));
    } else if (compression == TextureCompression::Dxt5) {
        tpcEncoding = kTpcEncodingRgba;
        headerDataSize = static_cast<std::uint32_t>(dxt5Size(first.width, first.height));
    } else if (compression == TextureCompression::Gray) {
        tpcEncoding = kTpcEncodingGray;
        headerDataSize = 0; // xoreos-compatible uncompressed grayscale
    } else if (compression == TextureCompression::SwizzledBgra) {
        tpcEncoding = kTpcEncodingSwizzledBgra;
        headerDataSize = 0;
    } else {
        tpcEncoding = hasAlpha ? kTpcEncodingRgba : kTpcEncodingRgb;
        headerDataSize = 0;
    }

    if (animated && (compression == TextureCompression::Dxt1 || compression == TextureCompression::Dxt5)) {
        std::uint64_t total = 0;
        for (const auto& chain : mipChains) {
            for (const auto& mip : chain) {
                const auto encodedBytes = bytesForEncoding(compression, mip.width, mip.height, hasAlpha);
                if (!parser::checkedAdd(total, encodedBytes, total) ||
                    total > std::numeric_limits<std::uint32_t>::max()) {
                    throw TextureError("TPC encoded animation payload exceeds the format limit");
                }
            }
        }
        headerDataSize = static_cast<std::uint32_t>(total);
    }

    std::vector<std::uint8_t> out(128, 0);
    writeLE32(out, 0, headerDataSize);
    writeLEFloat(out, 4, options.alphaBlending);
    writeLE16(out, 8, static_cast<std::uint16_t>(headerWidth));
    writeLE16(out, 10, static_cast<std::uint16_t>(headerHeight));
    out[12] = tpcEncoding;
    out[13] = animated ? 1 : mipMapCount;

    for (const auto& chain : mipChains) {
        for (const auto& mip : chain) {
            const auto encoded = encodeMip(mip);
            out.insert(out.end(), encoded.begin(), encoded.end());
        }
    }
    const std::string txi = normalizeTxiFooter(texture.txi);
    out.insert(out.end(), txi.begin(), txi.end());
    writeFileBytes(output, out);
}

namespace {

enum class DdsStorage {
    Dxt1,
    Dxt3,
    Dxt5,
    Bgra32,
    Bgr24,
    A1R5G5B5,
    R5G6B5,
    Argb4444,
};

std::size_t ddsMipSize(DdsStorage storage, std::uint32_t width, std::uint32_t height) {
    switch (storage) {
    case DdsStorage::Dxt1: return dxt1Size(width, height);
    case DdsStorage::Dxt3:
    case DdsStorage::Dxt5: return dxt5Size(width, height);
    case DdsStorage::Bgra32: return static_cast<std::size_t>(width) * height * 4;
    case DdsStorage::Bgr24: return static_cast<std::size_t>(width) * height * 3;
    case DdsStorage::A1R5G5B5:
    case DdsStorage::R5G6B5:
    case DdsStorage::Argb4444: return static_cast<std::size_t>(width) * height * 2;
    }
    return 0;
}

std::size_t ddsRawBytesPerPixel(DdsStorage storage) {
    switch (storage) {
    case DdsStorage::Bgra32: return 4;
    case DdsStorage::Bgr24: return 3;
    case DdsStorage::A1R5G5B5:
    case DdsStorage::R5G6B5:
    case DdsStorage::Argb4444: return 2;
    case DdsStorage::Dxt1:
    case DdsStorage::Dxt3:
    case DdsStorage::Dxt5: return 0;
    }
    return 0;
}

std::size_t checkedDdsRowPitch(std::uint32_t width, std::size_t bytesPerPixel, std::size_t alignment) {
    const std::uint64_t tight = static_cast<std::uint64_t>(width) * bytesPerPixel;
    const std::uint64_t aligned = alignment <= 1
        ? tight
        : (tight + alignment - 1) / alignment * alignment;
    if (aligned > std::numeric_limits<std::size_t>::max()) throw TextureError("DDS row pitch overflows");
    return static_cast<std::size_t>(aligned);
}

std::size_t checkedDdsSurfaceSize(std::size_t rowPitch, std::uint32_t height) {
    std::uint64_t size = 0;
    if (!parser::checkedMultiply(rowPitch, height, size) ||
        size > std::numeric_limits<std::size_t>::max()) {
        throw TextureError("DDS surface payload size overflows");
    }
    return static_cast<std::size_t>(size);
}

std::uint64_t ddsRawPayloadSize(std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t mipmaps,
                                std::uint32_t faceCount,
                                std::size_t bytesPerPixel,
                                std::size_t topPitch,
                                std::size_t alignment) {
    std::uint64_t total = 0;
    for (std::uint32_t face = 0; face < faceCount; ++face) {
        std::uint32_t mipWidth = width;
        std::uint32_t mipHeight = height;
        for (std::uint32_t mip = 0; mip < mipmaps; ++mip) {
            const std::size_t pitch = mip == 0
                ? topPitch
                : checkedDdsRowPitch(mipWidth, bytesPerPixel, alignment);
            std::uint64_t level = 0;
            if (!parser::checkedMultiply(pitch, mipHeight, level) ||
                !parser::checkedAdd(total, level, total)) {
                throw TextureError("DDS surface payload size overflows");
            }
            mipWidth = std::max<std::uint32_t>(1, mipWidth / 2);
            mipHeight = std::max<std::uint32_t>(1, mipHeight / 2);
        }
    }
    return total;
}

std::size_t inferDdsRowAlignment(std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t mipmaps,
                                 std::uint32_t faceCount,
                                 std::size_t bytesPerPixel,
                                 std::size_t topPitch,
                                 std::size_t availableBytes) {
    const auto tightTop = checkedDdsRowPitch(width, bytesPerPixel, 1);
    std::size_t bestAlignment = 1;
    std::uint64_t bestRemainder = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t alignment = 1; alignment <= 4096; alignment *= 2) {
        if (checkedDdsRowPitch(width, bytesPerPixel, alignment) != topPitch) continue;
        const auto required = ddsRawPayloadSize(width, height, mipmaps, faceCount,
                                                bytesPerPixel, topPitch, alignment);
        if (required <= availableBytes && availableBytes - required < bestRemainder) {
            bestAlignment = alignment;
            bestRemainder = availableBytes - required;
        }
    }
    // An uncommon writer may use an arbitrary base pitch. Preserve that pitch
    // for level zero and use tightly packed rows for lower mip levels.
    if (topPitch < tightTop) throw TextureError("DDS row pitch is smaller than the pixel row");
    return bestAlignment;
}

TextureLayer decodeDdsMip(const std::uint8_t* data,
                          std::size_t size,
                          std::uint32_t width,
                          std::uint32_t height,
                          DdsStorage storage,
                          std::size_t rowPitch = 0) {
    const auto bytesPerPixel = ddsRawBytesPerPixel(storage);
    const auto tightPitch = bytesPerPixel == 0 ? 0 : checkedDdsRowPitch(width, bytesPerPixel, 1);
    if (bytesPerPixel != 0 && rowPitch == 0) rowPitch = tightPitch;
    if (bytesPerPixel != 0 && rowPitch < tightPitch) throw TextureError("DDS row pitch is smaller than the pixel row");
    const auto required = bytesPerPixel == 0
        ? ddsMipSize(storage, width, height)
        : checkedDdsSurfaceSize(rowPitch, height);
    if (size < required) throw TextureError("DDS mipmap payload is truncated");
    if (storage == DdsStorage::Dxt1) return decodeDxt1(data, required, width, height);
    if (storage == DdsStorage::Dxt3) return decodeDxt3(data, required, width, height);
    if (storage == DdsStorage::Dxt5) return decodeDxt5(data, required, width, height);

    TextureLayer layer = makeLayer(width, height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* row = data + static_cast<std::size_t>(y) * rowPitch;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (storage == DdsStorage::Bgra32) {
                layer.rgba[index * 4 + 0] = row[x * 4 + 2];
                layer.rgba[index * 4 + 1] = row[x * 4 + 1];
                layer.rgba[index * 4 + 2] = row[x * 4 + 0];
                layer.rgba[index * 4 + 3] = row[x * 4 + 3];
            } else if (storage == DdsStorage::Bgr24) {
                layer.rgba[index * 4 + 0] = row[x * 3 + 2];
                layer.rgba[index * 4 + 1] = row[x * 3 + 1];
                layer.rgba[index * 4 + 2] = row[x * 3 + 0];
                layer.rgba[index * 4 + 3] = 255;
            } else {
                const std::uint16_t pixel = static_cast<std::uint16_t>(row[x * 2] | (row[x * 2 + 1] << 8));
                if (storage == DdsStorage::A1R5G5B5) {
                    layer.rgba[index * 4 + 0] = maskedChannelToByte(pixel, 0x7C00u);
                    layer.rgba[index * 4 + 1] = maskedChannelToByte(pixel, 0x03E0u);
                    layer.rgba[index * 4 + 2] = maskedChannelToByte(pixel, 0x001Fu);
                    layer.rgba[index * 4 + 3] = (pixel & 0x8000u) ? 255 : 0;
                } else if (storage == DdsStorage::R5G6B5) {
                    layer.rgba[index * 4 + 0] = maskedChannelToByte(pixel, 0xF800u);
                    layer.rgba[index * 4 + 1] = maskedChannelToByte(pixel, 0x07E0u);
                    layer.rgba[index * 4 + 2] = maskedChannelToByte(pixel, 0x001Fu);
                    layer.rgba[index * 4 + 3] = 255;
                } else {
                    layer.rgba[index * 4 + 0] = maskedChannelToByte(pixel, 0x0F00u);
                    layer.rgba[index * 4 + 1] = maskedChannelToByte(pixel, 0x00F0u);
                    layer.rgba[index * 4 + 2] = maskedChannelToByte(pixel, 0x000Fu);
                    layer.rgba[index * 4 + 3] = maskedChannelToByte(pixel, 0xF000u);
                }
            }
        }
    }
    return layer;
}

std::vector<std::uint8_t> encodeDdsMip(const TextureLayer& mip,
                                       TextureCompression compression,
                                       const TextureSaveOptions& options) {
    if (compression != TextureCompression::None) return encodeLayerRaw(mip, compression, true, options);
    std::vector<std::uint8_t> out;
    out.reserve(mip.rgba.size());
    for (std::size_t index = 0; index + 3 < mip.rgba.size(); index += 4) {
        out.push_back(mip.rgba[index + 2]);
        out.push_back(mip.rgba[index + 1]);
        out.push_back(mip.rgba[index + 0]);
        out.push_back(mip.rgba[index + 3]);
    }
    return out;
}

} // namespace

TextureData readDdsTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& path,
                                        std::string txi) {
    if (bytes.size() < 20) throw TextureError("DDS file is too small");
    TextureData texture;
    texture.kind = TextureFileKind::Dds;
    texture.sourcePath = path;
    texture.txi = sanitizeTxiPayload(std::move(txi));

    if (bytes.size() >= 128 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ') {
        if (readLE32(bytes, 4) != 124) throw TextureError("DDS header size is not 124");
        const std::uint32_t flags = readLE32(bytes, 8);
        const std::uint32_t height = readLE32(bytes, 12);
        const std::uint32_t width = readLE32(bytes, 16);
        const std::uint32_t declaredPitch = readLE32(bytes, 20);
        if (width == 0 || height == 0 || width >= 0x8000 || height >= 0x8000) {
            throw TextureError("DDS dimensions are invalid or too large");
        }
        std::uint32_t mipmaps = readLE32(bytes, 28);
        if ((flags & 0x00020000u) == 0 || mipmaps == 0) mipmaps = 1;
        mipmaps = std::min<std::uint32_t>(mipmaps, 255);
        const std::uint32_t pfFlags = readLE32(bytes, 80);
        const std::string fourcc(reinterpret_cast<const char*>(bytes.data() + 84), 4);
        const std::uint32_t bitCount = readLE32(bytes, 88);
        const std::uint32_t rMask = readLE32(bytes, 92);
        const std::uint32_t gMask = readLE32(bytes, 96);
        const std::uint32_t bMask = readLE32(bytes, 100);
        const std::uint32_t aMask = readLE32(bytes, 104);

        DdsStorage storage;
        if ((pfFlags & 0x4u) && fourcc == "DXT1") {
            storage = DdsStorage::Dxt1;
            texture.preferredCompression = TextureCompression::Dxt1;
            texture.compressed = true;
            texture.sourceEncoding = "DDS DXT1";
        } else if ((pfFlags & 0x4u) && fourcc == "DXT3") {
            storage = DdsStorage::Dxt3;
            texture.preferredCompression = TextureCompression::Dxt3;
            texture.compressed = true;
            texture.sourceEncoding = "DDS DXT3";
        } else if ((pfFlags & 0x4u) && fourcc == "DXT5") {
            storage = DdsStorage::Dxt5;
            texture.preferredCompression = TextureCompression::Dxt5;
            texture.compressed = true;
            texture.sourceEncoding = "DDS DXT5";
        } else if ((pfFlags & 0x40u) && bitCount == 32 && rMask == 0x00FF0000 &&
                   gMask == 0x0000FF00 && bMask == 0x000000FF && aMask == 0xFF000000) {
            storage = DdsStorage::Bgra32;
            texture.preferredCompression = TextureCompression::None;
            texture.sourceEncoding = "DDS BGRA32";
        } else if ((pfFlags & 0x40u) && bitCount == 24 && rMask == 0x00FF0000 &&
                   gMask == 0x0000FF00 && bMask == 0x000000FF) {
            storage = DdsStorage::Bgr24;
            texture.preferredCompression = TextureCompression::None;
            texture.sourceEncoding = "DDS BGR24";
        } else if ((pfFlags & 0x40u) && (pfFlags & 0x1u) && bitCount == 16 &&
                   rMask == 0x7C00u && gMask == 0x03E0u && bMask == 0x001Fu && aMask == 0x8000u) {
            storage = DdsStorage::A1R5G5B5;
            texture.preferredCompression = TextureCompression::None;
            texture.sourceEncoding = "DDS A1R5G5B5";
        } else if ((pfFlags & 0x40u) && !(pfFlags & 0x1u) && bitCount == 16 &&
                   rMask == 0xF800u && gMask == 0x07E0u && bMask == 0x001Fu) {
            storage = DdsStorage::R5G6B5;
            texture.preferredCompression = TextureCompression::None;
            texture.sourceEncoding = "DDS R5G6B5";
        } else if ((pfFlags & 0x40u) && (pfFlags & 0x1u) && bitCount == 16 &&
                   rMask == 0x0F00u && gMask == 0x00F0u && bMask == 0x000Fu && aMask == 0xF000u) {
            storage = DdsStorage::Argb4444;
            texture.preferredCompression = TextureCompression::None;
            texture.sourceEncoding = "DDS ARGB4444";
        } else {
            throw TextureError("Unsupported DDS pixel format");
        }

        const std::uint32_t caps2 = readLE32(bytes, 112);
        std::uint32_t faceCount = 1;
        if ((caps2 & 0x00000200u) != 0) {
            const std::uint32_t faceBits = caps2 & 0x0000FC00u;
            texture.cubeFaces.clear();
            for (const auto face : kAllCubeFaces) {
                if ((faceBits & ddsCubeFaceBit(face)) != 0) texture.cubeFaces.push_back(face);
            }
            if (texture.cubeFaces.empty()) texture.cubeFaces = allCubeFaces();
            faceCount = static_cast<std::uint32_t>(texture.cubeFaces.size());
            texture.cubeMap = true;
        }
        (void)checkedMipChainDecodedByteCount(width, height, mipmaps, faceCount, "DDS surfaces");

        const std::size_t bytesPerPixel = ddsRawBytesPerPixel(storage);
        std::size_t topPitch = bytesPerPixel == 0 ? 0 : checkedDdsRowPitch(width, bytesPerPixel, 1);
        std::size_t rowAlignment = 1;
        if (bytesPerPixel != 0 && (flags & 0x00000008u) != 0) {
            topPitch = declaredPitch;
            rowAlignment = inferDdsRowAlignment(width, height, mipmaps, faceCount, bytesPerPixel,
                                                topPitch, bytes.size() - 128);
        }

        std::size_t offset = 128;
        for (std::uint32_t face = 0; face < faceCount; ++face) {
            TextureLayer layer;
            std::uint32_t mipWidth = width;
            std::uint32_t mipHeight = height;
            for (std::uint32_t mip = 0; mip < mipmaps; ++mip) {
                const std::size_t rowPitch = bytesPerPixel == 0
                    ? 0
                    : (mip == 0 ? topPitch : checkedDdsRowPitch(mipWidth, bytesPerPixel, rowAlignment));
                const auto size = bytesPerPixel == 0
                    ? ddsMipSize(storage, mipWidth, mipHeight)
                    : checkedDdsSurfaceSize(rowPitch, mipHeight);
                if (!parser::rangeWithin(bytes.size(), offset, size)) throw TextureError("DDS surface payload is truncated");
                auto decoded = decodeDdsMip(bytes.data() + offset, size, mipWidth, mipHeight, storage, rowPitch);
                if (mip == 0) layer = std::move(decoded);
                else layer.mipmaps.push_back(std::move(decoded));
                offset += size;
                mipWidth = std::max<std::uint32_t>(1, mipWidth / 2);
                mipHeight = std::max<std::uint32_t>(1, mipHeight / 2);
            }
            texture.layers.push_back(std::move(layer));
        }
        texture.canvasWidth = width;
        texture.canvasHeight = height;
        texture.sourceMipMapCount = mipmaps;
    } else {
        const std::uint32_t width = readLE32(bytes, 0);
        const std::uint32_t height = readLE32(bytes, 4);
        const std::uint32_t bpp = readLE32(bytes, 8);
        const std::uint32_t dataSize = readLE32(bytes, 12);
        if (width == 0 || height == 0 || width >= 0x8000 || height >= 0x8000 || (bpp != 3 && bpp != 4)) {
            throw TextureError("Invalid BioWare DDS header");
        }
        const DdsStorage storage = bpp == 3 ? DdsStorage::Dxt1 : DdsStorage::Dxt5;
        if (dataSize > bytes.size() - 20) throw TextureError("BioWare DDS payload is truncated");
        TextureLayer layer;
        std::size_t offset = 20;
        std::uint32_t mipWidth = width;
        std::uint32_t mipHeight = height;
        std::uint32_t mipCount = 0;
        std::uint64_t decodedBytes = 0;
        while (mipCount < 255) {
            const auto size = ddsMipSize(storage, mipWidth, mipHeight);
            if (!parser::rangeWithin(bytes.size(), offset, size)) break;
            std::uint64_t nextDecodedBytes = 0;
            if (!parser::checkedAdd(decodedBytes,
                                    checkedRgbaByteCount(mipWidth, mipHeight, "BioWare DDS mipmap"),
                                    nextDecodedBytes) || nextDecodedBytes > parser::maxDecodedBytes()) {
                throw TextureError("BioWare DDS mipmaps exceed the decoded-image memory limit");
            }
            decodedBytes = nextDecodedBytes;
            auto decoded = decodeDdsMip(bytes.data() + offset, size, mipWidth, mipHeight, storage);
            if (mipCount == 0) layer = std::move(decoded);
            else layer.mipmaps.push_back(std::move(decoded));
            ++mipCount;
            offset += size;
            if (mipWidth == 1 && mipHeight == 1) break;
            mipWidth = std::max<std::uint32_t>(1, mipWidth / 2);
            mipHeight = std::max<std::uint32_t>(1, mipHeight / 2);
        }
        if (mipCount == 0) throw TextureError("BioWare DDS contains no complete mipmap");
        texture.canvasWidth = width;
        texture.canvasHeight = height;
        texture.layers.push_back(std::move(layer));
        texture.sourceMipMapCount = mipCount;
        texture.preferredCompression = bpp == 3 ? TextureCompression::Dxt1 : TextureCompression::Dxt5;
        texture.compressed = true;
        texture.sourceEncoding = bpp == 3 ? "BioWare DDS DXT1" : "BioWare DDS DXT5";
    }
    refreshHasAlpha(texture);
    applyTxiLayout(texture);
    return texture;
}

static void writeDdsTexture(const TextureData& texture, const std::filesystem::path& output, const TextureSaveOptions& options) {
    if (!texture.hasPixels()) throw TextureError("Cannot write DDS without pixel data");
    TextureData copy = texture;
    refreshHasAlpha(copy);
    std::vector<TextureLayer> layers = withSaveFlips(copy.layers, options);
    const bool cubeMap = copy.cubeMap;
    std::vector<CubeFace> cubeFaces = copy.cubeFaces;
    if (cubeMap && cubeFaces.empty()) {
        if (layers.size() != 6) throw TextureError("DDS cubemap export requires face identities for a partial cubemap");
        cubeFaces = allCubeFaces();
    }
    if (cubeMap && cubeFaces.size() != layers.size()) {
        throw TextureError("DDS cubemap face identities do not match the texture layers");
    }
    if (cubeMap) {
        std::set<CubeFace> uniqueFaces(cubeFaces.begin(), cubeFaces.end());
        if (uniqueFaces.size() != cubeFaces.size()) throw TextureError("DDS cubemap contains duplicate face identities");

        // DDS stores selected faces in fixed +X, -X, +Y, -Y, +Z, -Z
        // order. Keep each layer paired with its identity while normalizing
        // the serialized order, including for partial cubemaps.
        std::vector<TextureLayer> orderedLayers;
        std::vector<CubeFace> orderedFaces;
        orderedLayers.reserve(layers.size());
        orderedFaces.reserve(cubeFaces.size());
        for (const auto canonicalFace : kAllCubeFaces) {
            const auto found = std::find(cubeFaces.begin(), cubeFaces.end(), canonicalFace);
            if (found == cubeFaces.end()) continue;
            const auto index = static_cast<std::size_t>(std::distance(cubeFaces.begin(), found));
            orderedLayers.push_back(std::move(layers[index]));
            orderedFaces.push_back(canonicalFace);
        }
        if (orderedLayers.size() != layers.size()) {
            throw TextureError("DDS cubemap contains an unrecognized face identity");
        }
        layers = std::move(orderedLayers);
        cubeFaces = std::move(orderedFaces);
    }
    if (!cubeMap && layers.size() != 1) {
        TextureData composed = copy;
        composed.layers = {composeCanvas(copy)};
        layers = withSaveFlips(composed.layers, options);
    }
    const auto& first = layers.front();
    const bool hasAlpha = std::any_of(layers.begin(), layers.end(), layerHasAlpha);
    TextureCompression compression = options.compression;
    if (compression == TextureCompression::Auto) {
        if (copy.preferredCompression == TextureCompression::Dxt1 ||
            copy.preferredCompression == TextureCompression::Dxt3 ||
            copy.preferredCompression == TextureCompression::Dxt5) {
            compression = copy.preferredCompression;
        } else {
            compression = hasAlpha ? TextureCompression::Dxt5 : TextureCompression::Dxt1;
        }
    }
    if (compression == TextureCompression::Gray || compression == TextureCompression::SwizzledBgra) {
        compression = TextureCompression::None;
    }
    auto chains = makeMipChainPerLayer(layers, options.generateMipmaps, options.bicubicMipmaps, false);
    const std::size_t mipCount = chains.front().size();
    for (const auto& chain : chains) {
        if (chain.size() != mipCount) throw TextureError("DDS cubemap faces have different mipmap counts");
        for (std::size_t mip = 0; mip < mipCount; ++mip) {
            if (chain[mip].width != chains.front()[mip].width || chain[mip].height != chains.front()[mip].height) {
                throw TextureError("DDS cubemap mipmap dimensions do not match between faces");
            }
        }
    }

    std::vector<std::vector<std::vector<std::uint8_t>>> encoded(chains.size());
    for (std::size_t face = 0; face < chains.size(); ++face) {
        encoded[face].reserve(mipCount);
        for (const auto& mip : chains[face]) encoded[face].push_back(encodeDdsMip(mip, compression, options));
    }

    std::vector<std::uint8_t> out(128, 0);
    out[0] = 'D'; out[1] = 'D'; out[2] = 'S'; out[3] = ' ';
    writeLE32(out, 4, 124);
    std::uint32_t flags = 0x00001007u; // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    if (compression == TextureCompression::Dxt1 || compression == TextureCompression::Dxt3 ||
        compression == TextureCompression::Dxt5) flags |= 0x00080000u; // LINEARSIZE
    else flags |= 0x00000008u; // PITCH
    if (mipCount > 1) flags |= 0x00020000u;
    writeLE32(out, 8, flags);
    writeLE32(out, 12, first.height);
    writeLE32(out, 16, first.width);
    writeLE32(out, 20, static_cast<std::uint32_t>((compression == TextureCompression::None) ? first.width * 4 : encoded.front().front().size()));
    writeLE32(out, 24, 0);
    writeLE32(out, 28, static_cast<std::uint32_t>(mipCount));
    writeLE32(out, 76, 32);
    if (compression == TextureCompression::Dxt1 || compression == TextureCompression::Dxt3 ||
        compression == TextureCompression::Dxt5) {
        writeLE32(out, 80, 0x4); // FOURCC
        const char* four = compression == TextureCompression::Dxt1 ? "DXT1" :
                           compression == TextureCompression::Dxt3 ? "DXT3" : "DXT5";
        std::copy(four, four + 4, out.begin() + 84);
    } else {
        writeLE32(out, 80, 0x41); // RGB | ALPHAPIXELS
        writeLE32(out, 88, 32);
        writeLE32(out, 92, 0x00FF0000);
        writeLE32(out, 96, 0x0000FF00);
        writeLE32(out, 100, 0x000000FF);
        writeLE32(out, 104, 0xFF000000);
    }
    std::uint32_t caps = 0x1000; // TEXTURE
    if (mipCount > 1) caps |= 0x400008; // MIPMAP | COMPLEX
    if (cubeMap) caps |= 0x8; // COMPLEX
    writeLE32(out, 108, caps);
    if (cubeMap) {
        std::uint32_t caps2 = 0x00000200u;
        for (const auto face : cubeFaces) caps2 |= ddsCubeFaceBit(face);
        writeLE32(out, 112, caps2);
    }

    for (const auto& face : encoded) {
        for (const auto& mip : face) out.insert(out.end(), mip.begin(), mip.end());
    }
    writeFileBytes(output, out);
}

static TextureData readTxiTexture(const std::filesystem::path& path) {
    const auto bytes = readFileBytes(path);
    TextureData texture;
    texture.kind = TextureFileKind::Txi;
    texture.sourcePath = path;
    texture.txi = sanitizeTxiPayload(std::string(bytes.begin(), bytes.end()));
    texture.sourceEncoding = "TXI text";
    texture.notes = "TXI-only document; open the matching TGA/TPC/TXB/DDS to preview pixels.";
    return texture;
}

TextureData loadTexture(const std::filesystem::path& path) {
    const std::string ext = extensionLower(path);
    if (ext == "txi") return readTxiTexture(path);
    const auto bytes = readFileBytes(path);
    if (looksLikeDdsHeader(bytes)) return readDdsTextureBytesInternal(bytes, path, readTextSidecar(path));
    if (looksLikePngHeader(bytes)) return readPngTextureBytesInternal(bytes, path, readTextSidecar(path));
    if (looksLikeJpegHeader(bytes)) return readJpegTextureBytesInternal(bytes, path, readTextSidecar(path));
    if (looksLikeBmpHeader(bytes)) return readBmpTextureBytesInternal(bytes, path, readTextSidecar(path));
    if (looksLikeTxbHeader(bytes)) return readTxbTextureBytesInternal(bytes, path);
    if (looksLikeTpcHeader(bytes)) return readTpcTextureBytesInternal(bytes, path);
    if (looksLikeTgaHeader(bytes)) return readTgaTextureBytesInternal(bytes, path, readTextSidecar(path));
    throw TextureError("Unsupported or unrecognized texture content" + (ext.empty() ? std::string() : " in ." + ext + " file"));
}

TextureData loadTextureBytes(const std::vector<std::uint8_t>& bytes,
                             const std::filesystem::path& virtualPath,
                             std::string sidecarTxi) {
    const std::string ext = extensionLower(virtualPath);
    if (ext == "txi") {
        TextureData texture;
        texture.kind = TextureFileKind::Txi;
        texture.sourcePath = virtualPath;
        texture.txi = sanitizeTxiPayload(std::string(bytes.begin(), bytes.end()));
        texture.sourceEncoding = "TXI text";
        texture.notes = "TXI-only document; open the matching TGA/TPC/TXB/DDS to preview pixels.";
        return texture;
    }
    if (looksLikeDdsHeader(bytes)) return readDdsTextureBytesInternal(bytes, virtualPath, std::move(sidecarTxi));
    if (looksLikePngHeader(bytes)) return readPngTextureBytesInternal(bytes, virtualPath, std::move(sidecarTxi));
    if (looksLikeJpegHeader(bytes)) return readJpegTextureBytesInternal(bytes, virtualPath, std::move(sidecarTxi));
    if (looksLikeBmpHeader(bytes)) return readBmpTextureBytesInternal(bytes, virtualPath, std::move(sidecarTxi));
    if (looksLikeTxbHeader(bytes)) return readTxbTextureBytesInternal(bytes, virtualPath);
    if (looksLikeTpcHeader(bytes)) return readTpcTextureBytesInternal(bytes, virtualPath);
    if (looksLikeTgaHeader(bytes)) return readTgaTextureBytesInternal(bytes, virtualPath, std::move(sidecarTxi));
    throw TextureError("Unsupported or unrecognized in-memory texture content");
}

std::string imageCodecSupportReport() {
    std::ostringstream out;
    out << "Texture image codec support:\n";
    out << "  TPC: built-in read/write for raw, grayscale, Xbox-swizzled BGRA, DXT1/BC1, and DXT5/BC3 textures; mipmaps and cubemap faces are preserved\n";
    out << "  TXB: built-in read-only conversion support for Xbox swizzled BGRA/grayscale and DXT1/DXT5 textures\n";
    out << "  DDS: built-in read/write for pitched BGRA/BGR, A1R5G5B5, R5G6B5, ARGB4444, DXT1/DXT3/DXT5, mipmaps, and full or partial cubemaps\n";
    out << "  TGA: built-in read/write\n";
    out << "  BMP: built-in read/write for 1/4/8-bit paletted and 16/24/32-bit truecolor BMP\n";
    out << "  PNG: provided by the external libspng dependency\n";
    out << "  JPEG/JPG: provided through the libjpeg API (vcpkg uses libjpeg-turbo); alpha is dropped on JPEG output\n";
    out << "  TXI: built-in read/write, sidecar handling, and TPC embedding\n";
    return out.str();
}

void saveTexture(const TextureData& texture, const std::filesystem::path& output, const TextureSaveOptions& options) {
    const std::string ext = extensionLower(output);
    const bool sidecar = textureKindUsesTxiSidecar(ext);
    if (ext == "tga") {
        commitTextureAndSidecar(texture, output, sidecar, [&](const auto& staged) { writeTgaTexture(texture, staged); });
        return;
    }
    if (ext == "png") {
        commitTextureAndSidecar(texture, output, sidecar, [&](const auto& staged) { writePngTexture(texture, staged); });
        return;
    }
    if (ext == "jpg" || ext == "jpeg" || ext == "jpe") {
        commitTextureAndSidecar(texture, output, sidecar, [&](const auto& staged) { writeJpegTexture(texture, staged, options); });
        return;
    }
    if (ext == "bmp") {
        commitTextureAndSidecar(texture, output, sidecar, [&](const auto& staged) { writeBmpTexture(texture, staged); });
        return;
    }
    if (ext == "tpc") {
        commitTextureAndSidecar(texture, output, false, [&](const auto& staged) { writeTpcTexture(texture, staged, options); });
        return;
    }
    if (ext == "dds") {
        commitTextureAndSidecar(texture, output, sidecar, [&](const auto& staged) { writeDdsTexture(texture, staged, options); });
        return;
    }
    if (ext == "txb") {
        throw TextureError("TXB is supported as an input/conversion format only; choose TPC, DDS, TGA, PNG, JPEG, or BMP output");
    }
    if (ext == "txi") {
        commitTextureAndSidecar(texture, output, false, [&](const auto& staged) {
            writeFileBytes(staged, std::vector<std::uint8_t>(texture.txi.begin(), texture.txi.end()));
        });
        return;
    }
    throw TextureError("Unsupported texture output extension: " + ext + " (expected .tga, .png, .jpg, .bmp, .tpc, .dds, or .txi)");
}

std::string textureSummary(const TextureData& texture) {
    std::ostringstream out;
    out << "Texture resource: " << pathToUtf8(texture.sourcePath.filename()) << '\n'
        << "container: " << textureFileKindToString(texture.kind) << '\n'
        << "source encoding: " << texture.sourceEncoding << '\n';
    if (texture.hasPixels()) {
        const auto& first = texture.layers.front();
        out << "canvas: " << texture.canvasWidth << 'x' << texture.canvasHeight << '\n'
            << "layer size: " << first.width << 'x' << first.height << '\n'
            << "layers: " << texture.layers.size() << '\n'
            << "mipmaps in source/header: " << texture.sourceMipMapCount << '\n'
            << "decoded mip levels per first layer: " << (first.mipmaps.size() + 1) << '\n'
            << "alpha channel: " << (texture.hasAlpha ? "yes" : "no") << '\n'
            << "compressed: " << (texture.compressed ? "yes" : "no") << '\n'
            << "cube map: " << (texture.cubeMap ? "yes" : "no") << '\n'
            << "animated: " << (texture.animated ? "yes" : "no") << '\n'
            << "alpha blending: " << texture.alphaBlending << '\n';
    } else {
        out << "pixels: none\n";
    }
    if (texture.cubeMap && !texture.cubeFaces.empty()) {
        out << "cube faces:";
        for (const auto face : texture.cubeFaces) out << ' ' << cubeFaceToString(face);
        out << '\n';
    }
    const TxiFeatures features = parseTxiFeatures(texture.txi);
    out << "embedded/sidecar TXI bytes: " << texture.txi.size() << '\n';
    if (!texture.txi.empty()) {
        out << "TXI: cube=" << (features.cube ? "1" : "0")
            << " proceduretype=" << features.procedureType
            << " numx=" << features.numX
            << " numy=" << features.numY
            << " fps=" << features.fps << '\n';
        std::size_t txiErrors = 0;
        std::size_t txiWarnings = 0;
        for (const auto& issue : validateTxiText(texture.txi)) {
            if (issue.severity == TxiIssueSeverity::Error) ++txiErrors;
            else if (issue.severity == TxiIssueSeverity::Warning) ++txiWarnings;
        }
        out << "TXI validation: " << txiErrors << " error(s), " << txiWarnings << " warning(s)\n";
    }
    if (!texture.notes.empty()) out << "notes: " << texture.notes << '\n';
    return out.str();
}

std::string textureMetadataText(const TextureData& texture, const TextureSaveOptions& options) {
    std::ostringstream out;
    out << "# Editable texture save options. Width/height/layers below are informational.\n"
        << "container=" << textureFileKindToString(texture.kind) << '\n';
    if (texture.hasPixels()) {
        out << "width=" << texture.layers.front().width << '\n'
            << "height=" << texture.layers.front().height << '\n'
            << "layers=" << texture.layers.size() << '\n'
            << "hasAlpha=" << (texture.hasAlpha ? "true" : "false") << '\n';
    }
    out << "compression=" << textureCompressionToString(options.compression) << '\n'
        << "dxtQuality=" << dxtCompressionQualityToString(options.dxtQuality) << '\n'
        << "dxtMetric=" << dxtErrorMetricToString(options.dxtMetric) << '\n'
        << "weightColorByAlpha=" << (options.weightColorByAlpha ? "true" : "false") << '\n'
        << "dxt1AlphaThreshold=" << static_cast<unsigned>(options.dxt1AlphaThreshold) << '\n'
        << "generateMipmaps=" << (options.generateMipmaps ? "true" : "false") << '\n'
        << "bicubicMipmaps=" << (options.bicubicMipmaps ? "true" : "false") << '\n'
        << "flipXOnSave=" << (options.flipXOnSave ? "true" : "false") << '\n'
        << "flipYOnSave=" << (options.flipYOnSave ? "true" : "false") << '\n'
        << "alphaBlending=" << options.alphaBlending << '\n'
        << "jpegQuality=" << static_cast<unsigned>(options.jpegQuality) << '\n';
    return out.str();
}

void setTextureAlpha(TextureData& texture, std::uint8_t alpha) {
    for (auto& layer : texture.layers) {
        for (std::size_t i = 3; i < layer.rgba.size(); i += 4) layer.rgba[i] = alpha;
        for (auto& mip : layer.mipmaps) {
            for (std::size_t i = 3; i < mip.rgba.size(); i += 4) mip.rgba[i] = alpha;
        }
    }
    refreshHasAlpha(texture);
}

void scaleTextureAlpha(TextureData& texture, double scale) {
    if (!std::isfinite(scale) || scale < 0.0) {
        throw TextureError("Alpha scale must be a finite, non-negative number");
    }
    for (auto& layer : texture.layers) {
        for (std::size_t i = 3; i < layer.rgba.size(); i += 4) layer.rgba[i] = clampByte(layer.rgba[i] * scale);
        for (auto& mip : layer.mipmaps) {
            for (std::size_t i = 3; i < mip.rgba.size(); i += 4) mip.rgba[i] = clampByte(mip.rgba[i] * scale);
        }
    }
    refreshHasAlpha(texture);
}

void invertTextureAlpha(TextureData& texture) {
    for (auto& layer : texture.layers) {
        for (std::size_t i = 3; i < layer.rgba.size(); i += 4) layer.rgba[i] = static_cast<std::uint8_t>(255 - layer.rgba[i]);
        for (auto& mip : layer.mipmaps) {
            for (std::size_t i = 3; i < mip.rgba.size(); i += 4) mip.rgba[i] = static_cast<std::uint8_t>(255 - mip.rgba[i]);
        }
    }
    refreshHasAlpha(texture);
}

void flipTextureHorizontal(TextureData& texture) {
    for (auto& layer : texture.layers) layer = flipLayerHorizontalCopy(std::move(layer));
}

void flipTextureVertical(TextureData& texture) {
    for (auto& layer : texture.layers) layer = flipLayerVerticalCopy(std::move(layer));
}

} // namespace neotpc::texture
