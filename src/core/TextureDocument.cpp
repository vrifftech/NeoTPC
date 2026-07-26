#include "core/TextureDocument.hpp"

#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"
#include "texture/Txi.hpp"

#include <cctype>
#include <sstream>
#include <utility>

namespace neotpc {
namespace {

std::string canonicalTxiFooter(std::string txi) {
    auto first = txi.begin();
    while (first != txi.end() && std::isspace(static_cast<unsigned char>(*first))) ++first;
    auto last = txi.end();
    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1)))) --last;
    txi = std::string(first, last);
    if (txi.empty()) return {};

    std::string normalized;
    normalized.reserve(txi.size() + 2);
    for (std::size_t index = 0; index < txi.size(); ++index) {
        const char ch = txi[index];
        if (ch == '\r') {
            if (index + 1 < txi.size() && txi[index + 1] == '\n') ++index;
            normalized += "\r\n";
        } else if (ch == '\n') {
            normalized += "\r\n";
        } else {
            normalized.push_back(ch);
        }
    }
    if (normalized.size() < 2 || normalized.compare(normalized.size() - 2, 2, "\r\n") != 0) {
        normalized += "\r\n";
    }
    return normalized;
}

bool sameSaveOptions(const neotpc::texture::TextureSaveOptions& left,
                     const neotpc::texture::TextureSaveOptions& right) noexcept {
    return left.compression == right.compression &&
           left.generateMipmaps == right.generateMipmaps &&
           left.bicubicMipmaps == right.bicubicMipmaps &&
           left.flipXOnSave == right.flipXOnSave &&
           left.flipYOnSave == right.flipYOnSave &&
           left.alphaBlending == right.alphaBlending &&
           left.dxtQuality == right.dxtQuality &&
           left.dxtMetric == right.dxtMetric &&
           left.weightColorByAlpha == right.weightColorByAlpha &&
           left.dxt1AlphaThreshold == right.dxt1AlphaThreshold &&
           left.jpegQuality == right.jpegQuality;
}

} // namespace

void TextureDocument::open(const std::filesystem::path& path) {
    auto loaded = neotpc::texture::loadTexture(path);
    texture_ = std::move(loaded);
    path_ = path;
    options_ = {};
    options_.compression = texture_.preferredCompression;
    options_.alphaBlending = texture_.alphaBlending;
    open_ = true;
    resetSavedState();
}

void TextureDocument::close() noexcept {
    open_ = false;
    txiDirty_ = false;
    contentDirty_ = false;
    optionsDirty_ = false;
    path_.clear();
    texture_ = {};
    options_ = {};
    savedTxi_.clear();
    savedOptions_ = {};
}

bool TextureDocument::canPatchEmbeddedTxi() const noexcept {
    return open_ && texture_.kind == neotpc::texture::TextureFileKind::Tpc &&
           neotpc::texture::extensionLower(path_) == "tpc" &&
           txiDirty_ && !contentDirty_ && !optionsDirty_;
}

void TextureDocument::setSaveOptions(const neotpc::texture::TextureSaveOptions& options) {
    if (!open_) return;
    options_ = options;
    optionsDirty_ = !sameSaveOptions(options_, savedOptions_);
}

void TextureDocument::setTxi(std::string txi) {
    if (!open_) return;
    texture_.txi = std::move(txi);
    txiDirty_ = canonicalTxiFooter(texture_.txi) != canonicalTxiFooter(savedTxi_);
}

void TextureDocument::setAlpha(std::uint8_t value) {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::setTextureAlpha(texture_, value);
    contentDirty_ = true;
}

void TextureDocument::scaleAlpha(double factor) {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::scaleTextureAlpha(texture_, factor);
    contentDirty_ = true;
}

void TextureDocument::invertAlpha() {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::invertTextureAlpha(texture_);
    contentDirty_ = true;
}

void TextureDocument::flipHorizontal() {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::flipTextureHorizontal(texture_);
    contentDirty_ = true;
}

void TextureDocument::flipVertical() {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::flipTextureVertical(texture_);
    contentDirty_ = true;
}

void TextureDocument::save() {
    if (!open_) throw neotpc::texture::TextureError("No texture is open");
    if (canPatchEmbeddedTxi()) {
        neotpc::texture::replaceTpcEmbeddedTxi(path_, texture_.txi);
        texture_ = neotpc::texture::loadTexture(path_);
        options_.compression = texture_.preferredCompression;
        options_.alphaBlending = texture_.alphaBlending;
        resetSavedState();
        return;
    }
    saveTo(path_);
}

void TextureDocument::saveAs(const std::filesystem::path& output) {
    if (!open_) throw neotpc::texture::TextureError("No texture is open");
    saveTo(output);
}

void TextureDocument::resetSavedState() {
    savedTxi_ = texture_.txi;
    savedOptions_ = options_;
    txiDirty_ = false;
    contentDirty_ = false;
    optionsDirty_ = false;
}

void TextureDocument::saveTo(const std::filesystem::path& output) {
    if (output.empty()) throw neotpc::texture::TextureError("Texture output path is empty");
    neotpc::texture::saveTexture(texture_, output, options_);
    texture_ = neotpc::texture::loadTexture(output);
    path_ = output;
    options_.compression = texture_.preferredCompression;
    options_.alphaBlending = texture_.alphaBlending;
    resetSavedState();
}

std::string TextureDocument::summary() const {
    if (!open_) return "No texture is open.\n";
    std::ostringstream out;
    out << neotpc::texture::textureSummary(texture_)
        << "\nSave settings\n"
        << "compression: " << neotpc::texture::textureCompressionToString(options_.compression) << '\n'
        << "DXT quality: " << neotpc::texture::dxtCompressionQualityToString(options_.dxtQuality) << '\n'
        << "DXT metric: " << neotpc::texture::dxtErrorMetricToString(options_.dxtMetric) << '\n'
        << "mipmaps: " << (options_.generateMipmaps ? "yes" : "no") << '\n'
        << "bicubic mipmaps: " << (options_.bicubicMipmaps ? "yes" : "no") << '\n'
        << "JPEG quality: " << static_cast<unsigned>(options_.jpegQuality) << '\n';
    if (canPatchEmbeddedTxi()) {
        out << "save operation: replace embedded TXI only (encoded image data is preserved)\n";
    }
    out << '\n' << neotpc::texture::txiValidationReport(texture_.txi);
    return out.str();
}

} // namespace neotpc
