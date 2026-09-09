#pragma once

#include "texture/Image.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace neotpc::texture {

enum class BatchItemStatus {
    Ready,
    Conflict,
    Converted,
    Skipped,
    Failed,
};

struct BatchOptions {
    std::string outputExtension = "tpc";
    bool recursive = true;
    bool overwrite = false;
    // Explicit consent to process independent rows when a preflight has conflicts.
    bool skipConflicts = false;
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
    std::size_t conflicts = 0;
    bool cancelled = false;
    std::vector<BatchItemResult> items;

    bool ok() const noexcept { return failed == 0 && conflicts == 0 && !cancelled; }
    std::string summary() const;
};

struct BatchPlan {
    std::filesystem::path inputDirectory;
    std::filesystem::path outputDirectory;
    BatchOptions options;
    std::vector<BatchItemResult> items;
    std::size_t ready() const noexcept;
    std::size_t conflicts() const noexcept;
    std::string summary() const;
};

// current is COMPLETED items, never the index about to be encoded.
// Return false to cancel. OperationScope additionally allows mid-file cancellation.
using BatchProgress = std::function<bool(std::size_t current,
                                         std::size_t total,
                                         const std::filesystem::path& input)>;

bool isSupportedTexturePath(const std::filesystem::path& path);

BatchPlan planTextureBatch(const std::filesystem::path& inputDirectory,
                           const std::filesystem::path& outputDirectory,
                           const BatchOptions& options);
BatchReport executeTextureBatch(const BatchPlan& plan, const BatchProgress& progress = {});

BatchReport batchConvertTextures(const std::filesystem::path& inputDirectory,
                                 const std::filesystem::path& outputDirectory,
                                 const BatchOptions& options,
                                 const BatchProgress& progress = {});

} // namespace neotpc::texture
