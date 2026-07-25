// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/TextureDocument.hpp"

#include "texture/Error.hpp"
#include "texture/Txi.hpp"

#include <sstream>
#include <utility>

namespace neotpc {

void TextureDocument::open(const std::filesystem::path& path) {
    auto loaded = neotpc::texture::loadTexture(path);
    texture_ = std::move(loaded);
    path_ = path;
    options_ = {};
    options_.compression = texture_.preferredCompression;
    options_.alphaBlending = texture_.alphaBlending;
    open_ = true;
    dirty_ = false;
}

void TextureDocument::close() noexcept {
    open_ = false;
    dirty_ = false;
    path_.clear();
    texture_ = {};
    options_ = {};
}

void TextureDocument::setSaveOptions(const neotpc::texture::TextureSaveOptions& options) {
    if (!open_) return;
    if (neotpc::texture::textureMetadataText(texture_, options) !=
        neotpc::texture::textureMetadataText(texture_, options_)) {
        dirty_ = true;
    }
    options_ = options;
}

void TextureDocument::setTxi(std::string txi) {
    if (!open_ || texture_.txi == txi) return;
    texture_.txi = std::move(txi);
    dirty_ = true;
}

void TextureDocument::setAlpha(std::uint8_t value) {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::setTextureAlpha(texture_, value);
    dirty_ = true;
}

void TextureDocument::scaleAlpha(double factor) {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::scaleTextureAlpha(texture_, factor);
    dirty_ = true;
}

void TextureDocument::invertAlpha() {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::invertTextureAlpha(texture_);
    dirty_ = true;
}

void TextureDocument::flipHorizontal() {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::flipTextureHorizontal(texture_);
    dirty_ = true;
}

void TextureDocument::flipVertical() {
    if (!open_ || !texture_.hasPixels()) return;
    neotpc::texture::flipTextureVertical(texture_);
    dirty_ = true;
}

void TextureDocument::save() {
    if (!open_) throw neotpc::texture::TextureError("No texture is open");
    saveTo(path_);
}

void TextureDocument::saveAs(const std::filesystem::path& output) {
    if (!open_) throw neotpc::texture::TextureError("No texture is open");
    saveTo(output);
}

void TextureDocument::saveTo(const std::filesystem::path& output) {
    if (output.empty()) throw neotpc::texture::TextureError("Texture output path is empty");
    neotpc::texture::saveTexture(texture_, output, options_);
    texture_ = neotpc::texture::loadTexture(output);
    path_ = output;
    dirty_ = false;
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
        << "JPEG quality: " << static_cast<unsigned>(options_.jpegQuality) << '\n'
        << '\n' << neotpc::texture::txiValidationReport(texture_.txi);
    return out.str();
}

} // namespace neotpc
