#include "texture/Image.hpp"
#include "texture/Operation.hpp"
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

void orderCubeFaces(std::vector<TextureLayer>& layers, const std::vector<CubeFace>& faces, bool complete);
void denormalizeGameCubeStrip(std::vector<TextureLayer>& faces);

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

    if (texture.cubeMap) {
        TextureData strip = texture;
        orderCubeFaces(strip.layers, strip.cubeFaces, true);
        denormalizeGameCubeStrip(strip.layers);
        strip.cubeMap = false;
        return composeCanvas(strip);
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

void normalizeGameCubeStrip(std::vector<TextureLayer>& layers);

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
            normalizeGameCubeStrip(texture.layers);
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
        if (requiredWidth != canvas.width || requiredHeight != canvas.height) {
            throw TextureError("TXI animation grid must cover the source image exactly; pixels will not be silently cropped");
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
        checkOperation();
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
        checkOperation();
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

// Internal cubemaps use +X,-X,+Y,-Y,+Z,-Z with top-left pixels.
void orderCubeFaces(std::vector<TextureLayer>& layers, const std::vector<CubeFace>& faces,
                    bool complete) {
    if (complete && layers.size() != 6) throw TextureError("Game cubemap output requires all six faces");
    if (faces.empty()) {
        if (layers.size() != 6) throw TextureError("Partial cubemap is missing face identities");
        return;
    }
    if (faces.size() != layers.size()) throw TextureError("Cubemap face identities do not match layers");
    std::set<CubeFace> unique(faces.begin(), faces.end());
    if (unique.size() != faces.size()) throw TextureError("Cubemap contains duplicate face identities");
    std::vector<TextureLayer> ordered;
    for (auto face : kAllCubeFaces) {
        const auto it = std::find(faces.begin(), faces.end(), face);
        if (it != faces.end()) ordered.push_back(std::move(layers[static_cast<std::size_t>(it-faces.begin())]));
    }
    if (ordered.size() != layers.size()) throw TextureError("Cubemap has an unknown face identity");
    layers = std::move(ordered);
}

// A game raw strip is indexed from its BOTTOM, not its top-left image view.
// For stored strip i the engine selects target[i] and rotates the raw pixels.
// Conjugating that rotation by the row flip reverses it in top-left space.
constexpr std::array<std::size_t, 6> kStripTargets{{1, 0, 2, 3, 4, 5}};
constexpr std::array<unsigned, 6> kStripRotations{{3, 1, 0, 2, 2, 0}};
void normalizeGameCubeStrip(std::vector<TextureLayer>& topToBottom) {
    if (topToBottom.size() != 6) throw TextureError("Cube strip requires six square faces");
    std::vector<TextureLayer> faces(6);
    for (std::size_t i = 0; i < 6; ++i) {
        faces[kStripTargets[i]] = rotateLayer90Copy(std::move(topToBottom[5-i]),
                                                   (4-kStripRotations[i]) % 4);
    }
    topToBottom = std::move(faces);
}
void denormalizeGameCubeStrip(std::vector<TextureLayer>& faces) {
    if (faces.size() != 6) throw TextureError("Cube strip requires six square faces");
    std::vector<TextureLayer> strip(6);
    for (std::size_t i = 0; i < 6; ++i) {
        strip[5-i] = rotateLayer90Copy(std::move(faces[kStripTargets[i]]), kStripRotations[i]);
    }
    faces = std::move(strip);
}

std::vector<TextureLayer> withSaveFlips(const std::vector<TextureLayer>& layers, const TextureSaveOptions& options) {
    std::vector<TextureLayer> out = layers;
    for (auto& layer : out) {
        if (options.flipXOnSave) layer = flipLayerHorizontalCopy(std::move(layer));
        if (options.flipYOnSave) layer = flipLayerVerticalCopy(std::move(layer));
    }
    return out;
}

double srgbToLinear(double value) {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}
double linearToSrgb(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}
double cubicWeight(double value) {
    value = std::abs(value);
    if (value < 1.0) return 1.5 * value * value * value - 2.5 * value * value + 1.0;
    if (value < 2.0) return -0.5 * value * value * value + 2.5 * value * value - 4.0 * value + 2.0;
    return 0.0;
}

TextureLayer downsampleLayer(const TextureLayer& source, const TextureSaveOptions& options) {
    const std::uint32_t dstW = std::max<std::uint32_t>(1, source.width / 2);
    const std::uint32_t dstH = std::max<std::uint32_t>(1, source.height / 2);
    TextureLayer out = makeLayer(dstW, dstH);
    const double scaleX = static_cast<double>(source.width) / dstW;
    const double scaleY = static_cast<double>(source.height) / dstH;
    const bool opacity = options.mipmapAlpha == MipmapAlpha::Transparency;
    const bool srgb = options.mipmapColor == MipmapColor::Srgb;
    for (std::uint32_t y = 0; y < dstH; ++y) {
        checkOperation();
        for (std::uint32_t x = 0; x < dstW; ++x) {
            const double left = x * scaleX, top = y * scaleY;
            const double right = (x + 1) * scaleX, bottom = (y + 1) * scaleY;
            const double cx = (left + right) * 0.5, cy = (top + bottom) * 0.5;
            const int x0 = static_cast<int>(std::floor(options.bicubicMipmaps ? cx - 2 * scaleX : left));
            const int y0 = static_cast<int>(std::floor(options.bicubicMipmaps ? cy - 2 * scaleY : top));
            const int x1 = static_cast<int>(std::ceil(options.bicubicMipmaps ? cx + 2 * scaleX : right));
            const int y1 = static_cast<int>(std::ceil(options.bicubicMipmaps ? cy + 2 * scaleY : bottom));
            double sum[4] = {}, weight = 0.0;
            for (int sy = y0; sy < y1; ++sy) {
                const double wy = options.bicubicMipmaps ? cubicWeight((sy + 0.5 - cy) / scaleY)
                    : std::max(0.0, std::min(bottom, sy + 1.0) - std::max(top, static_cast<double>(sy)));
                for (int sx = x0; sx < x1; ++sx) {
                    const double wx = options.bicubicMipmaps ? cubicWeight((sx + 0.5 - cx) / scaleX)
                        : std::max(0.0, std::min(right, sx + 1.0) - std::max(left, static_cast<double>(sx)));
                    const double w = wx * wy;
                    if (std::abs(w) < 1e-15) continue;
                    const auto ix = std::clamp(sx, 0, static_cast<int>(source.width) - 1);
                    const auto iy = std::clamp(sy, 0, static_cast<int>(source.height) - 1);
                    const auto offset = (static_cast<std::size_t>(iy) * source.width + static_cast<unsigned>(ix)) * 4;
                    const double alpha = source.rgba[offset + 3] / 255.0;
                    for (int c = 0; c < 3; ++c) {
                        double color = source.rgba[offset + c] / 255.0;
                        if (srgb) color = srgbToLinear(color);
                        sum[c] += color * w * (opacity ? alpha : 1.0);
                    }
                    sum[3] += alpha * w;
                    weight += w;
                }
            }
            const auto dst = (static_cast<std::size_t>(y) * dstW + x) * 4;
            const double alpha = std::clamp(sum[3] / weight, 0.0, 1.0);
            for (int c = 0; c < 3; ++c) {
                double color = opacity ? (sum[3] > 1e-12 ? sum[c] / sum[3] : 0.0) : sum[c] / weight;
                if (srgb) color = linearToSrgb(color);
                out.rgba[dst + c] = clampByte(std::clamp(color, 0.0, 1.0) * 255.0);
            }
            out.rgba[dst + 3] = clampByte(alpha * 255.0);
        }
    }
    return out;
}

std::vector<TextureLayer> generateMipmaps(TextureLayer layer, const TextureSaveOptions& options) {
    std::vector<TextureLayer> mipmaps;
    layer.mipmaps.clear();
    mipmaps.push_back(std::move(layer));
    while (mipmaps.back().width > 1 || mipmaps.back().height > 1) {
        mipmaps.push_back(downsampleLayer(mipmaps.back(), options));
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

TextureLayer decodeDxt1(const std::uint8_t* data, std::size_t size, std::uint32_t width, std::uint32_t height, bool rgbOnly = false) {
    TextureLayer layer = makeLayer(width, height);
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    std::size_t offset = 0;
    for (std::uint32_t by = 0; by < blocksY; ++by) {
        checkOperation();
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            if (offset + 8 > size) throw TextureError("DXT1 payload is truncated");
            decodeDxtColorBlock(data + offset, layer.rgba.data(), width, height, bx * 4, by * 4, false);
            offset += 8;
        }
    }
    if (rgbOnly) for (std::size_t i = 3; i < layer.rgba.size(); i += 4) layer.rgba[i] = 255;
    return layer;
}

TextureLayer decodeDxt3(const std::uint8_t* data, std::size_t size, std::uint32_t width, std::uint32_t height) {
    TextureLayer layer = makeLayer(width, height);
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    std::size_t offset = 0;
    for (std::uint32_t by = 0; by < blocksY; ++by) {
        checkOperation();
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
        checkOperation();
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
    if (const auto sidecar = findTxiSidecar(texturePath)) {
        const auto bytes = readFileBytes(*sidecar);
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
                             ImageWriter&& imageWriter,
                             bool keepEmptySidecar = false) {
    if (output.empty()) throw TextureError("Texture output path is empty");
    const auto parent = output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) throw TextureError("Unable to create texture output directory: " + ec.message());

    auto sidecar = output;
    if (writeSidecar) {
        const auto existing = findTxiSidecar(output);
        if (existing) sidecar = *existing;
        else sidecar.replace_extension(".txi");
    }
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
        const bool stageSidecar = keepEmptySidecar || !texture.txi.empty();
        replacements.push_back({sidecar, stageSidecar ? stagedSidecar : std::filesystem::path{}, sidecarBackup});
    }

    try {
        imageWriter(stagedImage);
        if (writeSidecar && (keepEmptySidecar || !texture.txi.empty())) {
            writeFileBytes(stagedSidecar,
                           std::vector<std::uint8_t>(texture.txi.begin(), texture.txi.end()));
        }

        // Cancellation is allowed before commit, never midway through the pair.
        checkOperation();
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
    txi = sanitizeTxiPayload(std::move(txi));
    txi = trim(txi);
    if (txi.empty()) return {};
    std::string out;
    out.reserve(txi.size() + 2);
    for (std::size_t index = 0; index < txi.size(); ++index) {
        const char ch = txi[index];
        if (ch == '\r') {
            if (index + 1 < txi.size() && txi[index + 1] == '\n') ++index;
            out += "\r\n";
        } else if (ch == '\n') {
            out += "\r\n";
        } else {
            out.push_back(ch);
        }
    }
    if (out.size() < 2 || out.substr(out.size() - 2) != "\r\n") out += "\r\n";
    return out;
}

// Legacy NeoTPC rectangular animation reader fallback only. New game exports
// require square frames and complete chains; the legacy packed rectangular
// layout disagrees with the supplied games' width-derived frame stride.
std::uint32_t animatedTpcMipCount(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return 0;
    std::uint32_t count = 0;
    while (width > 0 && height > 0) {
        ++count;
        width /= 2;
        height /= 2;
    }
    return count;
}

std::uint32_t fullMipCount(std::uint32_t width, std::uint32_t height) {
    if (!width || !height) return 0;
    std::uint32_t count = 1;
    while (width > 1 || height > 1) {
        width = std::max<std::uint32_t>(1, width / 2);
        height = std::max<std::uint32_t>(1, height / 2);
        ++count;
    }
    return count;
}

// Conflicting or malformed layout directives cannot define a safe byte layout.
// Unchanged documents bypass encoding and retain even legacy metadata verbatim.
void validateGameLayoutDirectives(const std::string& txi) {
    static const std::set<std::string> keys = {
        "cube", "mipmap", "proceduretype", "numx", "numy",
        "defaultwidth", "defaultheight", "fps"
    };
    std::set<std::string> seen;
    for (const auto& entry : parseTxiEntries(txi)) {
        if (entry.blankOrComment || entry.listData || entry.listTerminator || !keys.count(entry.key)) continue;
        if (!seen.insert(entry.key).second) {
            throw TextureError("Duplicate TXI layout directive: " + entry.key + ". Keep one explicit value before encoding game output");
        }
    }
    for (const auto& issue : validateTxiText(txi)) {
        if (issue.severity == TxiIssueSeverity::Error && keys.count(issue.key)) {
            throw TextureError("Invalid TXI layout directive: " + issue.message);
        }
    }
}

// The effective TXI flag and bytes must agree. Base-only encoding supplies an
// explicit mipmap 0 if the caller did not specify a policy. Conflicts are errors.
void prepareMipPolicy(TextureData& texture, bool storeMipmaps, bool cube) {
    validateGameLayoutDirectives(texture.txi);
    const auto features = parseTxiFeatures(texture.txi);
    if (cube && features.mipmapSpecified && !features.mipmap) {
        throw TextureError("Processed game cubemaps require mipmap 1; mipmap 0 makes the game reuse face zero");
    }
    if (!storeMipmaps) {
        if (features.mipmapSpecified && features.mipmap) {
            throw TextureError("Base-only output conflicts with TXI mipmap 1. Enable mip generation or set mipmap 0");
        }
        if (!features.mipmapSpecified) texture.txi = setTxiValue(texture.txi, "mipmap", "0");
    }
}

std::vector<std::vector<TextureLayer>> makeMipChainPerLayer(const std::vector<TextureLayer>& layers,
                                                            bool generate,
                                                            const TextureSaveOptions& options,
                                                            bool tpcFileOrientation) {
    std::vector<std::vector<TextureLayer>> chains;
    chains.reserve(layers.size());
    for (TextureLayer layer : layers) {
        validateLayer(layer);
        std::vector<TextureLayer> chain;
        if (!generate) {
            layer.mipmaps.clear();
            chain.push_back(std::move(layer));
        } else if (options.mipmapPolicy == MipmapPolicy::Preserve && !layer.mipmaps.empty()) {
            auto stored = std::move(layer.mipmaps);
            layer.mipmaps.clear();
            chain.push_back(std::move(layer));
            for (auto& mip : stored) {
                mip.mipmaps.clear();
                chain.push_back(std::move(mip));
            }
        } else {
            chain = generateMipmaps(std::move(layer), options);
        }
        if (tpcFileOrientation) {
            for (auto& mip : chain) mip = flipLayerVerticalCopy(std::move(mip));
        }
        chains.push_back(std::move(chain));
    }
    return chains;
}

} // namespace

bool storesMipmaps(const TextureSaveOptions& options) noexcept {
    return options.generateMipmaps && options.mipmapPolicy != MipmapPolicy::BaseOnly;
}
TextureFileKind kindForExtension(const std::filesystem::path& path) {
    const auto ext = extensionLower(path);
    if (ext == "tpc") return TextureFileKind::Tpc;
    if (ext == "txb") return TextureFileKind::Txb;
    if (ext == "tga") return TextureFileKind::Tga;
    if (ext == "dds") return TextureFileKind::Dds;
    if (ext == "png") return TextureFileKind::Png;
    if (ext == "bmp") return TextureFileKind::Bmp;
    if (ext == "jpg" || ext == "jpeg" || ext == "jpe") return TextureFileKind::Jpeg;
    if (ext == "txi") return TextureFileKind::Txi;
    return TextureFileKind::Unknown;
}
bool sameEncodingOptions(const TextureSaveOptions& a, const TextureSaveOptions& b,
                         TextureFileKind kind, const TextureData& texture) {
    if (kind == TextureFileKind::Txi) return true;
    if (a.flipXOnSave != b.flipXOnSave || a.flipYOnSave != b.flipYOnSave) return false;
    if (kind == TextureFileKind::Jpeg) return a.jpegQuality == b.jpegQuality;
    if (kind != TextureFileKind::Tpc && kind != TextureFileKind::Dds && kind != TextureFileKind::Txb) return true;
    const auto dialect = [&](const TextureSaveOptions& o) {
        return o.ddsDialect != DdsDialect::Auto ? o.ddsDialect
            : (texture.ddsDialect != DdsDialect::Auto ? texture.ddsDialect : DdsDialect::Game);
    };
    if (kind == TextureFileKind::Dds && dialect(a) != dialect(b)) return false;
    const bool game = kind != TextureFileKind::Dds || dialect(a) == DdsDialect::Game;
    if (game) {
        const float av = a.alphaBlending.value_or(texture.alphaBlending);
        const float bv = b.alphaBlending.value_or(texture.alphaBlending);
        if (std::memcmp(&av, &bv, sizeof(float)) != 0) return false;
    }
    if (a.compression != b.compression || storesMipmaps(a) != storesMipmaps(b)) return false;
    if (storesMipmaps(a)) {
        if (a.mipmapPolicy != b.mipmapPolicy) return false;
        const bool filtering = a.mipmapPolicy == MipmapPolicy::Rebuild || texture.sourceMipMapCount <= 1;
        if (filtering && (a.bicubicMipmaps != b.bicubicMipmaps || a.mipmapAlpha != b.mipmapAlpha || a.mipmapColor != b.mipmapColor)) return false;
    }
    const auto compression = a.compression == TextureCompression::Auto
        ? (texture.hasAlpha ? TextureCompression::Dxt5 : TextureCompression::Dxt1) : a.compression;
    const bool dxt = compression == TextureCompression::Dxt1 || compression == TextureCompression::Dxt3 || compression == TextureCompression::Dxt5;
    if (dxt && (a.dxtQuality != b.dxtQuality || a.dxtMetric != b.dxtMetric || a.weightColorByAlpha != b.weightColorByAlpha)) return false;
    if (!game && compression == TextureCompression::Dxt1 && a.dxt1AlphaThreshold != b.dxt1AlphaThreshold) return false;
    return true;
}

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
        } else if (key == "mipmap") {
            features.mipmapSpecified = true;
            if (auto v = parseU32Loose(rest)) features.mipmap = *v != 0;
            else features.mipmap = !parseFalsy(rest);
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
        throw TextureError("Unknown TXI key: " + trimmedKey + ". Edit the TXI source view directly to preserve custom directives.");
    }
    if (directive->valueKind == TxiDirectiveValueKind::FloatList ||
        directive->valueKind == TxiDirectiveValueKind::Vector3List) {
        throw TextureError(directive->name +
            " begins a multi-line list; edit the TXI source view so its rows stay together");
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

namespace {

struct TpcContainerLayout {
    std::uint32_t dataSize = 0;
    float alphaBlending = 1.0f;
    std::uint32_t headerWidth = 0;
    std::uint32_t headerHeight = 0;
    std::uint8_t encoding = 0;
    std::uint8_t mipMapCount = 1;
    bool uncompressed = true;
    bool cubeMap = false;
    std::uint32_t layerCount = 1;
    std::uint32_t layerWidth = 0;
    std::uint32_t layerHeight = 0;
    TextureCompression compression = TextureCompression::None;
    std::string encodingName;
    std::size_t txiOffset = 128;
};

std::size_t tpcMipPayloadSize(const TpcContainerLayout& layout,
                              std::uint32_t width,
                              std::uint32_t height) {
    if (!layout.uncompressed && layout.compression == TextureCompression::Dxt1) {
        return dxt1Size(width, height);
    }
    if (!layout.uncompressed && layout.compression == TextureCompression::Dxt5) {
        return dxt5Size(width, height);
    }
    if (layout.encoding == kTpcEncodingGray) {
        return static_cast<std::size_t>(width) * height;
    }
    if (layout.encoding == kTpcEncodingRgb) {
        return static_cast<std::size_t>(width) * height * 3u;
    }
    return static_cast<std::size_t>(width) * height * 4u;
}

std::uint8_t inferAnimatedTpcMipCount(const TpcContainerLayout& layout,
                                      std::uint32_t layerWidth,
                                      std::uint32_t layerHeight,
                                      std::uint32_t layerCount) {
    const auto canonical = animatedTpcMipCount(layerWidth, layerHeight);
    if (canonical == 0) return 1;
    if (layout.dataSize == 0 || layerCount == 0) {
        return static_cast<std::uint8_t>(std::min<std::uint32_t>(255u, canonical));
    }

    // The animated header stores the complete payload size and uses a sentinel
    // mip count of 1. Infer the physical chain length from that payload so files
    // previously written by NeoTPC with an ordinary full rectangular chain can
    // still be opened for inspection/conversion. This does NOT certify the
    // legacy packed layout for game playback.
    std::uint32_t fullCount = 1;
    for (std::uint32_t w = layerWidth, h = layerHeight; w > 1 || h > 1; ) {
        w = std::max<std::uint32_t>(1, w / 2);
        h = std::max<std::uint32_t>(1, h / 2);
        ++fullCount;
    }

    std::uint64_t oneLayerBytes = 0;
    std::uint32_t w = layerWidth;
    std::uint32_t h = layerHeight;
    for (std::uint32_t count = 1; count <= fullCount; ++count) {
        std::uint64_t next = 0;
        if (!parser::checkedAdd(oneLayerBytes, tpcMipPayloadSize(layout, w, h), next)) {
            throw TextureError("TPC animated mipmap payload size overflows");
        }
        oneLayerBytes = next;

        std::uint64_t totalBytes = 0;
        if (!parser::checkedMultiply(oneLayerBytes, layerCount, totalBytes)) {
            throw TextureError("TPC animated mipmap payload size overflows");
        }
        if (totalBytes == layout.dataSize) {
            return static_cast<std::uint8_t>(std::min<std::uint32_t>(255u, count));
        }
        if (totalBytes > layout.dataSize) break;

        w = std::max<std::uint32_t>(1, w / 2);
        h = std::max<std::uint32_t>(1, h / 2);
    }

    return static_cast<std::uint8_t>(std::min<std::uint32_t>(255u, canonical));
}

TpcContainerLayout inspectTpcContainer(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 128 || !looksLikeTpcHeader(bytes)) {
        throw TextureError("Invalid or unsupported TPC header");
    }

    TpcContainerLayout layout;
    layout.dataSize = readLE32(bytes, 0);
    layout.alphaBlending = readLEFloat(bytes, 4);
    layout.headerWidth = readLE16(bytes, 8);
    layout.headerHeight = readLE16(bytes, 10);
    layout.encoding = bytes[12];
    layout.mipMapCount = bytes[13] == 0 ? 1 : bytes[13];
    layout.uncompressed = layout.dataSize == 0;
    layout.layerWidth = layout.headerWidth;
    layout.layerHeight = layout.headerHeight;

    const std::size_t candidateCubeBaseSize = layout.encoding == kTpcEncodingRgb
        ? dxt1Size(layout.headerWidth, layout.headerWidth)
        : layout.encoding == kTpcEncodingRgba
            ? dxt5Size(layout.headerWidth, layout.headerWidth)
            : 0;
    if (!layout.uncompressed && layout.headerWidth != 0 &&
        layout.headerHeight / layout.headerWidth == 6 &&
        layout.headerHeight % layout.headerWidth == 0 &&
        candidateCubeBaseSize == layout.dataSize) {
        layout.cubeMap = true;
        layout.layerCount = 6;
        layout.layerHeight = layout.headerHeight / 6;
    }

    if (layout.uncompressed) {
        if (layout.encoding == kTpcEncodingGray) {
            layout.compression = TextureCompression::Gray;
            layout.encodingName = "raw grayscale";
        } else if (layout.encoding == kTpcEncodingSwizzledBgra) {
            layout.compression = TextureCompression::SwizzledBgra;
            layout.encodingName = "Xbox swizzled BGRA";
        } else {
            layout.compression = TextureCompression::None;
            layout.encodingName = layout.encoding == kTpcEncodingRgba ? "raw RGBA" : "raw RGB";
        }
    } else if (layout.encoding == kTpcEncodingRgb) {
        layout.compression = TextureCompression::Dxt1;
        layout.encodingName = "DXT1";
    } else if (layout.encoding == kTpcEncodingRgba) {
        layout.compression = TextureCompression::Dxt5;
        layout.encodingName = "DXT5";
    } else if (layout.encoding == kTpcEncodingGray) {
        layout.compression = TextureCompression::Gray;
        layout.encodingName = "grayscale payload";
    } else {
        throw TextureError("Unknown compressed TPC encoding: " + std::to_string(layout.encoding));
    }

    std::uint64_t payloadSize = 0;
    auto addPayload = [&](std::uint64_t amount) {
        std::uint64_t next = 0;
        if (!parser::checkedAdd(payloadSize, amount, next)) {
            throw TextureError("TPC payload size overflows");
        }
        payloadSize = next;
    };

    if (!layout.uncompressed) {
        if (!layout.cubeMap) {
            addPayload(layout.dataSize);
            std::uint32_t width = layout.layerWidth;
            std::uint32_t height = layout.layerHeight;
            for (std::uint8_t mip = 1; mip < layout.mipMapCount; ++mip) {
                width = std::max<std::uint32_t>(1, width / 2);
                height = std::max<std::uint32_t>(1, height / 2);
                addPayload(tpcMipPayloadSize(layout, width, height));
            }
        } else {
            std::uint64_t oneLayer = layout.dataSize;
            std::uint32_t width = layout.layerWidth;
            std::uint32_t height = layout.layerHeight;
            for (std::uint8_t mip = 1; mip < layout.mipMapCount; ++mip) {
                width = std::max<std::uint32_t>(1, width / 2);
                height = std::max<std::uint32_t>(1, height / 2);
                std::uint64_t next = 0;
                if (!parser::checkedAdd(oneLayer, tpcMipPayloadSize(layout, width, height), next)) {
                    throw TextureError("TPC cubemap payload size overflows");
                }
                oneLayer = next;
            }
            if (!parser::checkedMultiply(oneLayer, layout.layerCount, payloadSize)) {
                throw TextureError("TPC cubemap payload size overflows");
            }
        }
    } else {
        std::uint64_t oneLayer = 0;
        std::uint32_t width = layout.layerWidth;
        std::uint32_t height = layout.layerHeight;
        for (std::uint8_t mip = 0; mip < layout.mipMapCount; ++mip) {
            std::uint64_t next = 0;
            if (!parser::checkedAdd(oneLayer, tpcMipPayloadSize(layout, width, height), next)) {
                throw TextureError("TPC raw payload size overflows");
            }
            oneLayer = next;
            width = std::max<std::uint32_t>(1, width / 2);
            height = std::max<std::uint32_t>(1, height / 2);
        }
        if (!parser::checkedMultiply(oneLayer, layout.layerCount, payloadSize)) {
            throw TextureError("TPC raw payload size overflows");
        }
    }

    std::uint64_t txiOffset = 0;
    if (!parser::checkedAdd(UINT64_C(128), payloadSize, txiOffset)) {
        throw TextureError("TPC payload offset overflows");
    }
    if (txiOffset > bytes.size()) {
        throw TextureError("TPC payload is truncated: declared mipmaps extend beyond the file");
    }
    layout.txiOffset = static_cast<std::size_t>(txiOffset);
    return layout;
}

// Contract recovered from the supplied K1 i386 / K2 x86_64 resource loaders
// and processed upload routines. This is deliberately stricter about bounds.
std::vector<std::string> tpcGameLayoutIssues(const TpcContainerLayout& layout,
                                           const TxiFeatures& features,
                                           bool animated, bool cube,
                                           std::uint32_t width, std::uint32_t height,
                                           std::uint32_t layers, std::uint32_t storedMips,
                                           std::size_t payloadBytes) {
    std::vector<std::string> issues;
    if (layout.encoding == kTpcEncodingSwizzledBgra) {
        issues.push_back("Xbox swizzled pixels are not decoded by the supplied desktop game loaders; convert the encoding for desktop use.");
    }
    if (!std::isfinite(layout.alphaBlending)) issues.push_back("The TPC header float is not finite.");
    if (features.cube != cube) issues.push_back("TXI cube flag disagrees with the processed face layout.");
    if (cube && animated) issues.push_back("Combined cube animation is not supported by this exporter.");
    if (cube && !features.mipmap) issues.push_back("Processed cubemap with mipmap 0 repeats face zero in the game.");
    if (animated) {
        if (width != height) issues.push_back("Rectangular animation frames do not match the game's square, width-derived frame stride. Use square frames; the atlas itself may be rectangular.");
        if (features.numX == 0 || features.numY == 0 || features.fps <= 0 ||
            static_cast<std::uint64_t>(features.numX) * features.numY != layers ||
            static_cast<std::uint64_t>(width) * features.numX != layout.headerWidth ||
            static_cast<std::uint64_t>(height) * features.numY != layout.headerHeight) {
            issues.push_back("Animation grid/frame dimensions do not exactly match the stored canvas.");
        }
        if ((features.defaultWidth && features.defaultWidth != width) ||
            (features.defaultHeight && features.defaultHeight != height)) {
            issues.push_back("Animation defaultwidth/defaultheight disagree with the numx/numy frame dimensions.");
        }
    } else if (features.numX > 1 || features.numY > 1 || iequals(features.procedureType, "cycle")) {
        issues.push_back("TXI requests animation/grid subdivision without matching stored animation frames.");
    }
    const bool gameCubeHeuristic = !layout.uncompressed && layout.headerWidth &&
                                   layout.headerHeight / layout.headerWidth == 6;
    if (gameCubeHeuristic && !cube) issues.push_back("The header height/width quotient is 6: the game treats this payload as six faces before reading TXI. Use a different atlas grid.");
    if (cube && (width != height || layers != 6)) issues.push_back("A game cubemap requires six square faces.");

    // Original footer arithmetic shifts BOTH dimensions towards zero, unlike
    // the upload loop which keeps an exhausted axis at one.
    const std::uint32_t faces = gameCubeHeuristic ? 6 : 1;
    std::uint32_t w = layout.headerWidth;
    std::uint32_t h = layout.headerHeight / faces;
    std::uint64_t gameBytes = layout.uncompressed ? 0 : static_cast<std::uint64_t>(layout.dataSize) * faces;
    const std::uint32_t bpp = (layout.encoding & 1) ? 1 : ((layout.encoding & 2) ? 3 : 4);
    for (std::uint32_t mip = 0; mip < layout.mipMapCount; ++mip) {
        if (layout.uncompressed) gameBytes += static_cast<std::uint64_t>(w) * h * bpp;
        else if (mip) gameBytes += static_cast<std::uint64_t>((w + 3) / 4) * ((h + 3) / 4) * (bpp == 3 ? 8 : 16) * faces;
        w /= 2; h /= 2;
    }
    if (gameBytes != payloadBytes) {
        issues.push_back("Game TXI boundary differs from the encoded payload (game " + std::to_string(gameBytes) +
                         " bytes, stored " + std::to_string(payloadBytes) +
                         "). Rectangular static TPCs must use base-only storage with mipmap 0, or another format.");
    }
    const auto requiredMips = fullMipCount(width, height);
    if ((features.mipmap || animated || cube) && storedMips != requiredMips) {
        issues.push_back("The game requests a complete mip chain but the stored chain has " + std::to_string(storedMips) +
                         " of " + std::to_string(requiredMips) + " levels.");
    }
    std::uint64_t storedStride = 0;
    w = width; h = height;
    for (std::uint32_t mip = 0; mip < storedMips; ++mip) {
        storedStride += tpcMipPayloadSize(layout, w, h);
        w = std::max<std::uint32_t>(1, w / 2); h = std::max<std::uint32_t>(1, h / 2);
    }
    if (storedStride * layers != payloadBytes) issues.push_back("Stored mip/face spans do not account for the entire image payload.");
    if (!animated && !layout.uncompressed && layout.dataSize != tpcMipPayloadSize(layout, width, height)) {
        issues.push_back("Compressed static/cube header size is not the size of its base level.");
    }
    if (animated) {
        std::uint64_t gameStride = 0;
        w = width;
        do {
            gameStride += tpcMipPayloadSize(layout, w, w);
            w /= 2;
        } while (w);
        if (gameStride != storedStride) issues.push_back("The game's animation frame addresses do not match the stored frame addresses.");
    }
    return issues;
}

void requireCompatible(const std::vector<std::string>& issues) {
    if (issues.empty()) return;
    std::string message = "Cannot encode this layout for the supplied desktop game loaders:";
    for (const auto& issue : issues) message += "\n- " + issue;
    throw TextureError(message);
}

} // namespace

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
    if ((bytes[17] & 0x30) != 0) {
        texture.compatibilityWarnings.push_back("This TGA uses an origin flag ignored by the supplied game loaders. Explicit conversion writes bottom-origin rows; an unchanged Save preserves the source.");
    }
    refreshHasAlpha(texture);
    applyTxiLayout(texture, true);
    return texture;
}

static std::vector<std::uint8_t> encodeTgaTexture(const TextureData& texture) {
    if (!texture.hasPixels()) throw TextureError("Cannot write TGA without pixel data");
    const TextureLayer canvas = composeCanvas(texture);
    validateLayer(canvas);
    if (canvas.width > 0xFFFF || canvas.height > 0xFFFF) throw TextureError("TGA dimensions exceed 16-bit header limits");
    std::vector<std::uint8_t> out(18, 0);
    out[2] = 2; // uncompressed true color
    writeLE16(out, 12, static_cast<std::uint16_t>(canvas.width));
    writeLE16(out, 14, static_cast<std::uint16_t>(canvas.height));
    out[16] = 32;
    out[17] = 0x08; // 8 alpha bits; bottom origin, as consumed by the game
    out.reserve(18 + canvas.rgba.size());
    for (std::uint32_t y = 0; y < canvas.height; ++y) {
        for (std::uint32_t x = 0; x < canvas.width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(canvas.height - 1 - y) * canvas.width + x) * 4;
            out.push_back(canvas.rgba[src + 2]);
            out.push_back(canvas.rgba[src + 1]);
            out.push_back(canvas.rgba[src + 0]);
            out.push_back(canvas.rgba[src + 3]);
        }
    }
    return out;
}

static std::vector<std::uint8_t> encodePngTexture(const TextureData& texture) {
    if (!texture.hasPixels()) throw TextureError("Cannot write PNG without pixel data");
    const TextureLayer canvas = composeCanvas(texture);
    validateLayer(canvas);
    return internal_image::encodePngRgba(canvas.width, canvas.height, canvas.rgba);
}

static std::vector<std::uint8_t> encodeJpegTexture(const TextureData& texture, const TextureSaveOptions& options) {
    if (!texture.hasPixels()) throw TextureError("Cannot write JPEG without pixel data");
    const TextureLayer canvas = composeCanvas(texture);
    validateLayer(canvas);
    return internal_image::encodeJpegRgb(canvas.width,
                                                         canvas.height,
                                                         canvas.rgba,
                                                         static_cast<std::uint8_t>(std::max<int>(1, std::min<int>(100, options.jpegQuality))));
}

static std::vector<std::uint8_t> encodeBmpTexture(const TextureData& texture) {
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
    return out;
}

TextureData readTpcTextureBytesInternal(const std::vector<std::uint8_t>& bytes,
                                        const std::filesystem::path& path) {
    const TpcContainerLayout layout = inspectTpcContainer(bytes);
    const std::uint32_t headerDataSize = layout.dataSize;
    const float alphaBlending = layout.alphaBlending;
    const std::uint32_t headerWidth = layout.headerWidth;
    const std::uint32_t headerHeight = layout.headerHeight;
    const std::uint8_t encoding = layout.encoding;
    std::uint8_t mipMapCount = layout.mipMapCount;
    const bool uncompressed = layout.uncompressed;
    std::uint32_t layerCount = layout.layerCount;
    std::uint32_t layerWidth = layout.layerWidth;
    std::uint32_t layerHeight = layout.layerHeight;
    const bool cubeMap = layout.cubeMap;
    const TextureCompression compression = layout.compression;

    auto mipSize = [&](std::uint32_t w, std::uint32_t h) -> std::size_t {
        return tpcMipPayloadSize(layout, w, h);
    };

    const std::size_t txiOffset = layout.txiOffset;
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
            mipMapCount = inferAnimatedTpcMipCount(layout, layerWidth, layerHeight, layerCount);
        }
    }

    if (mipMapCount > fullMipCount(layerWidth, layerHeight)) {
        throw TextureError("TPC declares mip levels beyond 1x1");
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
    texture.sourceEncoding = layout.encodingName;

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
                throw TextureError("TPC payload is truncated before a declared mipmap");
            }
            TextureLayer decoded;
            if (!uncompressed && compression == TextureCompression::Dxt1) {
                decoded = flipLayerVerticalCopy(decodeDxt1(bytes.data() + offset, size, w, h, true));
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
        // Processed TPC stores the renderer face order directly.
        texture.cubeFaces = allCubeFaces();
    }
    if (offset != payloadLimit) throw TextureError("TPC declared payload does not match its decoded layer/mip spans");
    texture.compatibilityWarnings = tpcGameLayoutIssues(layout, features, animated, cubeMap,
                                                       layerWidth, layerHeight, layerCount, mipMapCount,
                                                       payloadLimit - 128);
    try { validateGameLayoutDirectives(texture.txi); }
    catch (const TextureError& e) { texture.compatibilityWarnings.push_back(e.what()); }
    if (bytes[13] == 0) texture.compatibilityWarnings.push_back("Zero mip-count header has nonportable game footer semantics.");
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

static std::vector<std::uint8_t> encodeTpcTexture(const TextureData& input, const TextureSaveOptions& options) {
    if (!input.hasPixels()) throw TextureError("Cannot write TPC without pixel data");
    TextureData texture = input;
    validateGameLayoutDirectives(texture.txi);
    applyTxiLayout(texture);
    refreshHasAlpha(texture);
    std::vector<TextureLayer> layers = withSaveFlips(texture.layers, options);
    if (layers.empty()) throw TextureError("No texture layers to write");
    const bool hasAlpha = std::any_of(layers.begin(), layers.end(), layerHasAlpha);
    TextureCompression compression = chooseAutoCompression(texture, options);
    if (compression == TextureCompression::Dxt1 && hasAlpha) {
        if (options.compression == TextureCompression::Auto) compression = TextureCompression::Dxt5;
        else throw TextureError("Game TPC DXT1 is RGB-only, not punch-through alpha. Use DXT5, or explicitly make alpha opaque before encoding.");
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
    if (compression == TextureCompression::SwizzledBgra) {
        throw TextureError("Xbox swizzled TPC output is not desktop-compatible. Choose raw, DXT1 or DXT5; Xbox inputs remain readable and can be saved unchanged.");
    }
    if (cube) orderCubeFaces(layers, texture.cubeFaces, true);

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
        if (first.width != first.height) {
            throw TextureError("Rectangular animation frames are not supported for game TPC output. Use square frames (a rectangular atlas is allowed), or export an image atlas instead.");
        }
        const auto requiredMipCount = fullMipCount(first.width, first.height);
        if (requiredMipCount == 0) throw TextureError("TPC animation frames have invalid dimensions");
        for (auto& layer : layers) {
            if (layer.mipmaps.size() + 1 != requiredMipCount) layer.mipmaps.clear();
        }
    }

    // Animated TPC stores a sentinel mip count in the header; readers derive a
    // game-specific chain from each frame's dimensions, so animation output
    // must carry that chain even when ordinary mip generation is off.
    const bool generate = (storesMipmaps(options) || animated) && compression != TextureCompression::Gray;
    if (cube && !generate) throw TextureError("Game TPC cubemaps require complete mip chains; enable Generate mipmaps");
    if (!animated && !cube && first.width != first.height && generate) {
        throw TextureError("Rectangular static TPC mip chains disagree with the game TXI boundary. Disable mip generation (mipmap 0), or use TGA/game DDS instead");
    }
    prepareMipPolicy(texture, generate, cube);
    if (cube && !features.cube) texture.txi = setTxiValue(texture.txi, "cube", "1");
    if (!std::isfinite(options.alphaBlending.value_or(input.alphaBlending))) throw TextureError("TPC header float must be finite");
    auto mipChains = makeMipChainPerLayer(layers, generate, options, true);
    if (animated) {
        const auto requiredMipCount = static_cast<std::size_t>(fullMipCount(first.width, first.height));
        for (auto& chain : mipChains) {
            if (chain.size() < requiredMipCount) {
                throw TextureError("TPC animation mipmap chain is incomplete");
            }
            chain.resize(requiredMipCount);
        }
    }
    for (const auto& chain : mipChains) {
        if (chain.size() != mipChains.front().size() || chain.size() > fullMipCount(first.width, first.height)) {
            throw TextureError("TPC faces/frames must have the same mip count, without levels beyond 1x1");
        }
    }
    const std::uint8_t mipMapCount = static_cast<std::uint8_t>(std::min<std::size_t>(255, mipChains.front().size()));

    auto encodeMip = [&](const TextureLayer& layer) -> std::vector<std::uint8_t> {
        auto nativeOptions = options;
        if (compression == TextureCompression::Dxt1) nativeOptions.dxt1AlphaThreshold = 0;
        return encodeLayerRaw(layer, compression, hasAlpha, nativeOptions);
    };

    std::uint32_t headerWidth = first.width;
    std::uint32_t headerHeight = first.height;
    if (cube) {
        headerHeight = checkedDimensionProduct(first.width, 6, "TPC cube height");
    } else if (animated && features.numX > 0 && features.numY > 0) {
        headerWidth = checkedDimensionProduct(first.width, features.numX, "TPC animation width");
        headerHeight = checkedDimensionProduct(first.height, features.numY, "TPC animation height");
    } else if (texture.canvasWidth > 0 && texture.canvasHeight > 0 && layers.size() == 1) {
        if (texture.canvasWidth != first.width || texture.canvasHeight != first.height) {
            throw TextureError("Static TPC canvas dimensions disagree with its pixel layer");
        }
        headerWidth = texture.canvasWidth;
        headerHeight = texture.canvasHeight;
    }
    if (headerWidth >= 0x8000 || headerHeight >= 0x8000) throw TextureError("TPC canvas dimensions exceed the supported positive Odyssey range");

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
    writeLEFloat(out, 4, options.alphaBlending.value_or(input.alphaBlending));
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
    const auto payloadBytes = out.size() - 128;
    requireCompatible(tpcGameLayoutIssues(inspectTpcContainer(out), parseTxiFeatures(texture.txi),
                                          animated, cube, first.width, first.height,
                                          static_cast<std::uint32_t>(layers.size()), mipMapCount, payloadBytes));
    const std::string txi = normalizeTxiFooter(texture.txi);
    out.insert(out.end(), txi.begin(), txi.end());
    return out;
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
        texture.ddsDialect = DdsDialect::Standard;
        texture.notes += "Standard DDS is an interchange format; use explicit Game DDS conversion for these game resource readers. ";
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
        texture.ddsDialect = DdsDialect::Game;
        texture.alphaBlending = readLEFloat(bytes, 16);
        if (!std::isfinite(texture.alphaBlending)) throw TextureError("BioWare DDS header float is not finite");
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
            if (!parser::rangeWithin(bytes.size(), offset, size)) {
                if (offset != bytes.size()) throw TextureError("BioWare DDS ends partway through a mipmap");
                break;
            }
            std::uint64_t nextDecodedBytes = 0;
            if (!parser::checkedAdd(decodedBytes,
                                    checkedRgbaByteCount(mipWidth, mipHeight, "BioWare DDS mipmap"),
                                    nextDecodedBytes) || nextDecodedBytes > parser::maxDecodedBytes()) {
                throw TextureError("BioWare DDS mipmaps exceed the decoded-image memory limit");
            }
            decodedBytes = nextDecodedBytes;
            auto decoded = flipLayerVerticalCopy(
                decodeDdsMip(bytes.data() + offset, size, mipWidth, mipHeight, storage));
            if (storage == DdsStorage::Dxt1) {
                for (std::size_t i = 3; i < decoded.rgba.size(); i += 4) decoded.rgba[i] = 255;
            }
            if (mipCount == 0) layer = std::move(decoded);
            else layer.mipmaps.push_back(std::move(decoded));
            ++mipCount;
            offset += size;
            if (mipWidth == 1 && mipHeight == 1) break;
            mipWidth = std::max<std::uint32_t>(1, mipWidth / 2);
            mipHeight = std::max<std::uint32_t>(1, mipHeight / 2);
        }
        if (mipCount == 0) throw TextureError("BioWare DDS contains no complete mipmap");
        if (offset != bytes.size()) throw TextureError("BioWare DDS has unexplained trailing payload");
        const auto baseSize = ddsMipSize(storage, width, height);
        if (dataSize != baseSize && dataSize != bytes.size() - 20) {
            texture.compatibilityWarnings.push_back("BioWare DDS size field does not match its base or total payload.");
        }
        if (parseTxiFeatures(texture.txi).mipmap && mipCount != fullMipCount(width, height)) {
            texture.compatibilityWarnings.push_back("BioWare DDS has an incomplete mip chain while mipmapping is enabled.");
        }
        texture.canvasWidth = width;
        texture.canvasHeight = height;
        texture.layers.push_back(std::move(layer));
        texture.sourceMipMapCount = mipCount;
        texture.preferredCompression = bpp == 3 ? TextureCompression::Dxt1 : TextureCompression::Dxt5;
        texture.compressed = true;
        texture.sourceEncoding = bpp == 3 ? "BioWare DDS DXT1" : "BioWare DDS DXT5";
        try { validateGameLayoutDirectives(texture.txi); }
        catch (const TextureError& e) { texture.compatibilityWarnings.push_back(e.what()); }
        const auto flags = parseTxiFeatures(texture.txi);
        if (flags.cube || flags.numX > 1 || flags.numY > 1 || iequals(flags.procedureType, "cycle")) {
            texture.compatibilityWarnings.push_back("This game DDS requests cube/animation layout; only a single static game DDS surface is supported for encoding.");
        }
    }
    refreshHasAlpha(texture);
    applyTxiLayout(texture);
    return texture;
}

static std::vector<std::uint8_t> encodeGameDdsTexture(const TextureData& input, const TextureSaveOptions& options) {
    if (!input.hasPixels()) throw TextureError("Cannot write DDS without pixels");
    TextureData texture = input;
    const auto features = parseTxiFeatures(texture.txi);
    if (texture.cubeMap || texture.animated || texture.layers.size() != 1 || features.cube ||
        features.numX > 1 || features.numY > 1 || iequals(features.procedureType, "cycle")) {
        throw TextureError("Game DDS export currently supports a single static surface. Use TPC for a supported cube/animation, or explicitly select standard DDS for interchange");
    }
    auto layers = withSaveFlips(texture.layers, options);
    auto compression = options.compression;
    if (compression == TextureCompression::Auto) {
        compression = layerHasAlpha(layers.front()) ? TextureCompression::Dxt5 : TextureCompression::Dxt1;
    }
    if (compression != TextureCompression::Dxt1 && compression != TextureCompression::Dxt5) {
        throw TextureError("Game DDS supports DXT1 or DXT5. Select one of those, or standard DDS for other encodings");
    }
    if (compression == TextureCompression::Dxt1 && layerHasAlpha(layers.front())) {
        throw TextureError("Game DDS DXT1 is RGB-only, not punch-through alpha. Use DXT5, or explicitly make alpha opaque before encoding.");
    }
    prepareMipPolicy(texture, storesMipmaps(options), false);
    const auto chains = makeMipChainPerLayer(layers, storesMipmaps(options), options, true);
    const auto& chain = chains.front();
    const auto& base = chain.front();
    if (base.width >= 0x8000 || base.height >= 0x8000) throw TextureError("Game DDS dimensions exceed the supported positive Odyssey range");
    if (chain.size() > fullMipCount(base.width, base.height)) throw TextureError("Game DDS contains mip levels beyond 1x1");
    if (parseTxiFeatures(texture.txi).mipmap && chain.size() != fullMipCount(base.width, base.height)) {
        throw TextureError("Game DDS mipmapping requires a complete chain to 1x1");
    }
    const float alpha = options.alphaBlending.value_or(texture.alphaBlending);
    if (!std::isfinite(alpha)) throw TextureError("Game DDS header float must be finite");
    std::vector<std::uint8_t> out(20, 0);
    writeLE32(out, 0, base.width);
    writeLE32(out, 4, base.height);
    writeLE32(out, 8, compression == TextureCompression::Dxt1 ? 3 : 4);
    writeLE32(out, 12, static_cast<std::uint32_t>(bytesForEncoding(compression, base.width, base.height, true)));
    writeLEFloat(out, 16, alpha);
    for (const auto& mip : chain) {
        auto nativeOptions = options;
        if (compression == TextureCompression::Dxt1) nativeOptions.dxt1AlphaThreshold = 0;
        const auto bytes = encodeLayerRaw(mip, compression, true, nativeOptions);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

DdsDialect effectiveDdsDialect(const TextureData& texture, const TextureSaveOptions& options) {
    if (options.ddsDialect != DdsDialect::Auto) return options.ddsDialect;
    if (texture.kind == TextureFileKind::Dds && texture.ddsDialect != DdsDialect::Auto) return texture.ddsDialect;
    return DdsDialect::Game;
}

static std::vector<std::uint8_t> encodeDdsTexture(const TextureData& texture, const TextureSaveOptions& options) {
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
    auto chains = makeMipChainPerLayer(layers, storesMipmaps(options), options, false);
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
    return out;
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

EncodedTexture encodeTexture(const TextureData& input, const std::filesystem::path& output,
                              const TextureSaveOptions& options) {
    checkOperation();
    const std::string ext = extensionLower(output);
    const bool flat = ext == "tga" || ext == "png" || ext == "bmp" || ext == "jpg" || ext == "jpeg" || ext == "jpe";
    TextureData texture = input;
    if (flat && texture.cubeMap && !parseTxiFeatures(texture.txi).cube)
        texture.txi = setTxiValue(texture.txi, "cube", "1");
    if (flat && (options.flipXOnSave || options.flipYOnSave)) texture.layers = withSaveFlips(texture.layers, options);
    auto applied = options;
    if (flat) { applied.flipXOnSave = false; applied.flipYOnSave = false; }
    if (ext == "dds" && effectiveDdsDialect(texture, applied) == DdsDialect::Game)
        prepareMipPolicy(texture, storesMipmaps(applied), false);
    EncodedTexture result;
    if (textureKindUsesTxiSidecar(ext) && !texture.txi.empty())
        result.sidecar.emplace(texture.txi.begin(), texture.txi.end());
    if (ext == "tga") result.image = encodeTgaTexture(texture);
    else if (ext == "png") result.image = encodePngTexture(texture);
    else if (ext == "jpg" || ext == "jpeg" || ext == "jpe") result.image = encodeJpegTexture(texture, applied);
    else if (ext == "bmp") result.image = encodeBmpTexture(texture);
    else if (ext == "tpc") result.image = encodeTpcTexture(texture, applied);
    else if (ext == "dds") result.image = effectiveDdsDialect(texture, applied) == DdsDialect::Game
        ? encodeGameDdsTexture(texture, applied) : encodeDdsTexture(texture, applied);
    else if (ext == "txi") result.image.assign(texture.txi.begin(), texture.txi.end());
    else throw TextureError("Unsupported output extension: " + ext + ". Choose TPC, DDS, TGA, PNG, JPEG, BMP or TXI.");
    checkOperation();
    return result;
}

void saveTexture(const TextureData& texture, const std::filesystem::path& output, const TextureSaveOptions& options) {
    const auto encoded = encodeTexture(texture, output, options);
    saveEncodedTexture(encoded.image, encoded.sidecar, output);
}

void saveEncodedTexture(const std::vector<std::uint8_t>& imageBytes,
                        const std::optional<std::vector<std::uint8_t>>& sidecarBytes,
                        const std::filesystem::path& output) {
    TextureData metadata;
    if (sidecarBytes) metadata.txi.assign(sidecarBytes->begin(), sidecarBytes->end());
    commitTextureAndSidecar(metadata, output, textureKindUsesTxiSidecar(extensionLower(output)),
        [&](const auto& staged) { writeFileBytes(staged, imageBytes); }, sidecarBytes.has_value());
}

void replaceTpcEmbeddedTxi(const std::filesystem::path& tpcPath,
                           const std::string& txi) {
    if (tpcPath.empty()) throw TextureError("TPC path is empty");
    saveTpcWithEmbeddedTxi(readFileBytes(tpcPath), txi, tpcPath);
}

std::vector<std::uint8_t> patchTpcTxiBytes(const std::vector<std::uint8_t>& original, const std::string& txi) {
    const std::filesystem::path tpcPath("preview.tpc");
    validateGameLayoutDirectives(txi);
    const auto layout = inspectTpcContainer(original);
    const auto originalTexture = readTpcTextureBytesInternal(original, tpcPath);
    const std::string normalizedTxi = normalizeTxiFooter(txi);

    std::vector<std::uint8_t> replacement;
    replacement.reserve(layout.txiOffset + normalizedTxi.size());
    replacement.insert(replacement.end(), original.begin(),
                       original.begin() + static_cast<std::ptrdiff_t>(layout.txiOffset));
    replacement.insert(replacement.end(), normalizedTxi.begin(), normalizedTxi.end());

    // Validate the complete replacement before touching the original file.
    // This catches TXI animation layouts that are incompatible with the
    // existing encoded payload.
    const auto replacementTexture = readTpcTextureBytesInternal(replacement, tpcPath);
    requireCompatible(replacementTexture.compatibilityWarnings);
    if (replacementTexture.cubeMap != originalTexture.cubeMap ||
        replacementTexture.animated != originalTexture.animated ||
        replacementTexture.layers.size() != originalTexture.layers.size()) {
        throw TextureError("The edited TXI changes the TPC image layout; split and recombine the texture instead");
    }
    for (std::size_t layer = 0; layer < originalTexture.layers.size(); ++layer) {
        const auto& before = originalTexture.layers[layer];
        const auto& after = replacementTexture.layers[layer];
        if (before.width != after.width || before.height != after.height ||
            before.mipmaps.size() != after.mipmaps.size()) {
            throw TextureError("The edited TXI changes the TPC image layout; split and recombine the texture instead");
        }
        for (std::size_t mip = 0; mip < before.mipmaps.size(); ++mip) {
            if (before.mipmaps[mip].width != after.mipmaps[mip].width ||
                before.mipmaps[mip].height != after.mipmaps[mip].height) {
                throw TextureError("The edited TXI changes the TPC image layout; split and recombine the texture instead");
            }
        }
    }

    return replacement;
}

void saveTpcWithEmbeddedTxi(const std::vector<std::uint8_t>& original,
                           const std::string& txi, const std::filesystem::path& tpcPath) {
    const auto bytes = patchTpcTxiBytes(original, txi);
    saveEncodedTexture(bytes, {}, tpcPath);
}

TgaTxiPairPaths saveTgaTxiPair(const TextureData& texture,
                               const std::filesystem::path& outputTga) {
    if (extensionLower(outputTga) != "tga") {
        throw TextureError("TGA/TXI split output must use the .tga extension");
    }
    if (!texture.hasPixels()) {
        throw TextureError("Cannot create a TGA/TXI pair without pixel data");
    }

    auto converted = texture;
    if (converted.cubeMap && !parseTxiFeatures(converted.txi).cube) {
        converted.txi = setTxiValue(converted.txi, "cube", "1");
    }
    commitTextureAndSidecar(converted, outputTga, true,
                            [&](const auto& staged) { writeFileBytes(staged, encodeTgaTexture(converted)); },
                            true);
    auto txiPath = outputTga;
    txiPath.replace_extension(".txi");
    if(auto existing=findTxiSidecar(outputTga))txiPath=*existing;
    return {outputTga, std::move(txiPath)};
}

TgaTxiPairPaths splitTpcToTgaTxi(const std::filesystem::path& inputTpc,
                                 const std::filesystem::path& outputTga) {
    auto texture = loadTexture(inputTpc);
    if (texture.kind != TextureFileKind::Tpc) {
        throw TextureError("Split input is not a TPC texture");
    }
    return saveTgaTxiPair(texture, outputTga);
}

void combineTgaTxiToTpc(const std::filesystem::path& inputTga,
                        const std::optional<std::filesystem::path>& inputTxi,
                        const std::filesystem::path& outputTpc,
                        const TextureSaveOptions& options) {
    if (extensionLower(outputTpc) != "tpc") {
        throw TextureError("Combined texture output must use the .tpc extension");
    }

    TextureData texture;
    if (inputTxi) {
        const auto imageBytes = readFileBytes(inputTga);
        const auto txiBytes = readFileBytes(*inputTxi);
        texture = loadTextureBytes(imageBytes, inputTga,
            sanitizeTxiPayload(std::string(txiBytes.begin(), txiBytes.end())));
    } else {
        texture = loadTexture(inputTga);
    }
    if (texture.kind != TextureFileKind::Tga) {
        throw TextureError("Combine input is not a TGA image");
    }
    saveTexture(texture, outputTpc, options);
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
    if (!texture.compatibilityWarnings.empty()) {
        out << "\nGame compatibility warnings (readable is not game certification):\n";
        for (const auto& warning : texture.compatibilityWarnings) out << "- " << warning << '\n';
    }
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
        << "alphaBlending=" << options.alphaBlending.value_or(texture.alphaBlending) << '\n'
        << "jpegQuality=" << static_cast<unsigned>(options.jpegQuality) << '\n';
    return out.str();
}

void setTextureAlpha(TextureData& texture, std::uint8_t alpha) {
    for (auto& layer : texture.layers) {
        for (std::size_t i = 3; i < layer.rgba.size(); i += 4) { if((i & 262143u)==3)checkOperation(); layer.rgba[i] = alpha; }
        for (auto& mip : layer.mipmaps) {
            for (std::size_t i = 3; i < mip.rgba.size(); i += 4) { if((i & 262143u)==3)checkOperation(); mip.rgba[i] = alpha; }
        }
    }
    refreshHasAlpha(texture);
}

void scaleTextureAlpha(TextureData& texture, double scale) {
    if (!std::isfinite(scale) || scale < 0.0) {
        throw TextureError("Alpha scale must be a finite, non-negative number");
    }
    for (auto& layer : texture.layers) {
        for (std::size_t i = 3; i < layer.rgba.size(); i += 4) { if((i & 262143u)==3)checkOperation(); layer.rgba[i] = clampByte(layer.rgba[i] * scale); }
        for (auto& mip : layer.mipmaps) {
            for (std::size_t i = 3; i < mip.rgba.size(); i += 4) { if((i & 262143u)==3)checkOperation(); mip.rgba[i] = clampByte(mip.rgba[i] * scale); }
        }
    }
    refreshHasAlpha(texture);
}

void invertTextureAlpha(TextureData& texture) {
    for (auto& layer : texture.layers) {
        for (std::size_t i = 3; i < layer.rgba.size(); i += 4) { if((i & 262143u)==3)checkOperation(); layer.rgba[i] = static_cast<std::uint8_t>(255 - layer.rgba[i]); }
        for (auto& mip : layer.mipmaps) {
            for (std::size_t i = 3; i < mip.rgba.size(); i += 4) { if((i & 262143u)==3)checkOperation(); mip.rgba[i] = static_cast<std::uint8_t>(255 - mip.rgba[i]); }
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
