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

// Auto preserves an imported DDS dialect; new DDS exports use the game header.
enum class DdsDialect { Auto, Game, Standard };
enum class MipmapPolicy { Preserve, Rebuild, BaseOnly };
enum class MipmapAlpha { Independent, Transparency };
enum class MipmapColor { Stored, Srgb };

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
    bool mipmapSpecified = false;
    bool mipmap = true;
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
    MipmapPolicy mipmapPolicy = MipmapPolicy::Preserve;
    // Explicit authoring choices: data/mask channels are independent by default.
    MipmapAlpha mipmapAlpha = MipmapAlpha::Independent;
    MipmapColor mipmapColor = MipmapColor::Stored;
    bool flipXOnSave = false;
    bool flipYOnSave = false;
    // Unspecified preserves the input header float (not pixel opacity).
    std::optional<float> alphaBlending;
    DdsDialect ddsDialect = DdsDialect::Auto;

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
    DdsDialect ddsDialect = DdsDialect::Auto;
    // Readable does not imply game-compatible. Shown in info/GUI metadata.
    std::vector<std::string> compatibilityWarnings;
    bool compressed = false;
    bool hasAlpha = false;
    bool cubeMap = false;
    bool animated = false;
    std::uint32_t sourceMipMapCount = 1;
    std::string notes;

    bool hasPixels() const noexcept { return !layers.empty() && !layers.front().rgba.empty(); }
};

bool storesMipmaps(const TextureSaveOptions& options) noexcept;
// Compares only settings that can affect the requested output representation.
bool sameEncodingOptions(const TextureSaveOptions& a, const TextureSaveOptions& b,
                         TextureFileKind kind, const TextureData& texture);
TextureFileKind kindForExtension(const std::filesystem::path& path);

struct EncodedTexture {
    std::vector<std::uint8_t> image;
    std::optional<std::vector<std::uint8_t>> sidecar;
};
// Pure encode: no output file or temporary file is created. Suitable for preview
// and for committing exactly the already-reviewed bytes.
EncodedTexture encodeTexture(const TextureData& texture, const std::filesystem::path& output,
                             const TextureSaveOptions& options = {});
TextureData loadTexture(const std::filesystem::path& path);
TextureData loadTextureBytes(const std::vector<std::uint8_t>& bytes,
                             const std::filesystem::path& virtualPath,
                             std::string sidecarTxi = {});
void saveTexture(const TextureData& texture,
                 const std::filesystem::path& output,
                 const TextureSaveOptions& options = {});

// Transactional, exact encoded copy for an unchanged document. A present
// sidecar (including an empty one) is preserved; absence removes stale TXI.
void saveEncodedTexture(const std::vector<std::uint8_t>& imageBytes,
                        const std::optional<std::vector<std::uint8_t>>& sidecarBytes,
                        const std::filesystem::path& output);

struct TgaTxiPairPaths {
    std::filesystem::path tga;
    std::filesystem::path txi;
};

// Replaces only the embedded TXI footer of an existing TPC. The 128-byte
// header and encoded pixel/mipmap payload are preserved byte-for-byte.
void replaceTpcEmbeddedTxi(const std::filesystem::path& tpcPath,
                           const std::string& txi);

std::vector<std::uint8_t> patchTpcTxiBytes(const std::vector<std::uint8_t>& original, const std::string& txi);

void saveTpcWithEmbeddedTxi(const std::vector<std::uint8_t>& original,
                           const std::string& txi, const std::filesystem::path& output);

// Writes a lossless TGA plus a same-stem TXI sidecar. The TXI file is created
// even when the metadata is empty so an explicit split always produces a pair.
TgaTxiPairPaths saveTgaTxiPair(const TextureData& texture,
                               const std::filesystem::path& outputTga);
TgaTxiPairPaths splitTpcToTgaTxi(const std::filesystem::path& inputTpc,
                                 const std::filesystem::path& outputTga);

// Builds a TPC from a TGA and either the supplied TXI file or, when omitted,
// the same-stem sidecar discovered beside the TGA.
void combineTgaTxiToTpc(const std::filesystem::path& inputTga,
                        const std::optional<std::filesystem::path>& inputTxi,
                        const std::filesystem::path& outputTpc,
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
