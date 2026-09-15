#pragma once

#include "texture/Image.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
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
    // Empty means all supported inputs. Filtering never removes source protection.
    std::string inputExtension;
    // Same container/dialect: exact source copy, not an implicit bulk rebuild.
    // Set false to apply encoding options to these inputs too.
    bool preserveMatchingFormat = true;
};

struct BatchItemResult {
    std::filesystem::path input;
    std::filesystem::path output;
    BatchItemStatus status = BatchItemStatus::Failed;
    std::string message;
    bool discardsAlpha = false;
    bool preservesSource = false;
    // True only after a successful image/TXI commit, never for an exact in-place no-op.
    bool wroteOutput = false;
};

// Bounded per-input review data: no retained pixels or encoded payloads.
struct BatchInputReview {
    std::string fingerprint;
    std::string message;
    bool compatible = false;
    bool discardsAlpha = false;
    bool preservesSource = false;
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
    // Exclusion removes output claims, never protected input dependencies.
    std::vector<std::filesystem::path> excludedInputs;
    std::map<std::filesystem::path, BatchInputReview> reviewedInputs;
    std::string reviewedSettings;
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
bool batchInputRequested(const std::filesystem::path& path, const BatchOptions& options,
                         const std::vector<std::filesystem::path>& excludedInputs = {});
struct BatchEncoding {
    EncodedTexture encoded;
    bool preserved = false;
    bool discardedAlpha = false;
};
// The same exact-copy/encode decision for native and retained browser inputs.
BatchEncoding encodeBatchTexture(const std::vector<std::uint8_t>& bytes,
                                 const std::optional<std::vector<std::uint8_t>>& sidecar,
                                 const std::filesystem::path& input,
                                 const std::filesystem::path& output,
                                 const BatchOptions& options);

BatchPlan planTextureBatch(const std::filesystem::path& inputDirectory,
                           const std::filesystem::path& outputDirectory,
                           const BatchOptions& options,
                           const std::vector<std::filesystem::path>& excludedInputs = {});
// Changes only the selection/output claims. Already-reviewed inputs are not
// decoded again; Convert verifies their content fingerprints before any write.
BatchPlan replanTextureBatch(const BatchPlan& previous,
                             const std::vector<std::filesystem::path>& excludedInputs);
std::vector<std::filesystem::path> batchOutputMembers(const std::filesystem::path& output);
bool batchItemWritesOutput(const BatchItemResult& item);
BatchReport executeTextureBatch(const BatchPlan& plan, const BatchProgress& progress = {});

BatchReport batchConvertTextures(const std::filesystem::path& inputDirectory,
                                 const std::filesystem::path& outputDirectory,
                                 const BatchOptions& options,
                                 const BatchProgress& progress = {});

} // namespace neotpc::texture
