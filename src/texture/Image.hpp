// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neotpc::texture {

enum class TextureFileKind {
    Unknown,
    Tga,
    Tpc,
    Txb,
    Dds,
    Png,
    Jpeg,
    Bmp,
    Txi,
};

enum class TextureCompression {
    Auto,
    None,
    Gray,
    Dxt1,
    Dxt3,
    Dxt5,
    SwizzledBgra,
};

enum class DxtCompressionQuality {
    Fast,
    Normal,
    High,
};

enum class DxtErrorMetric {
    Perceptual,
    Uniform,
};

enum class CubeFace {
    PositiveX,
    NegativeX,
    PositiveY,
    NegativeY,
    PositiveZ,
    NegativeZ,
};

std::string textureCompressionToString(TextureCompression compression);
TextureCompression textureCompressionFromString(const std::string& value);
std::string dxtCompressionQualityToString(DxtCompressionQuality quality);
DxtCompressionQuality dxtCompressionQualityFromString(const std::string& value);
std::string dxtErrorMetricToString(DxtErrorMetric metric);
DxtErrorMetric dxtErrorMetricFromString(const std::string& value);
std::string textureFileKindToString(TextureFileKind kind);
std::string cubeFaceToString(CubeFace face);

struct TextureLayer {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Always RGBA8888 in top-left display/editing orientation.
    std::vector<std::uint8_t> rgba;
    // Lower-resolution levels in order. The base level remains directly in
    // this object so existing image/editing code can use width/height/rgba.
    // Each entry here is a leaf level whose own mipmaps vector is empty.
    std::vector<TextureLayer> mipmaps;
};

struct TxiFeatures {
    bool cube = false;
    bool isBumpMap = false;
    bool compressTextureSpecified = false;
    bool compressTexture = true;
    std::string procedureType;
    std::uint32_t numX = 0;
    std::uint32_t numY = 0;
    std::uint32_t defaultWidth = 0;
    std::uint32_t defaultHeight = 0;
    double fps = 0.0;
};

struct TextureSaveOptions {
    TextureCompression compression = TextureCompression::Auto;
    bool generateMipmaps = true;
    bool bicubicMipmaps = false;
    bool flipXOnSave = false;
    bool flipYOnSave = false;
    float alphaBlending = 1.0f;

    // DXT/BC1/BC3 encoder controls. Fast is range fitting, Normal adds
    // cluster endpoint fitting, High adds local endpoint search.
    DxtCompressionQuality dxtQuality = DxtCompressionQuality::High;
    DxtErrorMetric dxtMetric = DxtErrorMetric::Perceptual;
    bool weightColorByAlpha = false;
    std::uint8_t dxt1AlphaThreshold = 128;

    // Used only when writing JPEG output. PNG/TGA/BMP/TPC/DDS ignore it.
    std::uint8_t jpegQuality = 95;
};

struct TextureData {
    TextureFileKind kind = TextureFileKind::Unknown;
    std::filesystem::path sourcePath;
    std::uint32_t canvasWidth = 0;
    std::uint32_t canvasHeight = 0;
    std::vector<TextureLayer> layers;
    // When this is a cubemap, entries identify the corresponding layers in
    // order. It may contain fewer than six entries for a partial DDS cubemap.
    std::vector<CubeFace> cubeFaces;
    std::string txi;
    float alphaBlending = 1.0f;
    TextureCompression preferredCompression = TextureCompression::Auto;
    std::string sourceEncoding;
    bool compressed = false;
    bool hasAlpha = false;
    bool cubeMap = false;
    bool animated = false;
    std::uint32_t sourceMipMapCount = 1;
    std::string notes;

    bool hasPixels() const noexcept { return !layers.empty() && !layers.front().rgba.empty(); }
};

TextureData loadTexture(const std::filesystem::path& path);
TextureData loadTextureBytes(const std::vector<std::uint8_t>& bytes,
                             const std::filesystem::path& virtualPath,
                             std::string sidecarTxi = {});
void saveTexture(const TextureData& texture,
                 const std::filesystem::path& output,
                 const TextureSaveOptions& options = {});
TxiFeatures parseTxiFeatures(const std::string& txi);
std::string setTxiValue(const std::string& txi, const std::string& key, const std::string& value);
std::optional<std::string> getTxiValue(const std::string& txi, const std::string& key);
std::string textureSummary(const TextureData& texture);
std::string textureMetadataText(const TextureData& texture, const TextureSaveOptions& options);

void setTextureAlpha(TextureData& texture, std::uint8_t alpha);
void scaleTextureAlpha(TextureData& texture, double scale);
void invertTextureAlpha(TextureData& texture);
void flipTextureHorizontal(TextureData& texture);
void flipTextureVertical(TextureData& texture);

std::string imageCodecSupportReport();

} // namespace neotpc::texture
