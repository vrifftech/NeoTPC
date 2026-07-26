#include "texture/BatchConverter.hpp"
#include "texture/FileUtil.hpp"
#include "texture/Image.hpp"
#include "texture/ParserLimits.hpp"
#include "texture/Txi.hpp"
#include "core/TextureDocument.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
    fs::path path;
    TempDirectory() {
        const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("neotpc-tests-" + std::to_string(nonce));
        fs::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

neotpc::texture::TextureData gradient(bool alpha) {
    neotpc::texture::TextureData texture;
    texture.kind = neotpc::texture::TextureFileKind::Png;
    texture.canvasWidth = 24;
    texture.canvasHeight = 20;
    texture.hasAlpha = alpha;
    neotpc::texture::TextureLayer layer;
    layer.width = texture.canvasWidth;
    layer.height = texture.canvasHeight;
    layer.rgba.resize(static_cast<std::size_t>(layer.width) * layer.height * 4);
    for (std::uint32_t y = 0; y < layer.height; ++y) {
        for (std::uint32_t x = 0; x < layer.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * layer.width + x) * 4;
            layer.rgba[offset] = static_cast<std::uint8_t>((x * 255u) / (layer.width - 1u));
            layer.rgba[offset + 1] = static_cast<std::uint8_t>((y * 255u) / (layer.height - 1u));
            layer.rgba[offset + 2] = static_cast<std::uint8_t>(((x + y) * 255u) / (layer.width + layer.height - 2u));
            layer.rgba[offset + 3] = alpha ? static_cast<std::uint8_t>(((x * 3u + y * 5u) * 255u) /
                                                                       ((layer.width - 1u) * 3u + (layer.height - 1u) * 5u)) : 255;
        }
    }
    texture.layers.push_back(std::move(layer));
    texture.txi = "blending additive\nisbumpmap 1\n";
    return texture;
}

void writeLe16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    require(offset + 2 <= bytes.size(), "test fixture uint16 write is out of range");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void writeLe32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    require(offset + 4 <= bytes.size(), "test fixture uint32 write is out of range");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t readLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require(offset + 4 <= bytes.size(), "test fixture uint32 read is out of range");
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

neotpc::texture::TextureLayer solidLayer(std::uint32_t width,
                                         std::uint32_t height,
                                         std::array<std::uint8_t, 4> color) {
    neotpc::texture::TextureLayer layer;
    layer.width = width;
    layer.height = height;
    layer.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t offset = 0; offset < layer.rgba.size(); offset += 4) {
        std::copy(color.begin(), color.end(), layer.rgba.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return layer;
}

neotpc::texture::TextureLayer facePattern(std::uint32_t width, std::uint32_t height, unsigned face) {
    neotpc::texture::TextureLayer layer;
    layer.width = width;
    layer.height = height;
    layer.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            layer.rgba[offset + 0] = static_cast<std::uint8_t>(20u + x * 25u);
            layer.rgba[offset + 1] = static_cast<std::uint8_t>(30u + y * 24u);
            layer.rgba[offset + 2] = static_cast<std::uint8_t>(20u + face * 38u);
            layer.rgba[offset + 3] = static_cast<std::uint8_t>(255u - ((x + y + face) % 5u) * 10u);
        }
    }
    return layer;
}

neotpc::texture::TextureData mipTexture(unsigned faces = 1) {
    neotpc::texture::TextureData texture;
    texture.kind = neotpc::texture::TextureFileKind::Png;
    texture.canvasWidth = 8;
    texture.cubeMap = faces == 6;
    texture.canvasHeight = texture.cubeMap ? 48 : 8;
    texture.txi = texture.cubeMap ? "cube 1\n" : "mipmap 1\n";
    for (unsigned face = 0; face < faces; ++face) {
        auto layer = facePattern(8, 8, face);
        layer.mipmaps.push_back(solidLayer(4, 4, {static_cast<std::uint8_t>(10 + face * 20), 41, 82, 213}));
        layer.mipmaps.push_back(solidLayer(2, 2, {static_cast<std::uint8_t>(11 + face * 20), 42, 83, 172}));
        layer.mipmaps.push_back(solidLayer(1, 1, {static_cast<std::uint8_t>(12 + face * 20), 43, 84, 131}));
        texture.layers.push_back(std::move(layer));
    }
    texture.hasAlpha = true;
    return texture;
}

void requireLayerExact(const neotpc::texture::TextureLayer& expected,
                       const neotpc::texture::TextureLayer& actual,
                       const std::string& context) {
    require(expected.width == actual.width && expected.height == actual.height, context + " dimensions changed");
    require(expected.rgba == actual.rgba, context + " pixels changed");
    require(expected.mipmaps.size() == actual.mipmaps.size(), context + " mipmap count changed");
    for (std::size_t mip = 0; mip < expected.mipmaps.size(); ++mip) {
        requireLayerExact(expected.mipmaps[mip], actual.mipmaps[mip], context + " mip " + std::to_string(mip + 1));
    }
}

std::uint32_t mortonOffset(std::uint32_t x,
                           std::uint32_t y,
                           std::uint32_t width,
                           std::uint32_t height) {
    std::uint32_t widthBits = 0;
    std::uint32_t heightBits = 0;
    for (auto value = width; value > 1; value >>= 1) ++widthBits;
    for (auto value = height; value > 1; value >>= 1) ++heightBits;
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

std::vector<std::uint8_t> makeDds16(std::uint32_t pixelFlags,
                                    std::uint32_t redMask,
                                    std::uint32_t greenMask,
                                    std::uint32_t blueMask,
                                    std::uint32_t alphaMask,
                                    const std::vector<std::uint16_t>& pixels) {
    std::vector<std::uint8_t> bytes(128 + pixels.size() * 2, 0);
    bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
    writeLe32(bytes, 4, 124);
    writeLe32(bytes, 8, 0x0000100Fu);
    writeLe32(bytes, 12, 1);
    writeLe32(bytes, 16, static_cast<std::uint32_t>(pixels.size()));
    writeLe32(bytes, 20, static_cast<std::uint32_t>(pixels.size() * 2));
    writeLe32(bytes, 28, 1);
    writeLe32(bytes, 76, 32);
    writeLe32(bytes, 80, pixelFlags);
    writeLe32(bytes, 88, 16);
    writeLe32(bytes, 92, redMask);
    writeLe32(bytes, 96, greenMask);
    writeLe32(bytes, 100, blueMask);
    writeLe32(bytes, 104, alphaMask);
    writeLe32(bytes, 108, 0x1000);
    for (std::size_t index = 0; index < pixels.size(); ++index) writeLe16(bytes, 128 + index * 2, pixels[index]);
    return bytes;
}

double meanSquaredError(const neotpc::texture::TextureLayer& left, const neotpc::texture::TextureLayer& right, bool alpha) {
    require(left.width == right.width && left.height == right.height, "MSE dimensions differ");
    const unsigned channels = alpha ? 4 : 3;
    double error = 0.0;
    for (std::size_t offset = 0; offset < left.rgba.size(); offset += 4) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            const double delta = static_cast<double>(left.rgba[offset + channel]) - right.rgba[offset + channel];
            error += delta * delta;
        }
    }
    return error / (static_cast<double>(left.width) * left.height * channels);
}

void requireExact(const neotpc::texture::TextureData& expected, const neotpc::texture::TextureData& actual, const std::string& format) {
    require(actual.hasPixels(), format + " did not decode pixels");
    require(expected.layers.front().width == actual.layers.front().width &&
            expected.layers.front().height == actual.layers.front().height, format + " dimensions changed");
    require(expected.layers.front().rgba == actual.layers.front().rgba, format + " RGBA pixels changed");
}

void testLosslessCodecs(const fs::path& root) {
    const auto source = gradient(true);
    for (const char* extension : {"tga", "bmp", "png"}) {
        const auto path = root / (std::string("lossless.") + extension);
        neotpc::texture::saveTexture(source, path);
        const auto decoded = neotpc::texture::loadTexture(path);
        requireExact(source, decoded, extension);
        require(decoded.txi.find("isbumpmap") != std::string::npos, std::string(extension) + " TXI sidecar was not loaded");
    }
}

void testCompressedCodecs(const fs::path& root) {
    const auto source = gradient(true);
    neotpc::texture::TextureSaveOptions options;
    options.generateMipmaps = true;
    options.compression = neotpc::texture::TextureCompression::Dxt5;
    options.dxtQuality = neotpc::texture::DxtCompressionQuality::High;

    for (const char* extension : {"dds", "tpc"}) {
        const auto path = root / (std::string("compressed.") + extension);
        neotpc::texture::saveTexture(source, path, options);
        const auto decoded = neotpc::texture::loadTexture(path);
        require(decoded.hasPixels(), std::string(extension) + " did not decode pixels");
        require(decoded.layers.front().width == source.layers.front().width &&
                decoded.layers.front().height == source.layers.front().height, std::string(extension) + " dimensions changed");
        require(meanSquaredError(source.layers.front(), decoded.layers.front(), false) < 1100.0,
                std::string(extension) + " DXT5 RGB error is too high");
        require(meanSquaredError(source.layers.front(), decoded.layers.front(), true) < 1500.0,
                std::string(extension) + " DXT5 RGBA error is too high");
        require(decoded.txi.find("blending additive") != std::string::npos, std::string(extension) + " TXI metadata was lost");
    }

    auto opaque = gradient(false);
    options.compression = neotpc::texture::TextureCompression::Dxt1;
    const auto dxt1 = root / "opaque.dds";
    neotpc::texture::saveTexture(opaque, dxt1, options);
    const auto decoded = neotpc::texture::loadTexture(dxt1);
    require(meanSquaredError(opaque.layers.front(), decoded.layers.front(), false) < 1100.0, "DDS DXT1 error is too high");
}

void testTpcTxiEditingAndPairs(const fs::path& root) {
    auto source = gradient(true);
    source.txi = "downsamplemax 0\ndownsamplemin 0\n";

    neotpc::texture::TextureSaveOptions options;
    options.compression = neotpc::texture::TextureCompression::Dxt5;
    options.generateMipmaps = true;
    options.dxtQuality = neotpc::texture::DxtCompressionQuality::High;

    const auto tpc = root / "editable-embedded.tpc";
    neotpc::texture::saveTexture(source, tpc, options);
    const auto beforeTexture = neotpc::texture::loadTexture(tpc);
    const auto beforeBytes = neotpc::texture::readFileBytes(tpc);
    require(beforeBytes.size() >= beforeTexture.txi.size(),
            "TPC TXI footer is larger than its container");
    const std::size_t payloadEnd = beforeBytes.size() - beforeTexture.txi.size();

    const std::string replacement =
        "envmaptexture CM_Baremetal\n"
        "blending additive\n";
    neotpc::texture::replaceTpcEmbeddedTxi(tpc, replacement);
    const auto replacedBytes = neotpc::texture::readFileBytes(tpc);
    require(replacedBytes.size() >= payloadEnd,
            "embedded TXI replacement truncated the TPC payload");
    require(std::equal(beforeBytes.begin(),
                       beforeBytes.begin() + static_cast<std::ptrdiff_t>(payloadEnd),
                       replacedBytes.begin()),
            "embedded TXI replacement changed encoded TPC bytes");
    const auto replacedTexture = neotpc::texture::loadTexture(tpc);
    require(neotpc::texture::getTxiValue(replacedTexture.txi, "envmaptexture") ==
                std::optional<std::string>("CM_Baremetal"),
            "embedded TXI replacement did not persist envmaptexture");
    require(neotpc::texture::getTxiValue(replacedTexture.txi, "blending") ==
                std::optional<std::string>("additive"),
            "embedded TXI replacement did not persist blending");

    const auto stableBytes = replacedBytes;
    bool rejected = false;
    try {
        neotpc::texture::replaceTpcEmbeddedTxi(
            tpc,
            "proceduretype cycle\n"
            "numx 2\n"
            "numy 2\n"
            "defaultwidth 12\n"
            "defaultheight 10\n"
            "fps 8\n");
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "incompatible animation TXI was accepted for an existing TPC payload");
    require(neotpc::texture::readFileBytes(tpc) == stableBytes,
            "failed embedded TXI replacement changed the original TPC");

    const auto split = neotpc::texture::splitTpcToTgaTxi(
        tpc, root / "editable-split.tga");
    require(fs::is_regular_file(split.tga) && fs::is_regular_file(split.txi),
            "TPC split did not create both TGA and TXI files");
    const auto splitTxiBytes = neotpc::texture::readFileBytes(split.txi);
    require(std::string(splitTxiBytes.begin(), splitTxiBytes.end()) == replacedTexture.txi,
            "TPC split changed the TXI sidecar text");
    const auto splitTexture = neotpc::texture::loadTexture(split.tga);
    require(splitTexture.layers.size() == replacedTexture.layers.size(),
            "TPC split changed the layer count");
    for (std::size_t layer = 0; layer < splitTexture.layers.size(); ++layer) {
        require(splitTexture.layers[layer].width == replacedTexture.layers[layer].width &&
                    splitTexture.layers[layer].height == replacedTexture.layers[layer].height &&
                    splitTexture.layers[layer].rgba == replacedTexture.layers[layer].rgba,
                "TPC split changed decoded base-level pixels");
    }

    const auto combined = root / "editable-combined.tpc";
    neotpc::texture::combineTgaTxiToTpc(split.tga, std::nullopt, combined, options);
    const auto combinedTexture = neotpc::texture::loadTexture(combined);
    require(combinedTexture.canvasWidth == splitTexture.canvasWidth &&
                combinedTexture.canvasHeight == splitTexture.canvasHeight,
            "TGA/TXI combine changed canvas dimensions");
    require(combinedTexture.txi == splitTexture.txi,
            "TGA/TXI combine changed TXI metadata");
    require(meanSquaredError(splitTexture.layers.front(), combinedTexture.layers.front(), true) < 1500.0,
            "TGA/TXI combine DXT5 error is too high");

    auto noMetadata = gradient(true);
    noMetadata.txi.clear();
    const auto emptyPair = neotpc::texture::saveTgaTxiPair(
        noMetadata, root / "empty-metadata.tga");
    require(fs::is_regular_file(emptyPair.txi) && fs::file_size(emptyPair.txi) == 0,
            "explicit TGA/TXI split did not create an empty TXI sidecar");

    auto animationCanvas = gradient(true);
    animationCanvas.txi.clear();
    const auto animationTga = root / "animation-canvas.tga";
    neotpc::texture::saveTexture(animationCanvas, animationTga);
    const auto animationTxi = root / "animation-layout.txi";
    const std::string animationLayout =
        "proceduretype cycle\n"
        "numx 2\n"
        "numy 1\n"
        "defaultwidth 12\n"
        "defaultheight 20\n"
        "fps 8\n";
    neotpc::texture::writeFileBytes(
        animationTxi,
        std::vector<std::uint8_t>(animationLayout.begin(), animationLayout.end()));
    const auto animationTpc = root / "animation-combined.tpc";
    neotpc::texture::combineTgaTxiToTpc(
        animationTga, animationTxi, animationTpc, options);
    const auto animation = neotpc::texture::loadTexture(animationTpc);
    require(animation.animated && animation.layers.size() == 2,
            "explicit TXI was not applied before TGA animation layout inference");
    require(animation.layers[0].width == 12 && animation.layers[0].height == 20,
            "combined animation frame dimensions are wrong");

    auto verifyFooterOnlyUpdate = [&](const std::string& name,
                                      neotpc::texture::TextureData candidate,
                                      neotpc::texture::TextureSaveOptions candidateOptions) {
        candidate.txi = neotpc::texture::setTxiValue(candidate.txi, "downsamplemax", "0");
        const auto path = root / ("footer-" + name + ".tpc");
        neotpc::texture::saveTexture(candidate, path, candidateOptions);
        const auto loadedBefore = neotpc::texture::loadTexture(path);
        const auto bytesBefore = neotpc::texture::readFileBytes(path);
        require(bytesBefore.size() >= loadedBefore.txi.size(),
                name + " TPC has an invalid TXI footer size");
        const auto prefixSize = bytesBefore.size() - loadedBefore.txi.size();
        const auto layerCount = loadedBefore.layers.size();
        std::vector<std::size_t> mipCounts;
        mipCounts.reserve(layerCount);
        for (const auto& layer : loadedBefore.layers) mipCounts.push_back(layer.mipmaps.size());

        const auto updatedTxi = neotpc::texture::setTxiValue(
            loadedBefore.txi, "downsamplemax", "1");
        neotpc::texture::replaceTpcEmbeddedTxi(path, updatedTxi);
        const auto bytesAfter = neotpc::texture::readFileBytes(path);
        require(bytesAfter.size() >= prefixSize,
                name + " TXI replacement truncated the TPC");
        require(std::equal(bytesBefore.begin(),
                           bytesBefore.begin() + static_cast<std::ptrdiff_t>(prefixSize),
                           bytesAfter.begin()),
                name + " TXI replacement changed encoded image bytes");

        const auto loadedAfter = neotpc::texture::loadTexture(path);
        require(loadedAfter.layers.size() == layerCount,
                name + " TXI replacement changed the layer count");
        for (std::size_t layer = 0; layer < layerCount; ++layer) {
            require(loadedAfter.layers[layer].mipmaps.size() == mipCounts[layer],
                    name + " TXI replacement changed a mipmap count");
        }
        require(neotpc::texture::getTxiValue(loadedAfter.txi, "downsamplemax") ==
                    std::optional<std::string>("1"),
                name + " TXI replacement did not persist the new value");
    };

    auto rawOptions = options;
    rawOptions.compression = neotpc::texture::TextureCompression::None;
    verifyFooterOnlyUpdate("raw", mipTexture(), rawOptions);

    auto grayOptions = options;
    grayOptions.compression = neotpc::texture::TextureCompression::Gray;
    grayOptions.generateMipmaps = false;
    verifyFooterOnlyUpdate("gray", mipTexture(), grayOptions);

    auto swizzledOptions = options;
    swizzledOptions.compression = neotpc::texture::TextureCompression::SwizzledBgra;
    verifyFooterOnlyUpdate("swizzled", mipTexture(), swizzledOptions);

    auto cubeOptions = options;
    cubeOptions.compression = neotpc::texture::TextureCompression::Dxt5;
    verifyFooterOnlyUpdate("cube", mipTexture(6), cubeOptions);

    neotpc::texture::TextureData animated;
    animated.kind = neotpc::texture::TextureFileKind::Tga;
    animated.canvasWidth = 16;
    animated.canvasHeight = 16;
    animated.hasAlpha = true;
    animated.animated = true;
    animated.txi =
        "proceduretype cycle\n"
        "numx 2\n"
        "numy 2\n"
        "defaultwidth 8\n"
        "defaultheight 8\n"
        "fps 8\n";
    animated.layers.push_back(solidLayer(8, 8, {255, 0, 0, 255}));
    animated.layers.push_back(solidLayer(8, 8, {0, 255, 0, 255}));
    animated.layers.push_back(solidLayer(8, 8, {0, 0, 255, 255}));
    animated.layers.push_back(solidLayer(8, 8, {255, 255, 0, 255}));
    verifyFooterOnlyUpdate("animation", std::move(animated), options);
}

void testMipmapsAndSwizzledTpc(const fs::path& root) {
    const auto source = mipTexture();
    neotpc::texture::TextureSaveOptions options;
    options.generateMipmaps = true;
    options.compression = neotpc::texture::TextureCompression::SwizzledBgra;
    const auto tpcPath = root / "swizzled-mips.tpc";
    neotpc::texture::saveTexture(source, tpcPath, options);
    const auto tpcBytes = neotpc::texture::readFileBytes(tpcPath);
    require(tpcBytes.size() > 128 && tpcBytes[12] == 0x0C && readLe32(tpcBytes, 0) == 0,
            "swizzled TPC header encoding is wrong");
    const auto tpc = neotpc::texture::loadTexture(tpcPath);
    require(tpc.preferredCompression == neotpc::texture::TextureCompression::SwizzledBgra,
            "swizzled TPC compression metadata was not preserved");
    requireLayerExact(source.layers.front(), tpc.layers.front(), "swizzled TPC");

    options.compression = neotpc::texture::TextureCompression::None;
    const auto ddsPath = root / "raw-mips.dds";
    neotpc::texture::saveTexture(source, ddsPath, options);
    const auto dds = neotpc::texture::loadTexture(ddsPath);
    requireLayerExact(source.layers.front(), dds.layers.front(), "raw DDS");

    const auto disguised = root / "raw-mips.texture-data";
    neotpc::texture::writeFileBytes(disguised, neotpc::texture::readFileBytes(ddsPath));
    const auto sniffed = neotpc::texture::loadTexture(disguised);
    require(sniffed.kind == neotpc::texture::TextureFileKind::Dds, "content detection did not recognize a misnamed DDS");
    requireLayerExact(source.layers.front(), sniffed.layers.front(), "misnamed raw DDS");

    auto alphaEdited = source;
    neotpc::texture::setTextureAlpha(alphaEdited, 77);
    for (const auto& layer : alphaEdited.layers) {
        for (std::size_t offset = 3; offset < layer.rgba.size(); offset += 4) require(layer.rgba[offset] == 77, "base alpha edit missed a pixel");
        for (const auto& mip : layer.mipmaps) {
            for (std::size_t offset = 3; offset < mip.rgba.size(); offset += 4) require(mip.rgba[offset] == 77, "alpha edit missed a stored mipmap");
        }
    }
}

void testDdsVariantsAndCubemaps(const fs::path& root) {
    std::vector<std::uint8_t> pitchedBgr24(136, 0);
    pitchedBgr24[0] = 'D'; pitchedBgr24[1] = 'D'; pitchedBgr24[2] = 'S'; pitchedBgr24[3] = ' ';
    writeLe32(pitchedBgr24, 4, 124);
    writeLe32(pitchedBgr24, 8, 0x0000100Fu);
    writeLe32(pitchedBgr24, 12, 2);
    writeLe32(pitchedBgr24, 16, 1);
    writeLe32(pitchedBgr24, 20, 4);
    writeLe32(pitchedBgr24, 28, 1);
    writeLe32(pitchedBgr24, 76, 32);
    writeLe32(pitchedBgr24, 80, 0x40);
    writeLe32(pitchedBgr24, 88, 24);
    writeLe32(pitchedBgr24, 92, 0x00FF0000);
    writeLe32(pitchedBgr24, 96, 0x0000FF00);
    writeLe32(pitchedBgr24, 100, 0x000000FF);
    writeLe32(pitchedBgr24, 108, 0x1000);
    pitchedBgr24[128] = 0; pitchedBgr24[129] = 0; pitchedBgr24[130] = 255; pitchedBgr24[131] = 0xEE;
    pitchedBgr24[132] = 0; pitchedBgr24[133] = 255; pitchedBgr24[134] = 0; pitchedBgr24[135] = 0xDD;
    const auto pitched = neotpc::texture::loadTextureBytes(pitchedBgr24, "pitched-bgr24.dds");
    require(pitched.layers.front().rgba == std::vector<std::uint8_t>({255, 0, 0, 255, 0, 255, 0, 255}),
            "DDS BGR24 row pitch was ignored");

    const auto a1r5g5b5 = neotpc::texture::loadTextureBytes(
        makeDds16(0x41, 0x7C00, 0x03E0, 0x001F, 0x8000, {0xFC00, 0x03E0}), "a1r5g5b5.bin");
    require(a1r5g5b5.sourceEncoding.find("A1R5G5B5") != std::string::npos, "A1R5G5B5 DDS was not identified");
    require(a1r5g5b5.layers.front().rgba[0] > 245 && a1r5g5b5.layers.front().rgba[3] == 255,
            "A1R5G5B5 red/alpha decoding is wrong");
    require(a1r5g5b5.layers.front().rgba[5] > 245 && a1r5g5b5.layers.front().rgba[7] == 0,
            "A1R5G5B5 green/transparent decoding is wrong");

    const auto r5g6b5 = neotpc::texture::loadTextureBytes(
        makeDds16(0x40, 0xF800, 0x07E0, 0x001F, 0, {0xF800, 0x07E0}), "r5g6b5.bin");
    require(r5g6b5.sourceEncoding.find("R5G6B5") != std::string::npos, "R5G6B5 DDS was not identified");
    require(r5g6b5.layers.front().rgba[0] > 245 && r5g6b5.layers.front().rgba[5] > 245,
            "R5G6B5 channel decoding is wrong");

    const auto argb4444 = neotpc::texture::loadTextureBytes(
        makeDds16(0x41, 0x0F00, 0x00F0, 0x000F, 0xF000, {0xFF00, 0x80F0}), "argb4444.bin");
    require(argb4444.sourceEncoding.find("ARGB4444") != std::string::npos, "ARGB4444 DDS was not identified");
    require(argb4444.layers.front().rgba[0] == 255 && argb4444.layers.front().rgba[3] == 255,
            "ARGB4444 red/alpha decoding is wrong");
    require(argb4444.layers.front().rgba[5] == 255 && argb4444.layers.front().rgba[7] >= 128,
            "ARGB4444 green/alpha decoding is wrong");

    auto dxt3Source = gradient(true);
    neotpc::texture::TextureSaveOptions options;
    options.generateMipmaps = false;
    options.compression = neotpc::texture::TextureCompression::Dxt3;
    const auto dxt3Path = root / "explicit-alpha.dds";
    neotpc::texture::saveTexture(dxt3Source, dxt3Path, options);
    const auto dxt3 = neotpc::texture::loadTexture(dxt3Path);
    require(dxt3.preferredCompression == neotpc::texture::TextureCompression::Dxt3,
            "DDS DXT3 writer did not emit DXT3");
    require(meanSquaredError(dxt3Source.layers.front(), dxt3.layers.front(), true) < 1500.0,
            "DDS DXT3 round-trip error is too high");

    const auto cubeSource = mipTexture(6);
    options.generateMipmaps = true;
    options.compression = neotpc::texture::TextureCompression::None;
    const auto ddsCubePath = root / "cube.dds";
    neotpc::texture::saveTexture(cubeSource, ddsCubePath, options);
    const auto cubeBytes = neotpc::texture::readFileBytes(ddsCubePath);
    require(readLe32(cubeBytes, 112) == 0x0000FE00u, "DDS cubemap caps are incomplete");
    const auto ddsCube = neotpc::texture::loadTexture(ddsCubePath);
    require(ddsCube.cubeMap && ddsCube.layers.size() == 6 && ddsCube.cubeFaces.size() == 6,
            "DDS cubemap did not load six identified faces");
    for (std::size_t face = 0; face < cubeSource.layers.size(); ++face) {
        requireLayerExact(cubeSource.layers[face], ddsCube.layers[face], "DDS cube face " + std::to_string(face));
    }

    neotpc::texture::TextureData partialCube;
    partialCube.kind = neotpc::texture::TextureFileKind::Dds;
    partialCube.canvasWidth = 2;
    partialCube.canvasHeight = 2;
    partialCube.cubeMap = true;
    partialCube.cubeFaces = {
        neotpc::texture::CubeFace::PositiveX,
        neotpc::texture::CubeFace::PositiveZ,
    };
    partialCube.layers.push_back(solidLayer(2, 2, {255, 0, 0, 255}));
    partialCube.layers.push_back(solidLayer(2, 2, {0, 255, 0, 255}));
    options.generateMipmaps = false;
    options.compression = neotpc::texture::TextureCompression::None;
    const auto partialCubePath = root / "partial-cube.dds";
    neotpc::texture::saveTexture(partialCube, partialCubePath, options);
    const auto partialBytes = neotpc::texture::readFileBytes(partialCubePath);
    require(readLe32(partialBytes, 112) == 0x00004600u, "partial DDS cubemap face flags were not preserved");
    const auto partialDecoded = neotpc::texture::loadTexture(partialCubePath);
    require(partialDecoded.cubeMap && partialDecoded.cubeFaces == partialCube.cubeFaces &&
            partialDecoded.layers.size() == partialCube.layers.size(),
            "partial DDS cubemap face identities did not round-trip");
    for (std::size_t face = 0; face < partialCube.layers.size(); ++face) {
        requireLayerExact(partialCube.layers[face], partialDecoded.layers[face],
                          "partial DDS cube face " + std::to_string(face));
    }

    neotpc::texture::TextureData unorderedCube = partialCube;
    unorderedCube.cubeFaces = {
        neotpc::texture::CubeFace::PositiveZ,
        neotpc::texture::CubeFace::PositiveX,
    };
    unorderedCube.layers = {
        solidLayer(2, 2, {255, 0, 0, 255}), // +Z
        solidLayer(2, 2, {0, 255, 0, 255}), // +X
    };
    const auto unorderedCubePath = root / "unordered-partial-cube.dds";
    neotpc::texture::saveTexture(unorderedCube, unorderedCubePath, options);
    const auto unorderedBytes = neotpc::texture::readFileBytes(unorderedCubePath);
    require(readLe32(unorderedBytes, 112) == 0x00004600u,
            "unordered partial DDS cubemap face flags were not preserved");
    const auto unorderedDecoded = neotpc::texture::loadTexture(unorderedCubePath);
    const std::vector<neotpc::texture::CubeFace> canonicalSubset = {
        neotpc::texture::CubeFace::PositiveX,
        neotpc::texture::CubeFace::PositiveZ,
    };
    require(unorderedDecoded.cubeFaces == canonicalSubset && unorderedDecoded.layers.size() == 2,
            "DDS writer did not serialize a partial cubemap in canonical face order");
    requireLayerExact(unorderedCube.layers[1], unorderedDecoded.layers[0],
                      "unordered DDS +X cube-face identity");
    requireLayerExact(unorderedCube.layers[0], unorderedDecoded.layers[1],
                      "unordered DDS +Z cube-face identity");

    options.generateMipmaps = true;
    options.compression = neotpc::texture::TextureCompression::Dxt5;
    const auto tpcCubePath = root / "cube.tpc";
    neotpc::texture::saveTexture(cubeSource, tpcCubePath, options);
    const auto tpcCube = neotpc::texture::loadTexture(tpcCubePath);
    require(tpcCube.cubeMap && tpcCube.layers.size() == 6 && tpcCube.cubeFaces.size() == 6,
            "TPC cubemap did not load six identified canonical faces");
    for (std::size_t face = 0; face < cubeSource.layers.size(); ++face) {
        require(meanSquaredError(cubeSource.layers[face], tpcCube.layers[face], true) < 1500.0,
                "TPC cubemap face mapping or orientation changed for face " + std::to_string(face));
        require(tpcCube.layers[face].mipmaps.size() == cubeSource.layers[face].mipmaps.size(),
                "TPC cubemap mipmaps were not preserved");
    }
}

void testTxbInput(const fs::path& root) {
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 4;
    constexpr std::size_t payloadSize = width * height * 4;
    std::vector<std::uint8_t> bytes(128 + payloadSize, 0);
    writeLe32(bytes, 0, static_cast<std::uint32_t>(payloadSize));
    writeLe32(bytes, 4, 0x3F800000u);
    writeLe16(bytes, 8, width);
    writeLe16(bytes, 10, height);
    bytes[12] = 0x04;
    bytes[13] = 1;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t output = 128 + static_cast<std::size_t>(mortonOffset(x, y, width, height)) * 4;
            const std::uint8_t red = static_cast<std::uint8_t>(10 + x * 20);
            const std::uint8_t green = static_cast<std::uint8_t>(30 + y * 30);
            const std::uint8_t blue = static_cast<std::uint8_t>(50 + x + y);
            bytes[output + 0] = blue;
            bytes[output + 1] = green;
            bytes[output + 2] = red;
            bytes[output + 3] = 200;
        }
    }
    const std::string txi = "decal1\r\nmipmap 1\r\n";
    bytes.insert(bytes.end(), txi.begin(), txi.end());
    bytes.push_back(0);
    bytes.push_back(0);
    const auto decoded = neotpc::texture::loadTextureBytes(bytes, "misnamed-resource.data");
    require(decoded.kind == neotpc::texture::TextureFileKind::Txb, "content detection did not route TXB input");
    require(decoded.preferredCompression == neotpc::texture::TextureCompression::SwizzledBgra,
            "TXB swizzled encoding metadata was not retained");
    require(decoded.txi.find('\0') == std::string::npos, "TXB TXI padding was not trimmed");
    const auto& layer = decoded.layers.front();
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            require(layer.rgba[offset + 0] == static_cast<std::uint8_t>(10 + x * 20) &&
                    layer.rgba[offset + 1] == static_cast<std::uint8_t>(30 + (height - 1 - y) * 30) &&
                    layer.rgba[offset + 3] == 200,
                    "TXB Morton deswizzle or orientation is wrong");
        }
    }
    const auto issues = neotpc::texture::validateTxiText(decoded.txi);
    require(std::none_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.severity != neotpc::texture::TxiIssueSeverity::Info;
    }), "TXB legacy decal1 metadata did not validate");
    require(neotpc::texture::isSupportedTexturePath("input.txb"), "batch discovery does not accept TXB input");

    const auto txbPath = root / "xbox-input.txb";
    neotpc::texture::writeFileBytes(txbPath, bytes);
    const auto fromFile = neotpc::texture::loadTexture(txbPath);
    requireLayerExact(decoded.layers.front(), fromFile.layers.front(), "TXB file input");
    neotpc::texture::TextureSaveOptions options;
    options.compression = neotpc::texture::TextureCompression::SwizzledBgra;
    options.generateMipmaps = false;
    const auto convertedPath = root / "xbox-converted.tpc";
    neotpc::texture::saveTexture(fromFile, convertedPath, options);
    const auto converted = neotpc::texture::loadTexture(convertedPath);
    requireLayerExact(decoded.layers.front(), converted.layers.front(), "TXB to TPC conversion");
}

void testLayoutInferenceAndAnimation(const fs::path& root) {
    neotpc::texture::TextureData strip;
    strip.kind = neotpc::texture::TextureFileKind::Tga;
    strip.canvasWidth = 4;
    strip.canvasHeight = 24;
    auto canvas = solidLayer(4, 24, {0, 0, 0, 255});
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = face * 4; y < face * 4 + 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                canvas.rgba[(static_cast<std::size_t>(y) * 4 + x) * 4] = static_cast<std::uint8_t>(20 + face * 30);
            }
        }
    }
    strip.layers.push_back(std::move(canvas));
    const auto stripPath = root / "six-by-one.tga";
    neotpc::texture::saveTexture(strip, stripPath);
    const auto inferred = neotpc::texture::loadTexture(stripPath);
    require(inferred.cubeMap && inferred.layers.size() == 6, "6:1 TGA cubemap inference failed");

    strip.txi = "proceduretype cycle\nnumx 1\nnumy 6\nfps 8\n";
    const auto verticalAnimationPath = root / "six-by-one-animation.tga";
    neotpc::texture::saveTexture(strip, verticalAnimationPath);
    const auto verticalAnimation = neotpc::texture::loadTexture(verticalAnimationPath);
    require(verticalAnimation.animated && !verticalAnimation.cubeMap && verticalAnimation.layers.size() == 6,
            "explicit 1x6 animation metadata did not override TGA cubemap inference");
    neotpc::texture::TextureSaveOptions animationOptions;
    animationOptions.compression = neotpc::texture::TextureCompression::Dxt5;
    animationOptions.generateMipmaps = false;
    const auto verticalTpcPath = root / "six-by-one-animation.tpc";
    neotpc::texture::saveTexture(verticalAnimation, verticalTpcPath, animationOptions);
    const auto verticalTpc = neotpc::texture::loadTexture(verticalTpcPath);
    require(verticalTpc.animated && !verticalTpc.cubeMap && verticalTpc.layers.size() == 6,
            "1x6 animated TPC was confused with the cubemap height convention");
    require(verticalTpc.layers.front().mipmaps.size() == 2,
            "animated TPC did not retain the format-required complete mip chain");

    auto atlas = gradient(true);
    atlas.txi = "proceduretype cycle\nnumx 2\nnumy 2\n";
    const auto staticPath = root / "cycle-without-fps.tga";
    neotpc::texture::saveTexture(atlas, staticPath);
    const auto withoutFps = neotpc::texture::loadTexture(staticPath);
    require(!withoutFps.animated && withoutFps.layers.size() == 1,
            "proceduretype cycle without fps was incorrectly split into frames");

    atlas.txi += "fps 8\n";
    const auto animatedPath = root / "cycle-with-fps.tga";
    neotpc::texture::saveTexture(atlas, animatedPath);
    const auto withFps = neotpc::texture::loadTexture(animatedPath);
    require(withFps.animated && withFps.layers.size() == 4,
            "proceduretype cycle with dimensions and fps was not split into frames");
}

void testJpeg(const fs::path& root) {
    const auto source = gradient(false);
    neotpc::texture::TextureSaveOptions options;
    options.jpegQuality = 92;
    const auto path = root / "image.jpg";
    neotpc::texture::saveTexture(source, path, options);
    const auto decoded = neotpc::texture::loadTexture(path);
    require(decoded.layers.front().width == source.layers.front().width &&
            decoded.layers.front().height == source.layers.front().height, "JPEG dimensions changed");
    require(meanSquaredError(source.layers.front(), decoded.layers.front(), false) < 450.0, "JPEG quality regression");

    auto invalidHuffman = neotpc::texture::readFileBytes(path);
    const std::array<std::uint8_t, 2> startOfScan{{0xFF, 0xDA}};
    const auto scan = std::search(invalidHuffman.begin(), invalidHuffman.end(),
                                  startOfScan.begin(), startOfScan.end());
    require(scan != invalidHuffman.end() &&
            static_cast<std::size_t>(std::distance(scan, invalidHuffman.end())) > 6,
            "generated JPEG is missing its start-of-scan marker");
    scan[6] = 0x33; // Select undefined DC/AC Huffman tables after the header has been accepted.
    std::string decoderError;
    try {
        (void)neotpc::texture::loadTextureBytes(invalidHuffman, "invalid-huffman.jpg");
    } catch (const std::exception& error) {
        decoderError = error.what();
    }
    require(decoderError.find("Huffman table") != std::string::npos,
            "JPEG fatal decode error did not cross the guarded codec boundary");

    std::string boundedEncoderError;
    {
        neotpc::texture::parser::ScopedResourceLimits limits(
            100, neotpc::texture::parser::maxDecodedBytes());
        try {
            neotpc::texture::saveTexture(source, root / "jpeg-over-budget.jpg", options);
        } catch (const std::exception& error) {
            boundedEncoderError = error.what();
        }
    }
    require(boundedEncoderError.find("Encoded JPEG exceeds") != std::string::npos,
            "JPEG encoder ignored its bounded output-buffer limit");
}

void testTxi() {
    const std::string valid = "proceduretype cycle\nnumx 4\nnumy 2\nfps 12\nblending additive\n";
    const auto issues = neotpc::texture::validateTxiText(valid);
    require(std::none_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.severity == neotpc::texture::TxiIssueSeverity::Error;
    }), "valid TXI produced an error");
    const auto invalid = neotpc::texture::txiValidationReport("additive\nspecularcolor nope\n");
    require(invalid.find("value for another directive") != std::string::npos, "TXI value-token error missing");
    require(invalid.find("Expected one or more numeric values") != std::string::npos, "TXI numeric error missing");
    const auto updated = neotpc::texture::setTxiValue(valid, "blending", "punchthrough");
    require(neotpc::texture::getTxiValue(updated, "blending") == std::optional<std::string>("punchthrough"), "TXI setter failed");

    const std::string fontAndCompatibility =
        "numchars 2\n"
        "rows 1\n"
        "cols 2\n"
        "numcharspersheet 2\n"
        "fontwidth 12.5\n"
        "codepage 1252\n"
        "isdoublebyte 0\n"
        "upperleftcoords 2\n"
        "0.0 0.0 65\n"
        "0.5 0.0 66\n"
        "lowerrightcoords 2\n"
        "0.5 1.0 65\n"
        "1.0 1.0 66\n"
        "candownsample 1\n"
        "downsamplefactor 0.5\n"
        "maxsizehq 2048\n"
        "minsizelq 64\n"
        "ondemand 0\n"
        "priority 2\n"
        "unique 1\n"
        "xbox_downsample 1\n"
        "decal1\n";
    const auto fontIssues = neotpc::texture::validateTxiText(fontAndCompatibility);
    require(std::none_of(fontIssues.begin(), fontIssues.end(), [](const auto& issue) {
        return issue.severity == neotpc::texture::TxiIssueSeverity::Error ||
               issue.severity == neotpc::texture::TxiIssueSeverity::Warning;
    }), "valid font coordinate blocks or compatibility TXI keys produced an issue");
    const auto entries = neotpc::texture::parseTxiEntries(fontAndCompatibility);
    require(std::count_if(entries.begin(), entries.end(), [](const auto& entry) { return entry.coordinateData; }) == 4,
            "TXI coordinate record rows were not parsed structurally");
    require(std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        return !entry.coordinateData && entry.key == "decal" && entry.value == "1";
    }), "legacy decal1 alias was not normalized");

    const auto incomplete = neotpc::texture::txiValidationReport("upperleftcoords 2\n0.0 0.0 65\n");
    require(incomplete.find("ended early") != std::string::npos, "incomplete TXI coordinate block was accepted");
    const auto malformedRow = neotpc::texture::txiValidationReport("upperleftcoords 1\nnot coordinates\n");
    require(malformedRow.find("Expected a coordinate row") != std::string::npos, "malformed TXI coordinate row was accepted");

    bool structuredSetterRejected = false;
    try {
        (void)neotpc::texture::setTxiValue(fontAndCompatibility, "upperleftcoords", "1");
    } catch (const std::exception&) {
        structuredSetterRejected = true;
    }
    require(structuredSetterRejected, "single-line TXI setter allowed a counted coordinate block to be corrupted");

    const std::string nulPadded("mipmap 1\0\0", 11);
    const auto nulIssues = neotpc::texture::validateTxiText(nulPadded);
    require(std::none_of(nulIssues.begin(), nulIssues.end(),
                         [](const auto& issue) { return issue.severity == neotpc::texture::TxiIssueSeverity::Error; }),
            "NUL-padded embedded TXI did not validate");

    const auto directiveCompletion = neotpc::texture::txiAutocomplete("bl");
    require(directiveCompletion.kind == neotpc::texture::TxiAutocompleteKind::Directive &&
                directiveCompletion.replacementLength == 2 &&
                std::find(directiveCompletion.suggestions.begin(), directiveCompletion.suggestions.end(),
                          "blending") != directiveCompletion.suggestions.end(),
            "TXI directive autocomplete did not use the catalog");
    require(std::find(directiveCompletion.suggestions.begin(), directiveCompletion.suggestions.end(),
                      "additive") == directiveCompletion.suggestions.end(),
            "TXI directive autocomplete exposed a value token as a standalone key");

    const auto enumCompletion = neotpc::texture::txiAutocomplete("blending pu");
    require(enumCompletion.kind == neotpc::texture::TxiAutocompleteKind::Value &&
                enumCompletion.directive == "blending" &&
                enumCompletion.replacementLength == 2 &&
                enumCompletion.suggestions == std::vector<std::string>{"punchthrough"},
            "TXI enum value autocomplete did not filter allowed values");

    const auto procedureCompletion = neotpc::texture::txiAutocomplete("proceduretype ");
    require(std::find(procedureCompletion.suggestions.begin(), procedureCompletion.suggestions.end(),
                      "cycle") != procedureCompletion.suggestions.end() &&
                std::find(procedureCompletion.suggestions.begin(), procedureCompletion.suggestions.end(),
                          "water") != procedureCompletion.suggestions.end(),
            "TXI proceduretype autocomplete omitted indexed values");

    const auto bumpCompletion = neotpc::texture::txiAutocomplete("isbumpmap ");
    require(bumpCompletion.suggestions == std::vector<std::string>({"0", "1", "2"}),
            "TXI isbumpmap autocomplete omitted the authored-normal value");

    const auto numericDefaultCompletion = neotpc::texture::txiAutocomplete("fps ");
    require(numericDefaultCompletion.suggestions == std::vector<std::string>{"1.0"},
            "TXI autocomplete did not expose a numeric catalog default");

    const auto allDirectives = neotpc::texture::txiAutocomplete("   ", true);
    require(allDirectives.kind == neotpc::texture::TxiAutocompleteKind::Directive &&
                allDirectives.replacementLength == 0 &&
                allDirectives.suggestions.size() > 20,
            "explicit TXI autocomplete did not list the directive index");

    std::vector<std::string> indexedDirectives;
    for (const auto& directive : neotpc::texture::txiDirectiveCatalog()) {
        if (directive.valueKind != neotpc::texture::TxiDirectiveValueKind::ValueToken) {
            indexedDirectives.push_back(directive.name);
        }
        if (directive.valueKind == neotpc::texture::TxiDirectiveValueKind::Enum) {
            auto expected = directive.allowedValues;
            std::sort(expected.begin(), expected.end());
            expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
            require(neotpc::texture::txiAutocomplete(directive.name + " ").suggestions == expected,
                    "TXI enum autocomplete drifted from the directive catalog for " + directive.name);
        }
    }
    std::sort(indexedDirectives.begin(), indexedDirectives.end());
    indexedDirectives.erase(std::unique(indexedDirectives.begin(), indexedDirectives.end()),
                            indexedDirectives.end());
    require(allDirectives.suggestions == indexedDirectives,
            "TXI directive autocomplete drifted from the searchable catalog");
    require(neotpc::texture::txiAutocomplete("# blend", true).suggestions.empty(),
            "TXI autocomplete offered suggestions inside a comment");
    require(neotpc::texture::txiDirectiveHint("blending").find("punchthrough") != std::string::npos,
            "TXI autocomplete hint omitted indexed enum values");
    require(neotpc::texture::txiValueHint("blending", "punchthrough").find("cutout") != std::string::npos,
            "TXI autocomplete value hint did not use the indexed value description");
}

void testFixtures() {
    const fs::path fixtures = fs::path(NEOTPC_TEST_FIXTURE_DIR);
    for (const char* filename : {"test.tpc", "test.dds", "cm_506ond.tga"}) {
        const auto texture = neotpc::texture::loadTexture(fixtures / filename);
        require(texture.hasPixels(), std::string("fixture did not decode: ") + filename);
    }
}

void testBatch(const fs::path& root) {
    const auto input = root / "batch-in";
    const auto output = root / "batch-out";
    fs::create_directories(input / "nested");
    const auto source = gradient(true);
    neotpc::texture::saveTexture(source, input / "nested" / "same.tga");
    neotpc::texture::saveTexture(source, input / "nested" / "same.bmp");

    neotpc::texture::BatchOptions options;
    options.outputExtension = "png";
    options.recursive = true;
    const auto report = neotpc::texture::batchConvertTextures(input, output, options);
    require(report.ok() && report.converted == 2, "batch conversion did not convert both source formats");
    require(fs::is_regular_file(output / "nested" / "same.png"), "batch primary output missing");
    require(fs::is_regular_file(output / "nested" / "same.tga.png") ||
            fs::is_regular_file(output / "nested" / "same.bmp.png"), "batch collision-safe output missing");

    const auto external = root / "external.tga";
    neotpc::texture::saveTexture(source, external);
    const auto inputLink = input / "outside.tga";
    std::error_code linkError;
    fs::create_symlink(external, inputLink, linkError);
    if (!linkError) {
        const auto symlinkReport = neotpc::texture::batchConvertTextures(input, output, options);
        require(symlinkReport.discovered == 2,
                "batch conversion followed a symlinked input file");
        require(!fs::exists(root / "external.png"),
                "batch conversion wrote outside its output directory through an input symlink");
    }

    const auto escapeInput = root / "batch-escape-in";
    const auto escapeOutput = root / "batch-escape-out";
    const auto escaped = root / "batch-escaped-destination";
    fs::create_directories(escapeInput / "redirect");
    fs::create_directories(escapeOutput);
    fs::create_directories(escaped);
    neotpc::texture::saveTexture(source, escapeInput / "redirect" / "payload.tga");
    linkError.clear();
    fs::create_directory_symlink(escaped, escapeOutput / "redirect", linkError);
    if (!linkError) {
        const auto escapeReport = neotpc::texture::batchConvertTextures(
            escapeInput, escapeOutput, options);
        require(escapeReport.failed == 1,
                "batch conversion accepted a symlinked output directory");
        require(!fs::exists(escaped / "payload.png"),
                "batch conversion escaped through a symlinked output directory");
    }

    const auto fileLinkInput = root / "batch-file-link-in";
    const auto fileLinkOutput = root / "batch-file-link-out";
    const auto externalOutput = root / "batch-external-output.png";
    fs::create_directories(fileLinkInput);
    fs::create_directories(fileLinkOutput);
    neotpc::texture::saveTexture(source, fileLinkInput / "payload.tga");
    neotpc::texture::writeFileBytes(externalOutput, {0x4e, 0x45, 0x4f});
    linkError.clear();
    fs::create_symlink(externalOutput, fileLinkOutput / "payload.png", linkError);
    if (!linkError) {
        auto overwriteOptions = options;
        overwriteOptions.overwrite = true;
        const auto fileLinkReport = neotpc::texture::batchConvertTextures(
            fileLinkInput, fileLinkOutput, overwriteOptions);
        require(fileLinkReport.failed == 1,
                "batch conversion accepted a symlinked output file");
        require(neotpc::texture::readFileBytes(externalOutput) ==
                    std::vector<std::uint8_t>({0x4e, 0x45, 0x4f}),
                "batch conversion overwrote an external symlink target");
    }
}

void testConflictingImageDiscovery(const fs::path& root) {
    const auto directory = root / "conflicting-images";
    fs::create_directories(directory);
    const std::vector<std::string> expectedNames = {
        "DXUNtex.tga",
        "dxuntex.tpc",
        "dxUnTex(1).tga",
        "dxuntex.dds",
        "dxuntex(2).tpc",
        "dxuntex(2) - copy.tpc",
        "dxuNteX- copy.tga",
    };
    for (const auto& name : expectedNames) neotpc::texture::writeFileBytes(directory / name, {0});
    for (const char* name : {"dxuntexture.tga", "dxuntex_backup.tpc", "dxuntex.txi", "other.dds"}) {
        neotpc::texture::writeFileBytes(directory / name, {0});
    }

    require(neotpc::texture::canonicalTextureConflictStem("dxuntex(2) - copy.tpc") == "dxuntex",
            "copy and duplicate suffixes were not normalized together");
    require(neotpc::texture::canonicalTextureConflictStem("DXUNTEX - Copy (3).DDS") == "dxuntex",
            "case-insensitive conflict suffix normalization failed");
    const auto matches = neotpc::texture::findConflictingTexturePaths(directory / "DXUNtex.tga");
    require(matches.size() == expectedNames.size(), "conflicting-image discovery returned the wrong number of files");
    require(matches.front().filename() == fs::path("DXUNtex.tga"), "current image was not first in the conflict set");
    std::set<std::string> names;
    for (const auto& match : matches) names.insert(neotpc::texture::pathToUtf8(match.filename()));
    require(names == std::set<std::string>(expectedNames.begin(), expectedNames.end()),
            "conflicting-image discovery included a distractor or missed a filename variant");
    const auto fromSuffixedName = neotpc::texture::findConflictingTexturePaths(directory / "dxuntex(2) - copy.tpc");
    require(fromSuffixedName.size() == expectedNames.size() &&
            fromSuffixedName.front().filename() == fs::path("dxuntex(2) - copy.tpc"),
            "conflicting-image discovery did not use a suffixed current filename as the group root");

    const auto unicodeDirectory = root / fs::u8path(u8"conflicting-\u6e2c\u8a66-\U0001f5bc");
    fs::create_directories(unicodeDirectory);
    const std::vector<std::string> unicodeExpectedNames = {
        u8"\u6e2c\u8a66Texture.tga",
        u8"\u6e2c\u8a66texture(2).tpc",
        u8"\u6e2c\u8a66TEXTURE - copy.dds",
    };
    for (const auto& name : unicodeExpectedNames) {
        neotpc::texture::writeFileBytes(unicodeDirectory / fs::u8path(name), {0});
    }
    neotpc::texture::writeFileBytes(
        unicodeDirectory / fs::u8path(u8"\u6e2c\u8a66texture_backup.tga"), {0});

    const auto unicodeCurrent = unicodeDirectory / fs::u8path(unicodeExpectedNames.front());
    const auto unicodeMatches = neotpc::texture::findConflictingTexturePaths(unicodeCurrent);
    require(unicodeMatches.size() == unicodeExpectedNames.size(),
            "Unicode conflicting-image discovery returned the wrong number of files");
    require(unicodeMatches.front() == unicodeCurrent,
            "Unicode current image was not first in the conflict set");
    std::set<std::string> unicodeNames;
    for (const auto& match : unicodeMatches) {
        unicodeNames.insert(neotpc::texture::pathToUtf8(match.filename()));
    }
    require(unicodeNames == std::set<std::string>(unicodeExpectedNames.begin(), unicodeExpectedNames.end()),
            "Unicode conflicting-image discovery lost or misidentified a filename");
    require(neotpc::texture::pathToUtf8(unicodeCurrent).find(unicodeExpectedNames.front()) != std::string::npos,
            "Unicode path conversion did not preserve the filename as UTF-8");
}

void testDocument(const fs::path& root) {
    const auto source = root / "document-source.tga";
    neotpc::texture::saveTexture(gradient(true), source);

    neotpc::TextureDocument document;
    document.open(source);
    require(document.isOpen() && !document.dirty(), "document did not open cleanly");
    document.invertAlpha();
    require(document.dirty() && document.contentDirty(),
            "pixel edit did not mark document content dirty");

    const auto output = root / "document-output.png";
    document.saveAs(output);
    require(document.path() == output && !document.dirty(), "save-as did not reset document state");
    document.setTxi("blending additive\n");
    require(document.dirty() && document.txiDirty(), "TXI edit did not mark TXI state dirty");
    document.save();
    require(!document.dirty(), "save did not reset document state");
    document.close();
    require(!document.isOpen(), "document did not close");

    auto tpcSource = gradient(true);
    tpcSource.txi = "downsamplemax 0\nblending additive\n";
    neotpc::texture::TextureSaveOptions tpcOptions;
    tpcOptions.compression = neotpc::texture::TextureCompression::Dxt5;
    tpcOptions.generateMipmaps = true;
    const auto tpc = root / "document-embedded.tpc";
    neotpc::texture::saveTexture(tpcSource, tpc, tpcOptions);

    document.open(tpc);
    const auto beforeTexture = document.texture();
    const auto beforeBytes = neotpc::texture::readFileBytes(tpc);
    require(beforeBytes.size() >= beforeTexture.txi.size(),
            "document TPC has an invalid TXI footer size");
    const auto payloadEnd = beforeBytes.size() - beforeTexture.txi.size();

    auto equivalentLineEndings = beforeTexture.txi;
    equivalentLineEndings.erase(
        std::remove(equivalentLineEndings.begin(), equivalentLineEndings.end(), '\r'),
        equivalentLineEndings.end());
    document.setTxi(equivalentLineEndings);
    require(!document.dirty(), "line-ending-only TXI edit dirtied the document");

    document.setTxi(neotpc::texture::setTxiValue(
        document.texture().txi, "blending", "punchthrough"));
    require(document.txiDirty() && document.canPatchEmbeddedTxi(),
            "TXI-only TPC edit was not eligible for footer replacement");
    require(document.summary().find("replace embedded TXI only") != std::string::npos,
            "document summary did not identify the footer-only save path");

    auto changedOptions = document.saveOptions();
    changedOptions.bicubicMipmaps = !changedOptions.bicubicMipmaps;
    document.setSaveOptions(changedOptions);
    require(document.optionsDirty() && !document.canPatchEmbeddedTxi(),
            "save-option edit did not disable footer-only replacement");
    changedOptions.bicubicMipmaps = !changedOptions.bicubicMipmaps;
    document.setSaveOptions(changedOptions);
    require(!document.optionsDirty() && document.canPatchEmbeddedTxi(),
            "reverting save options did not restore footer-only replacement");

    document.save();
    require(!document.dirty(), "footer-only document save did not reset dirty state");
    const auto afterBytes = neotpc::texture::readFileBytes(tpc);
    require(afterBytes.size() >= payloadEnd,
            "footer-only document save truncated the TPC");
    require(std::equal(beforeBytes.begin(),
                       beforeBytes.begin() + static_cast<std::ptrdiff_t>(payloadEnd),
                       afterBytes.begin()),
            "footer-only document save changed encoded TPC bytes");
    const auto savedTpc = neotpc::texture::loadTexture(tpc);
    require(neotpc::texture::getTxiValue(savedTpc.txi, "blending") ==
                std::optional<std::string>("punchthrough"),
            "footer-only document save did not persist TXI metadata");
}

void testMalformedInput(const fs::path& root) {
    const auto malformed = root / "bad.tga";
    neotpc::texture::writeFileBytes(malformed, {0, 0, 2});
    bool rejected = false;
    try {
        (void)neotpc::texture::loadTexture(malformed);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "truncated TGA was accepted");

    auto tiny = gradient(false);
    tiny.layers.front().width = 2;
    tiny.layers.front().height = 1;
    tiny.canvasWidth = 2;
    tiny.canvasHeight = 1;
    tiny.layers.front().rgba.resize(8);
    tiny.txi = "proceduretype cycle\nnumx 2147483648\nnumy 1\ndefaultwidth 2\ndefaultheight 1\nfps 1\n";
    const auto path = root / "overflow.tga";
    tiny.txi.clear();
    neotpc::texture::saveTexture(tiny, path);
    neotpc::texture::writeFileBytes(root / "overflow.txi", {'p','r','o','c','e','d','u','r','e','t','y','p','e',' ','c','y','c','l','e','\n',
        'n','u','m','x',' ','2','1','4','7','4','8','3','6','4','8','\n','n','u','m','y',' ','1','\n',
        'd','e','f','a','u','l','t','w','i','d','t','h',' ','2','\n','d','e','f','a','u','l','t','h','e','i','g','h','t',' ','1','\n',
        'f','p','s',' ','1','\n'});
    rejected = false;
    try {
        (void)neotpc::texture::loadTexture(path);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "overflowing TXI animation layout was accepted");

    rejected = false;
    try {
        auto invalidScale = gradient(true);
        neotpc::texture::scaleTextureAlpha(invalidScale, std::numeric_limits<double>::quiet_NaN());
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "non-finite alpha scale was accepted");
}

void testScopedResourceLimits(const fs::path& root) {
    const auto path = root / "resource-limits.png";
    neotpc::texture::saveTexture(gradient(true), path);
    const auto defaultFileLimit = neotpc::texture::parser::maxInMemoryFileBytes();
    const auto defaultDecodedLimit = neotpc::texture::parser::maxDecodedBytes();

    bool rejected = false;
    {
        neotpc::texture::parser::ScopedResourceLimits limits(1, defaultDecodedLimit);
        try {
            (void)neotpc::texture::readFileBytes(path);
        } catch (const std::exception&) {
            rejected = true;
        }
    }
    require(rejected, "scoped input-file limit was not enforced");

    rejected = false;
    {
        neotpc::texture::parser::ScopedResourceLimits limits(defaultFileLimit, 100);
        try {
            (void)neotpc::texture::loadTexture(path);
        } catch (const std::exception&) {
            rejected = true;
        }
    }
    require(rejected, "scoped decoded-image limit was not enforced");
    require(neotpc::texture::parser::maxInMemoryFileBytes() == defaultFileLimit &&
            neotpc::texture::parser::maxDecodedBytes() == defaultDecodedLimit,
            "scoped parser limits were not restored");

    {
        neotpc::texture::parser::ScopedResourceLimits outer(1000, 2000);
        neotpc::texture::parser::ScopedResourceLimits inner(2000, 3000);
        require(neotpc::texture::parser::maxInMemoryFileBytes() == 1000 &&
                neotpc::texture::parser::maxDecodedBytes() == 2000,
                "nested parser limits widened an outer budget");
    }
}

void testDeterministicMalformedCorpus() {
    std::uint32_t state = 0x4e656f54u;
    for (const char* extension : {"tpc", "txb", "tga", "dds", "png", "jpg", "bmp"}) {
        for (std::size_t size = 0; size <= 128; ++size) {
            std::vector<std::uint8_t> bytes(size);
            for (auto& byte : bytes) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                byte = static_cast<std::uint8_t>(state);
            }
            try {
                (void)neotpc::texture::loadTextureBytes(bytes, fs::path(std::string("malformed.") + extension));
            } catch (const std::exception&) {
                // Rejection is expected; the corpus checks bounded failure paths under sanitizers.
            }
        }
    }
}

void testTransactionalSidecarRollback(const fs::path& root) {
    const auto output = root / "blocked.tga";
    const std::vector<std::uint8_t> original{'o', 'l', 'd'};
    neotpc::texture::writeFileBytes(output, original);
    fs::create_directory(root / "blocked.txi");

    bool failed = false;
    try {
        neotpc::texture::saveTexture(gradient(true), output);
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "save unexpectedly replaced a directory TXI target");
    require(neotpc::texture::readFileBytes(output) == original, "failed image/TXI save did not roll back the original image");
    require(fs::is_directory(root / "blocked.txi"), "failed image/TXI save damaged the sidecar directory");
}

} // namespace

int main() {
    TempDirectory temp;
    try {
        testLosslessCodecs(temp.path);
        testCompressedCodecs(temp.path);
        testTpcTxiEditingAndPairs(temp.path);
        testMipmapsAndSwizzledTpc(temp.path);
        testDdsVariantsAndCubemaps(temp.path);
        testTxbInput(temp.path);
        testLayoutInferenceAndAnimation(temp.path);
        testJpeg(temp.path);
        testTxi();
        testFixtures();
        testBatch(temp.path);
        testConflictingImageDiscovery(temp.path);
        testDocument(temp.path);
        testMalformedInput(temp.path);
        testScopedResourceLimits(temp.path);
        testDeterministicMalformedCorpus();
        testTransactionalSidecarRollback(temp.path);
        const auto codecReport = neotpc::texture::imageCodecSupportReport();
        require(codecReport.find("libspng") != std::string::npos,
                "codec support report does not identify libspng");
        require(codecReport.find("libjpeg") != std::string::npos,
                "codec support report does not identify the libjpeg dependency");
        std::cout << "All NeoTPC codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Codec test failure: " << error.what() << '\n';
        return 1;
    }
}
