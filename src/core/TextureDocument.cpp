#include "core/TextureDocument.hpp"
#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"
#include "texture/Txi.hpp"
#include "texture/Operation.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
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

std::optional<std::vector<std::uint8_t>> readSidecar(const std::filesystem::path& path) {
    if (!texture::usesTxiSidecar(path)) return {};
    if (auto sidecar = texture::findTxiSidecar(path)) return texture::readFileBytes(*sidecar);
    return {};
}
bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec) return true;
    return texture::canonicalPathKey(a) == texture::canonicalPathKey(b);
}
std::size_t pixelBytes(const texture::TextureData& data) {
    std::size_t n = 0;
    for (const auto& layer : data.layers) {
        n += layer.rgba.size();
        for (const auto& mip : layer.mipmaps) n += mip.rgba.size();
    }
    return n;
}
bool sameAllOptions(const texture::TextureSaveOptions& a, const texture::TextureSaveOptions& b) {
    return a.compression == b.compression && a.generateMipmaps == b.generateMipmaps &&
        a.bicubicMipmaps == b.bicubicMipmaps && a.mipmapPolicy == b.mipmapPolicy &&
        a.mipmapAlpha == b.mipmapAlpha && a.mipmapColor == b.mipmapColor &&
        a.flipXOnSave == b.flipXOnSave && a.flipYOnSave == b.flipYOnSave &&
        a.alphaBlending == b.alphaBlending && a.ddsDialect == b.ddsDialect &&
        a.dxtQuality == b.dxtQuality && a.dxtMetric == b.dxtMetric &&
        a.weightColorByAlpha == b.weightColorByAlpha && a.dxt1AlphaThreshold == b.dxt1AlphaThreshold &&
        a.jpegQuality == b.jpegQuality;
}
texture::TextureSaveOptions importedOptions(const texture::TextureData& data) {
    texture::TextureSaveOptions o;
    o.compression = data.preferredCompression == texture::TextureCompression::SwizzledBgra
        ? texture::TextureCompression::Auto : data.preferredCompression;
    o.generateMipmaps = (data.kind == texture::TextureFileKind::Tpc || data.kind == texture::TextureFileKind::Dds ||
                         data.kind == texture::TextureFileKind::Txb) ? data.sourceMipMapCount > 1 : true;
    o.mipmapPolicy = o.generateMipmaps ? texture::MipmapPolicy::Preserve : texture::MipmapPolicy::BaseOnly;
    o.alphaBlending = data.alphaBlending;
    return o;
}
} // namespace

void TextureDocument::open(const std::filesystem::path& path) {
    auto bytes = texture::readFileBytes(path);
    auto sidecar = readSidecar(path);
    auto loaded = texture::loadTextureBytes(bytes, path, sidecar ? std::string(sidecar->begin(), sidecar->end()) : std::string());
    sourceBytes_ = std::move(bytes);
    sidecarBytes_ = std::move(sidecar);
    texture_ = std::move(loaded);
    path_ = path;
    options_ = importedOptions(texture_);
    open_ = true;
    ++revision_;
    pixelRevision_ = ++nextPixelRevision_;
    undo_.clear(); redo_.clear(); historyPixels_.reset(); editGroup_.clear();
    resetSavedState();
}
void TextureDocument::close() noexcept {
    open_ = false; txiDirty_ = contentDirty_ = optionsDirty_ = false;
    path_.clear(); texture_ = {}; options_ = {}; savedTxi_.clear(); savedOptions_ = {};
    sourceBytes_.clear(); sidecarBytes_.reset(); undo_.clear(); redo_.clear(); historyPixels_.reset(); editGroup_.clear();
    ++revision_;
}
bool TextureDocument::canPatchEmbeddedTxi() const noexcept {
    return open_ && texture_.kind == texture::TextureFileKind::Tpc && txiDirty_ && !contentDirty_ && !optionsDirty_;
}
TextureDocument::Snapshot TextureDocument::snapshot(const std::string& label) {
    if (!historyPixels_) historyPixels_ = std::make_shared<const texture::TextureData>(texture_);
    return {historyPixels_, texture_.txi, options_, pixelRevision_, label};
}
void TextureDocument::trimHistory() {
    constexpr std::size_t maxSteps = 32, budget = 128u * 1024 * 1024;
    const auto bytes = [&] {
        std::set<const texture::TextureData*> distinct;
        std::size_t n = 0;
        for (const auto* history : {&undo_, &redo_}) for (const auto& state : *history)
            if (distinct.insert(state.pixels.get()).second) n += pixelBytes(*state.pixels);
        return n;
    };
    // Retain one undo for a large image even if that image alone exceeds budget.
    while (undo_.size() > 1 && (undo_.size() > maxSteps || bytes() > budget)) undo_.erase(undo_.begin());
    while (redo_.size() > 1 && (redo_.size() > maxSteps || bytes() > budget)) redo_.erase(redo_.begin());
}
void TextureDocument::beforeEdit(const std::string& label, bool coalesce) {
    if (!coalesce || editGroup_ != label || undo_.empty() || !redo_.empty()) undo_.push_back(snapshot(label));
    redo_.clear(); editGroup_ = coalesce ? label : std::string();
    trimHistory();
}
void TextureDocument::refreshDirty() {
    txiDirty_ = canonicalTxiFooter(texture_.txi) != canonicalTxiFooter(savedTxi_);
    contentDirty_ = pixelRevision_ != savedPixelRevision_;
    optionsDirty_ = !texture::sameEncodingOptions(options_, savedOptions_, texture_.kind, texture_);
}
void TextureDocument::resetSavedState() {
    savedTxi_ = texture_.txi; savedOptions_ = options_; savedPixelRevision_ = pixelRevision_;
    txiDirty_ = contentDirty_ = optionsDirty_ = false;
}
void TextureDocument::setSaveOptions(const texture::TextureSaveOptions& options) {
    if (!open_ || sameAllOptions(options_, options)) return;
    beforeEdit("Encoding settings", true);
    options_ = options; ++revision_; refreshDirty();
}
void TextureDocument::setTxi(std::string txi) {
    if (!open_ || texture_.txi == txi) return;
    beforeEdit("TXI edit", true);
    texture_.txi = std::move(txi); ++revision_; refreshDirty();
}
void TextureDocument::editPixels(const std::string& label, const std::function<void(texture::TextureData&)>& edit) {
    if (!open_ || !texture_.hasPixels()) return;
    // Build the replacement before recording/mutating; cancellation or allocation
    // failure leaves both the document and its history unchanged.
    auto next = texture_;
    edit(next);
    beforeEdit(label);
    texture_ = std::move(next);
    historyPixels_.reset(); pixelRevision_ = ++nextPixelRevision_; ++revision_;
    refreshDirty();
}
void TextureDocument::setAlpha(std::uint8_t value) { editPixels("Set alpha", [=](auto& t) { texture::setTextureAlpha(t, value); }); }
void TextureDocument::scaleAlpha(double factor) { editPixels("Scale alpha", [=](auto& t) { texture::scaleTextureAlpha(t, factor); }); }
void TextureDocument::invertAlpha() { editPixels("Invert alpha", [](auto& t) { texture::invertTextureAlpha(t); }); }
void TextureDocument::flipHorizontal() { editPixels("Flip horizontal", [](auto& t) { texture::flipTextureHorizontal(t); }); }
void TextureDocument::flipVertical() { editPixels("Flip vertical", [](auto& t) { texture::flipTextureVertical(t); }); }
void TextureDocument::restore(const Snapshot& state, texture::TextureData prepared) {
    texture_ = std::move(prepared); options_ = state.options; pixelRevision_ = state.pixelRevision;
    historyPixels_ = state.pixels; editGroup_.clear(); ++revision_; refreshDirty();
}
void TextureDocument::undo() {
    if (!canUndo()) return;
    const auto state = undo_.back();
    auto next=*state.pixels; next.txi=state.txi;
    redo_.push_back(snapshot(state.label)); undo_.pop_back(); restore(state,std::move(next)); trimHistory();
}
void TextureDocument::redo() {
    if (!canRedo()) return;
    const auto state = redo_.back();
    auto next=*state.pixels; next.txi=state.txi;
    undo_.push_back(snapshot(state.label)); redo_.pop_back(); restore(state,std::move(next)); trimHistory();
}
std::string TextureDocument::undoLabel() const { return canUndo() ? undo_.back().label : std::string(); }
std::string TextureDocument::redoLabel() const { return canRedo() ? redo_.back().label : std::string(); }
bool TextureDocument::layoutPending() const {
    if (!open_) return false;
    for (const char* key : {"cube", "proceduretype", "numx", "numy", "defaultwidth", "defaultheight"})
        if (texture::getTxiValue(texture_.txi, key) != texture::getTxiValue(savedTxi_, key)) return true;
    return false;
}
void TextureDocument::requireSourceUnchanged() const {
    if (texture::readFileBytes(path_) != sourceBytes_ || readSidecar(path_) != sidecarBytes_)
        throw texture::TextureError("This texture or its TXI changed on disk after opening. Reopen it or export to another name; external changes were not overwritten.");
}

TexturePreview TextureDocument::preview(const std::filesystem::path& output, const texture::TextureSaveOptions& options) const {
    using namespace texture;
    if (!open_ || output.empty()) throw TextureError("No texture or output format selected");
    TexturePreview result;
    result.outputKind = kindForExtension(output); result.options = options; result.revision = revision_;
    const bool sameFormat = result.outputKind == texture_.kind;
    const bool reencode = !sameFormat || contentDirty_ || !sameEncodingOptions(options, savedOptions_, result.outputKind, texture_);
    if (!reencode && !txiDirty_) {
        result.encoded = {sourceBytes_, sidecarBytes_}; result.pixelsPreserved = true;
    } else if (!reencode && texture_.kind == TextureFileKind::Tpc) {
        result.encoded.image = patchTpcTxiBytes(sourceBytes_, texture_.txi); result.pixelsPreserved = true;
    } else if (!reencode && usesTxiSidecar(output)) {
        result.encoded.image = sourceBytes_;
        if (!texture_.txi.empty()) result.encoded.sidecar.emplace(texture_.txi.begin(), texture_.txi.end());
        result.pixelsPreserved = true;
    } else result.encoded = encodeTexture(texture_, output, options);
    result.decoded = loadTextureBytes(result.encoded.image, output,
        result.encoded.sidecar ? std::string(result.encoded.sidecar->begin(), result.encoded.sidecar->end()) : std::string());
    if (txiDirty_ && result.pixelsPreserved && result.decoded.ddsDialect == DdsDialect::Game && !result.decoded.compatibilityWarnings.empty())
        throw TextureError(result.decoded.compatibilityWarnings.front());
    return result;
}
void TextureDocument::commitPreview(const std::filesystem::path& output, const TexturePreview& staged, bool adopt) {
    using namespace texture;
    if (!open_ || staged.revision != revision_ || staged.outputKind != kindForExtension(output))
        throw TextureError("This encoded preview is stale. Preview the current document and output settings again before saving.");
    const bool sameSource = samePath(output, path_);
    if (sameSource) requireSourceUnchanged();
    if (!adopt && sameSource) throw TextureError("Export cannot silently replace the open source. Use Save/Save As, or choose a different export name.");
    if (sameSource && staged.encoded.image == sourceBytes_ && staged.encoded.sidecar == sidecarBytes_) {
        finishEditGroup(); return;
    }
    // Decode/copy before replacing the file, so allocation/parse errors cannot
    // leave a saved file with an unreported invalid editor state.
    if(!adopt){saveEncodedTexture(staged.encoded.image,staged.encoded.sidecar,output);return;}
    auto next = staged.decoded;
    auto bytes = staged.encoded.image;
    auto sidecar = staged.encoded.sidecar;
    saveEncodedTexture(bytes, sidecar, output);
    sourceBytes_ = std::move(bytes); sidecarBytes_ = std::move(sidecar);
    texture_ = std::move(next); texture_.sourcePath = output; path_ = output;
    options_ = importedOptions(texture_);
    options_.dxtQuality = staged.options.dxtQuality; options_.dxtMetric = staged.options.dxtMetric;
    options_.weightColorByAlpha = staged.options.weightColorByAlpha; options_.dxt1AlphaThreshold = staged.options.dxt1AlphaThreshold;
    options_.jpegQuality = staged.options.jpegQuality;
    historyPixels_.reset(); pixelRevision_ = ++nextPixelRevision_; ++revision_;
    finishEditGroup(); resetSavedState();
}
void TextureDocument::saveTo(const std::filesystem::path& output) {
    if (!open_ || output.empty()) throw texture::TextureError("No texture or output path selected");
    if (samePath(output, path_)) {
        requireSourceUnchanged();
        if (!dirty()) { finishEditGroup(); return; }
    }
    const auto staged = preview(output, options_);
    commitPreview(output, staged, true);
}
void TextureDocument::save() { saveTo(path_); }
void TextureDocument::saveAs(const std::filesystem::path& output) { saveTo(output); }
std::string TextureDocument::summary() const {
    if (!open_) return "No texture is open.\n";
    std::ostringstream out;
    out << texture::textureSummary(texture_);
    if (layoutPending()) out << "\nLAYOUT CHANGES PENDING: the left preview shows the loaded layout. Use Preview output to validate the proposed layout.\n";
    if (!dirty()) out << "\nSave preserves the original encoded bytes. Export settings are separate.\n";
    else if (canPatchEmbeddedTxi()) out << "\nTXI-only save preserves the encoded image.\n";
    else if (contentDirty_ || optionsDirty_) out << "\nSaving requires an image encode; compressed formats may change pixels.\n";
    return out.str();
}
} // namespace neotpc
