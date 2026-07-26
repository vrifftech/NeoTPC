#pragma once

#include "texture/Image.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace neotpc {

class TextureDocument {
public:
    void open(const std::filesystem::path& path);
    void close() noexcept;

    bool isOpen() const noexcept { return open_; }
    bool dirty() const noexcept { return txiDirty_ || contentDirty_ || optionsDirty_; }
    bool txiDirty() const noexcept { return txiDirty_; }
    bool contentDirty() const noexcept { return contentDirty_; }
    bool optionsDirty() const noexcept { return optionsDirty_; }
    bool canPatchEmbeddedTxi() const noexcept;
    const std::filesystem::path& path() const noexcept { return path_; }
    const neotpc::texture::TextureData& texture() const noexcept { return texture_; }
    const neotpc::texture::TextureSaveOptions& saveOptions() const noexcept { return options_; }

    void setSaveOptions(const neotpc::texture::TextureSaveOptions& options);
    void setTxi(std::string txi);
    void setAlpha(std::uint8_t value);
    void scaleAlpha(double factor);
    void invertAlpha();
    void flipHorizontal();
    void flipVertical();

    void save();
    void saveAs(const std::filesystem::path& output);
    std::string summary() const;

private:
    void resetSavedState();
    void saveTo(const std::filesystem::path& output);

    bool open_ = false;
    bool txiDirty_ = false;
    bool contentDirty_ = false;
    bool optionsDirty_ = false;
    std::filesystem::path path_;
    neotpc::texture::TextureData texture_;
    neotpc::texture::TextureSaveOptions options_;
    std::string savedTxi_;
    neotpc::texture::TextureSaveOptions savedOptions_;
};

} // namespace neotpc
