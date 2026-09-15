#pragma once

#include <neoshared/texture/FileUtil.hpp>

namespace neotpc::texture {
using neoshared::texture::asciiLower;
using neoshared::texture::pathToUtf8;
using neoshared::texture::genericPathToUtf8;
using neoshared::texture::extensionLower;
using neoshared::texture::findTxiSidecar;
using neoshared::texture::usesTxiSidecar;
using neoshared::texture::canonicalPathKey;
using neoshared::texture::readFileBytes;
using neoshared::texture::writeFileBytes;
using neoshared::texture::canonicalTextureConflictStem;
using neoshared::texture::findConflictingTexturePaths;

} // namespace neotpc::texture
