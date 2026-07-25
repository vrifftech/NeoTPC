#pragma once

#include "texture/Image.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace neotpc::texture {

enum class BatchItemStatus {
    Converted,
    Skipped,
    Failed,
};

struct BatchOptions {
    std::string outputExtension = "tpc";
    bool recursive = true;
    bool overwrite = false;
    TextureSaveOptions saveOptions;
};

struct BatchItemResult {
    std::filesystem::path input;
    std::filesystem::path output;
    BatchItemStatus status = BatchItemStatus::Failed;
    std::string message;
};

struct BatchReport {
    std::size_t discovered = 0;
    std::size_t converted = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    std::vector<BatchItemResult> items;

    bool ok() const noexcept { return failed == 0 && !cancelled; }
    std::string summary() const;
};

// Return false from the callback to cancel before the current file is encoded.
using BatchProgress = std::function<bool(std::size_t current,
                                         std::size_t total,
                                         const std::filesystem::path& input)>;

bool isSupportedTexturePath(const std::filesystem::path& path);

BatchReport batchConvertTextures(const std::filesystem::path& inputDirectory,
                                 const std::filesystem::path& outputDirectory,
                                 const BatchOptions& options,
                                 const BatchProgress& progress = {});

} // namespace neotpc::texture
