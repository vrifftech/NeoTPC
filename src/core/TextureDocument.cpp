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
bool sameLayerPixels(const texture::TextureLayer& a, const texture::TextureLayer& b) {
    texture::checkOperation();
    if (a.width != b.width || a.height != b.height || a.rgba != b.rgba || a.mipmaps.size() != b.mipmaps.size()) return false;
    for (std::size_t i = 0; i < a.mipmaps.size(); ++i)
        if (!sameLayerPixels(a.mipmaps[i], b.mipmaps[i])) return false;
    return true;
}
bool samePixels(const texture::TextureData& a, const texture::TextureData& b) {
    if (a.canvasWidth != b.canvasWidth || a.canvasHeight != b.canvasHeight || a.layers.size() != b.layers.size() ||
        a.cubeMap != b.cubeMap || a.animated != b.animated || a.cubeFaces != b.cubeFaces) return false;
    for (std::size_t i = 0; i < a.layers.size(); ++i)
        if (!sameLayerPixels(a.layers[i], b.layers[i])) return false;
    return true;
}
std::filesystem::path sidecarPath(const std::filesystem::path& path) {
    if (const auto existing = texture::findTxiSidecar(path)) return *existing;
    auto result = path; result.replace_extension(".txi"); return result;
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

} // namespace

void TextureDocument::open(const std::filesystem::path& path) {
    auto bytes = texture::readFileBytes(path);
    auto sidecar = readSidecar(path);
    auto loaded = texture::loadTextureBytes(bytes, path, sidecar ? std::string(sidecar->begin(), sidecar->end()) : std::string());
    sourceBytes_ = std::move(bytes);
    sidecarBytes_ = std::move(sidecar);
    texture_ = std::move(loaded);
    path_ = path;
    options_ = texture::importedTextureOptions(texture_);
    open_ = true;
    ++revision_;
    pixelRevision_ = ++nextPixelRevision_;
    undo_.clear(); redo_.clear(); historyPixels_.reset(); editGroup_.clear();
    resetSavedState();
}
bool TextureDocument::reloadIfSourceChanged() {
    if (!open_) return false;
    if (texture::readFileBytes(path_) == sourceBytes_ && readSidecar(path_) == sidecarBytes_) return false;
    const auto sourcePath = path_;
    open(sourcePath);
    return true;
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
    txiDirty_ = texture_.txi != savedTxi_;
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
    if (samePixels(texture_, next)) return; // Do not dirty/recompress a no-op.
    beforeEdit(label);
    texture_ = std::move(next);
    historyPixels_.reset();
    pixelRevision_ = ++nextPixelRevision_;
    ++revision_;
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

bool TextureDocument::outputTouchesSource(const std::filesystem::path& output) const {
    if (!open_ || output.empty()) return false;
    std::vector<std::filesystem::path> sources{path_}, outputs{output};
    if (texture::usesTxiSidecar(path_)) sources.push_back(sidecarPath(path_));
    if (texture::usesTxiSidecar(output)) outputs.push_back(sidecarPath(output));
    for (const auto& to : outputs) for (const auto& from : sources)
        if (samePath(to, from)) return true;
    return false;
}
void TextureDocument::validateExportDestination(const std::filesystem::path& output) const {
    if (!open_ || output.empty()) throw texture::TextureError("Choose an open texture and an export destination.");
    if (outputTouchesSource(output))
        throw texture::TextureError("Export would change the open source image or its TXI: " + texture::pathToUtf8(path_) +
            ". Choose a separate export folder, or use Save to update this document.");
}

std::string TextureDocument::outputIssue(const std::filesystem::path& output, const texture::TextureSaveOptions& o) const {
    using namespace texture;
    if (!open_) return "Open a texture first.";
    const auto kind = kindForExtension(output);
    if (kind == TextureFileKind::Unknown || kind == TextureFileKind::Txb) return "Choose a supported output format.";
    if (kind == TextureFileKind::Txi) return {};
    if (!texture_.hasPixels()) return "This document contains TXI only; it has no image pixels to export.";
    const bool preserved = kind == texture_.kind && !contentDirty_ && !txiDirty_ && sameEncodingOptions(o, savedOptions_, kind, texture_);
    if (preserved) return {}; // Copying is not a claim that an old file is compatible.
    return texture::textureOutputIssue(texture_, output, o);

}

bool TextureDocument::outputPreservesImage(const std::filesystem::path& output, const texture::TextureSaveOptions& options) const {
    return open_ && texture::kindForExtension(output) == texture_.kind && !contentDirty_ &&
        texture::sameEncodingOptions(options, savedOptions_, texture_.kind, texture_);
}
TexturePreview TextureDocument::preview(const std::filesystem::path& output, const texture::TextureSaveOptions& options) const {
    using namespace texture;
    if (!open_ || output.empty()) throw TextureError("No texture or output format selected");
    TexturePreview result;
    result.outputKind = kindForExtension(output); result.options = options; result.revision = revision_;
    const bool reencode = !outputPreservesImage(output, options);
    if (!reencode && !txiDirty_) {
        result.encoded = {sourceBytes_, sidecarBytes_}; result.pixelsPreserved = true;
    } else if (!reencode && texture_.kind == TextureFileKind::Tpc) {
        result.encoded.image = patchTpcTxiBytes(sourceBytes_, texture_.txi, true); result.pixelsPreserved = true;
    } else if (!reencode && usesTxiSidecar(output)) {
        result.encoded.image = sourceBytes_;
        if (!texture_.txi.empty()) result.encoded.sidecar.emplace(texture_.txi.begin(), texture_.txi.end());
        result.pixelsPreserved = true;
    } else result.encoded = encodeTexture(texture_, output, options);
    result.decoded = loadTextureBytes(result.encoded.image, output,
        result.encoded.sidecar ? std::string(result.encoded.sidecar->begin(), result.encoded.sidecar->end()) : std::string());
    if (reencode && result.outputKind == TextureFileKind::Tpc) {
        auto exactTxi = texture_.txi;
        for (const auto& entry : parseTxiEntries(result.decoded.txi)) {
            if (getTxiValue(exactTxi, entry.key) != getTxiValue(result.decoded.txi, entry.key))
                exactTxi = setTxiValue(exactTxi, entry.key, entry.value);
        }
        if (exactTxi != result.decoded.txi) {
            result.encoded.image = patchTpcTxiBytes(result.encoded.image, exactTxi, true);
            result.decoded.txi = std::move(exactTxi);
        }
    }
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
    if (!adopt || !sameSource) validateExportDestination(output);
    const bool bytesUnchanged = sameSource && staged.encoded.image == sourceBytes_ &&
        staged.encoded.sidecar == sidecarBytes_;
    // Decode/copy before replacing the file, so allocation/parse errors cannot
    // leave a saved file with an unreported invalid editor state.
    if(!adopt){saveEncodedTexture(staged.encoded.image,staged.encoded.sidecar,output);return;}
    auto next = staged.decoded;
    auto bytes = staged.encoded.image;
    auto sidecar = staged.encoded.sidecar;
    const bool pixelsUnchanged = samePixels(texture_, next);
    if (!bytesUnchanged) saveEncodedTexture(bytes, sidecar, output);
    sourceBytes_ = std::move(bytes); sidecarBytes_ = std::move(sidecar);
    texture_ = std::move(next); texture_.sourcePath = output; path_ = output;
    options_ = texture::importedTextureOptions(texture_);
    options_.dxtQuality = staged.options.dxtQuality; options_.dxtMetric = staged.options.dxtMetric;
    options_.weightColorByAlpha = staged.options.weightColorByAlpha; options_.dxt1AlphaThreshold = staged.options.dxt1AlphaThreshold;
    options_.jpegQuality = staged.options.jpegQuality;
    historyPixels_.reset();
    if (!pixelsUnchanged) pixelRevision_ = ++nextPixelRevision_;
    ++revision_;
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
std::string TextureDocument::exportSummary(const std::filesystem::path& output, const texture::TextureSaveOptions& opts,
                                            const TexturePreview* staged) const {
    if (!open_) return "Open a texture first.";
    const auto kind = texture::kindForExtension(output);
    const auto issue = outputIssue(output, opts);
    const bool stagedValid = staged && staged->revision == revision() && staged->outputKind == kind &&
        texture::sameEncodingOptions(staged->options, opts, kind, texture());
    const bool preserved = outputPreservesImage(output, opts);
    const auto* result = stagedValid ? &staged->decoded : (preserved ? &texture() : nullptr);
    std::string text = "Output: " + texture::pathToUtf8(output) + "\n";
    if (kind == texture::TextureFileKind::Txi) text += "TXI — metadata only";
    else if (kind == texture::TextureFileKind::Dds) {
        const bool standard = result ? result->ddsDialect == texture::DdsDialect::Standard :
            opts.ddsDialect == texture::DdsDialect::Standard ||
            (opts.ddsDialect == texture::DdsDialect::Auto && texture().ddsDialect == texture::DdsDialect::Standard);
        text += standard ? "Standard DDS" : "Game DDS";
    } else text += texture::textureFileKindToString(kind);
    if (kind == texture::TextureFileKind::Tpc || kind == texture::TextureFileKind::Dds) {
        if (result) {
            text += " | " + texture::textureCompressionToString(result->preferredCompression);
            text += " | " + std::to_string(result->sourceMipMapCount) + " mip level(s)";
        } else {
            text += " | " + texture::textureCompressionToString(opts.compression);
            text += "\nMipmaps: ";
            text += !texture::storesMipmaps(opts) ? "Base only" : opts.mipmapPolicy == texture::MipmapPolicy::Rebuild ? "Rebuild" : "Preserve existing / create if absent";
        }
    } else if (kind != texture::TextureFileKind::Txi) {
        text += result ? " | " + result->sourceEncoding : (kind == texture::TextureFileKind::Jpeg ? " | JPEG (lossy)" : " | Uncompressed/lossless image");
        text += " | base image only";
    }
    if (texture::usesTxiSidecar(output)) {
        auto txi = output; txi.replace_extension(".txi");
        text += "\nTXI companion";
        if (stagedValid) text += staged->encoded.sidecar ? " (will write): " : " (absent; stale output TXI removed): ";
        else text += " (when present): ";
        text += texture::pathToUtf8(txi);
    }
    if (!issue.empty()) text += "\n" + issue;
    else if (preserved) text += txiDirty() ? "\nEncoded image preserved — metadata updated." : "\nExact encoded copy — no recompression. Existing compatibility warnings are unchanged.";
    else if (stagedValid) text += "\nEncoded preview ready. Export writes these staged bytes.";
    else text += "\nReady for encoded preview; the encoder checks the complete layout before writing.";
    if (layoutPending()) text += "\nLayout changes pending: the current image keeps its loaded layout. Preview encoded output to inspect the proposed layout.";
    if (stagedValid) {
        text += "\nPreview: " + std::to_string(staged->encoded.image.size()) + " bytes";
        if (staged->encoded.sidecar) text += "; TXI " + std::to_string(staged->encoded.sidecar->size()) + " bytes";
    }
    if (result) for (const auto& warning : result->compatibilityWarnings) text += "\n" + warning;
    return text;
}
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
