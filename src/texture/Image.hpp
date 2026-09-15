#pragma once

#include <neoshared/texture/Image.hpp>

namespace neotpc::texture {
using neoshared::texture::TextureFileKind;
using neoshared::texture::DdsDialect;
using neoshared::texture::MipmapPolicy;
using neoshared::texture::MipmapAlpha;
using neoshared::texture::MipmapColor;
using neoshared::texture::TextureCompression;
using neoshared::texture::DxtCompressionQuality;
using neoshared::texture::DxtErrorMetric;
using neoshared::texture::CubeFace;
using neoshared::texture::TextureLayer;
using neoshared::texture::TxiFeatures;
using neoshared::texture::TextureSaveOptions;
using neoshared::texture::TextureData;
using neoshared::texture::EncodedTexture;
using neoshared::texture::TgaTxiPairPaths;
using neoshared::texture::textureCompressionToString;
using neoshared::texture::textureCompressionFromString;
using neoshared::texture::dxtCompressionQualityToString;
using neoshared::texture::dxtCompressionQualityFromString;
using neoshared::texture::dxtErrorMetricToString;
using neoshared::texture::dxtErrorMetricFromString;
using neoshared::texture::textureFileKindToString;
using neoshared::texture::cubeFaceToString;
using neoshared::texture::storesMipmaps;
using neoshared::texture::sameEncodingOptions;
using neoshared::texture::kindForExtension;
using neoshared::texture::saveEncodedTexture;
using neoshared::texture::replaceTpcEmbeddedTxi;
using neoshared::texture::patchTpcTxiBytes;
using neoshared::texture::saveTpcWithEmbeddedTxi;
using neoshared::texture::saveTgaTxiPair;
using neoshared::texture::splitTpcToTgaTxi;
using neoshared::texture::combineTgaTxiToTpc;
using neoshared::texture::parseTxiFeatures;
using neoshared::texture::setTxiValue;
using neoshared::texture::removeTxiValue;
using neoshared::texture::getTxiValue;
using neoshared::texture::textureSummary;
using neoshared::texture::textureMetadataText;
using neoshared::texture::setTextureAlpha;
using neoshared::texture::scaleTextureAlpha;
using neoshared::texture::invertTextureAlpha;
using neoshared::texture::flipTextureHorizontal;
using neoshared::texture::flipTextureVertical;

// NeoTPC adds PNG/JPEG/BMP interchange; all game formats use NeoShared.
TextureData loadTexture(const std::filesystem::path& path);
TextureData loadTextureBytes(const std::vector<std::uint8_t>& bytes,
                             const std::filesystem::path& virtualPath,
                             std::string sidecarTxi = {});
EncodedTexture encodeTexture(const TextureData& texture, const std::filesystem::path& output,
                             const TextureSaveOptions& options = {});
void saveTexture(const TextureData& texture, const std::filesystem::path& output,
                 const TextureSaveOptions& options = {});
std::string imageCodecSupportReport();
TextureSaveOptions importedTextureOptions(const TextureData& texture);
// Fast policy checks; full byte-layout validation still belongs to the encoder.
std::string textureOutputIssue(const TextureData& texture, const std::filesystem::path& output,
                               const TextureSaveOptions& options);

} // namespace neotpc::texture
