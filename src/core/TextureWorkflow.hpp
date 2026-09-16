#pragma once

#include "texture/Image.hpp"
#include "texture/FileUtil.hpp"

#include <filesystem>
#include <string>

namespace neotpc::workflow {

// Use the detected container, not a possibly absent or misleading filename suffix.
inline std::string formatExtension(texture::TextureFileKind kind) {
    using texture::TextureFileKind;
    switch (kind) {
    case TextureFileKind::Tga: return "tga";
    case TextureFileKind::Tpc: return "tpc";
    case TextureFileKind::Txb: return "txb";
    case TextureFileKind::Dds: return "dds";
    case TextureFileKind::Png: return "png";
    case TextureFileKind::Jpeg: return "jpg";
    case TextureFileKind::Bmp: return "bmp";
    case TextureFileKind::Txi: return "txi";
    default: return {};
    }
}

inline bool writableFormat(texture::TextureFileKind kind) {
    return kind != texture::TextureFileKind::Unknown && kind != texture::TextureFileKind::Txb;
}

inline std::string defaultExportExtension(texture::TextureFileKind kind) {
    return writableFormat(kind) ? formatExtension(kind) : "tpc";
}

inline std::string preserveFormatExtension(texture::TextureFileKind kind,
                                           const std::filesystem::path& source) {
    // Preserve legitimate aliases (.jpeg, .jpe), but repair misleading extensions.
    return texture::kindForExtension(source) == kind
        ? texture::extensionLower(source) : formatExtension(kind);
}

inline bool canSaveInPlace(texture::TextureFileKind kind, const std::filesystem::path& source) {
    return writableFormat(kind) && texture::kindForExtension(source) == kind;
}

} // namespace neotpc::workflow
