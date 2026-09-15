#pragma once

#include "texture/Image.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neotpc {
struct TexturePreview {
    texture::EncodedTexture encoded;
    texture::TextureData decoded;
    texture::TextureFileKind outputKind = texture::TextureFileKind::Unknown;
    texture::TextureSaveOptions options;
    std::uint64_t revision = 0;
    bool pixelsPreserved = false;
};

class TextureDocument {
public:
    void open(const std::filesystem::path& path);
    void close() noexcept;
    // Used only after an acknowledged write by this application. Failed loads
    // retain the entire current document; unchanged bytes retain pending edits.
    bool reloadIfSourceChanged();
    bool isOpen() const noexcept { return open_; }
    bool dirty() const noexcept { return txiDirty_ || contentDirty_ || optionsDirty_; }
    bool txiDirty() const noexcept { return txiDirty_; }
    bool contentDirty() const noexcept { return contentDirty_; }
    bool optionsDirty() const noexcept { return optionsDirty_; }
    bool canPatchEmbeddedTxi() const noexcept;
    const std::filesystem::path& path() const noexcept { return path_; }
    const texture::TextureData& texture() const noexcept { return texture_; }
    const texture::TextureSaveOptions& saveOptions() const noexcept { return options_; }
    std::uint64_t revision() const noexcept { return revision_; }
    std::uint64_t pixelRevision() const noexcept { return pixelRevision_; }

    void setSaveOptions(const texture::TextureSaveOptions& options);
    void setTxi(std::string txi);
    void setAlpha(std::uint8_t value);
    void scaleAlpha(double factor);
    void invertAlpha();
    void flipHorizontal();
    void flipVertical();
    bool layoutPending() const;

    bool canUndo() const noexcept { return !undo_.empty(); }
    bool canRedo() const noexcept { return !redo_.empty(); }
    void undo();
    void redo();
    void finishEditGroup() noexcept { editGroup_.clear(); }
    std::string undoLabel() const;
    std::string redoLabel() const;

    // Includes the image, present/absent sidecar and filesystem aliases.
    bool outputTouchesSource(const std::filesystem::path& output) const;
    void validateExportDestination(const std::filesystem::path& output) const;
    std::string outputIssue(const std::filesystem::path& output, const texture::TextureSaveOptions& options) const;
    bool outputPreservesImage(const std::filesystem::path& output, const texture::TextureSaveOptions& options) const;
    TexturePreview preview(const std::filesystem::path& output, const texture::TextureSaveOptions& options) const;
    // Commit exactly the staged bytes. A stale preview cannot overwrite a file.
    // adopt=false is an export: it leaves this document and its saved state alone.
    void commitPreview(const std::filesystem::path& output, const TexturePreview& preview, bool adopt = false);
    void save();
    void saveAs(const std::filesystem::path& output);
    std::string summary() const;
    std::string exportSummary(const std::filesystem::path& output, const texture::TextureSaveOptions& options,
                              const TexturePreview* staged = nullptr) const;

private:
    struct Snapshot {
        std::shared_ptr<const texture::TextureData> pixels;
        std::string txi;
        texture::TextureSaveOptions options;
        std::uint64_t pixelRevision = 0;
        std::string label;
    };
    Snapshot snapshot(const std::string& label);
    void beforeEdit(const std::string& label, bool coalesce = false);
    void restore(const Snapshot& state, texture::TextureData prepared);
    void trimHistory();
    void editPixels(const std::string& label, const std::function<void(texture::TextureData&)>& edit);
    void refreshDirty();
    void resetSavedState();
    void saveTo(const std::filesystem::path& output);
    void requireSourceUnchanged() const;
    std::vector<std::uint8_t> sourceBytes_;
    std::optional<std::vector<std::uint8_t>> sidecarBytes_;
    bool open_ = false;
    bool txiDirty_ = false;
    bool contentDirty_ = false;
    bool optionsDirty_ = false;
    std::filesystem::path path_;
    texture::TextureData texture_;
    texture::TextureSaveOptions options_;
    std::string savedTxi_;
    texture::TextureSaveOptions savedOptions_;
    std::uint64_t revision_ = 0;
    std::uint64_t pixelRevision_ = 0;
    std::uint64_t nextPixelRevision_ = 0;
    std::uint64_t savedPixelRevision_ = 0;
    // Consecutive text/options edits share pixels; pixels are copied only on
    // a pixel action or restore. No whole-image copy per TXI keystroke.
    std::shared_ptr<const texture::TextureData> historyPixels_;
    std::vector<Snapshot> undo_, redo_;
    std::string editGroup_;
};
} // namespace neotpc
