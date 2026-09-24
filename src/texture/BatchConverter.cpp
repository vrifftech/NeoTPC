#include "texture/BatchConverter.hpp"
#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"
#include "texture/Operation.hpp"

#include <algorithm>
#include <map>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace neotpc::texture {
namespace fs = std::filesystem;
namespace {
bool pixelExtension(const std::string& ext) {
    return ext == "tpc" || ext == "txb" || ext == "tga" || ext == "dds" ||
           ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "jpe" || ext == "bmp";
}
std::string normalizedExtension(std::string ext) {
    ext = asciiLower(std::move(ext));
    while (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    return ext == "jpeg" || ext == "jpe" ? "jpg" : ext;
}
const char* statusText(BatchItemStatus status) {
    switch (status) {
    case BatchItemStatus::Ready: return "ready";
    case BatchItemStatus::Conflict: return "held";
    case BatchItemStatus::Converted: return "converted";
    case BatchItemStatus::Skipped: return "skipped";
    case BatchItemStatus::Failed: return "failed";
    }
    return "failed";
}
std::string oneLine(std::string text) {
    for (auto& c : text) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return text;
}
bool safeRelativePath(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& component : path) {
        if (component == "..") return false;
    }
    return true;
}

fs::path lexicalRelativePath(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    const fs::path absolutePath = fs::absolute(path, ec).lexically_normal();
    if (ec) throw TextureError("Unable to resolve batch input path: " + ec.message());
    const fs::path absoluteRoot = fs::absolute(root, ec).lexically_normal();
    if (ec) throw TextureError("Unable to resolve batch input directory: " + ec.message());
    const fs::path relative = absolutePath.lexically_relative(absoluteRoot);
    if (!safeRelativePath(relative)) {
        throw TextureError("Batch input is outside the selected input directory: " + pathToUtf8(path));
    }
    return relative;
}

bool pathIsWithin(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty()) return candidate == root;
    return safeRelativePath(relative);
}

void requireContainedOutput(const fs::path& outputRoot, const fs::path& output) {
    // Batch paths are derived from already-relative input paths. Keep the
    // rooted-operation invariant without canonicalizing and enumerating the
    // same destination directory for every item.
    std::error_code ec;
    const fs::path absoluteRoot = fs::absolute(outputRoot, ec).lexically_normal();
    if (ec) throw TextureError("Unable to resolve batch output directory: " + ec.message());
    const fs::path absoluteOutput = fs::absolute(output, ec).lexically_normal();
    if (ec) throw TextureError("Unable to resolve batch output path: " + ec.message());
    if (!pathIsWithin(absoluteRoot, absoluteOutput)) {
        throw TextureError("Refusing batch output outside the selected output directory: " +
                           pathToUtf8(output));
    }
}

std::vector<fs::path> outputMembers(const fs::path& image) {
    std::vector<fs::path> out{image};
    if (usesTxiSidecar(image)) {
        auto sidecar = image;
        sidecar.replace_extension(".txi");
        out.push_back(std::move(sidecar));
    }
    return out;
}
void hold(BatchItemResult& row, const std::string& reason) {
    row.status = BatchItemStatus::Conflict;
    if (!row.message.empty()) row.message += " | ";
    row.message += reason;
}
std::string rowsText(const std::vector<BatchItemResult>& items) {
    std::ostringstream out;
    out << "status\tinput\toutput\tmessage\n";
    for (const auto& row : items)
        out << statusText(row.status) << '\t' << genericPathToUtf8(row.input) << '\t'
            << genericPathToUtf8(row.output) << '\t' << oneLine(row.message) << '\n';
    return out.str();
}
bool preserveBatchInput(const TextureData& image, const fs::path& output, const BatchOptions& options) {
    if (!options.preserveMatchingFormat || image.kind != kindForExtension(output)) return false;
    // An explicit DDS dialect conversion is not a same-representation copy.
    return image.kind != TextureFileKind::Dds || options.saveOptions.ddsDialect == DdsDialect::Auto ||
        image.ddsDialect == options.saveOptions.ddsDialect;
}
struct InputSnapshot {
    std::vector<std::uint8_t> bytes;
    std::optional<std::vector<std::uint8_t>> sidecar;
    std::optional<fs::path> sidecarPath;
};
InputSnapshot readInput(const fs::path& input) {
    InputSnapshot result{readFileBytes(input), {}, {}};
    if (usesTxiSidecar(input)) if (const auto sidecar = findTxiSidecar(input)) {
        result.sidecarPath = *sidecar; result.sidecar = readFileBytes(*sidecar);
    }
    return result;
}
std::string settingsKey(const BatchOptions& options) {
    const auto& o = options.saveOptions;
    std::ostringstream key; key << std::setprecision(17);
    key << normalizedExtension(options.outputExtension) << '|' << normalizedExtension(options.inputExtension)
        << '|' << options.recursive << '|' << options.overwrite << '|' << options.preserveMatchingFormat
        << '|' << static_cast<int>(o.compression) << '|' << o.generateMipmaps << '|' << o.bicubicMipmaps
        << '|' << static_cast<int>(o.mipmapPolicy) << '|' << static_cast<int>(o.mipmapAlpha)
        << '|' << static_cast<int>(o.mipmapColor) << '|' << o.flipXOnSave << '|' << o.flipYOnSave
        << '|' << static_cast<int>(o.ddsDialect) << '|' << static_cast<int>(o.dxtQuality)
        << '|' << static_cast<int>(o.dxtMetric) << '|' << o.weightColorByAlpha
        << '|' << static_cast<int>(o.dxt1AlphaThreshold) << '|' << static_cast<unsigned>(o.jpegQuality)
        << '|' << o.alphaBlending.has_value();
    if (o.alphaBlending) key << '|' << *o.alphaBlending;
    return key.str();
}
TextureData decodeInput(const InputSnapshot& source, const fs::path& path) {
    return loadTextureBytes(source.bytes, path,
        source.sidecar ? std::string(source.sidecar->begin(), source.sidecar->end()) : std::string());
}
std::string batchInputIssue(const TextureData& image, const fs::path& output, const BatchOptions& options) {
    return preserveBatchInput(image, output, options) ? std::string() : textureOutputIssue(image, output, options.saveOptions);
}
} // namespace

bool isSupportedTexturePath(const fs::path& path) {
    return pixelExtension(extensionLower(path)) || extensionLower(path) == "txi";
}
bool batchInputRequested(const fs::path& path, const BatchOptions& options,
                         const std::vector<fs::path>& excludedInputs) {
    if (!options.inputExtension.empty() && normalizedExtension(extensionLower(path)) != normalizedExtension(options.inputExtension)) return false;
    return std::find(excludedInputs.begin(), excludedInputs.end(), path) == excludedInputs.end();
}
BatchEncoding encodeBatchTexture(const std::vector<std::uint8_t>& bytes,
                                 const std::optional<std::vector<std::uint8_t>>& sidecar,
                                 const fs::path& input, const fs::path& output, const BatchOptions& options) {
    const auto image = loadTextureBytes(bytes, input, sidecar ? std::string(sidecar->begin(), sidecar->end()) : std::string());
    const auto issue = batchInputIssue(image, output, options);
    if (!issue.empty()) throw TextureError(issue);
    if (preserveBatchInput(image, output, options)) return {{bytes, sidecar}, true, false};
    return {encodeTexture(image, output, options.saveOptions), false,
        kindForExtension(output) == TextureFileKind::Jpeg && image.hasAlpha};
}
std::size_t BatchPlan::ready() const noexcept {
    return static_cast<std::size_t>(std::count_if(items.begin(), items.end(), [](const auto& row) { return row.status == BatchItemStatus::Ready; }));
}
std::size_t BatchPlan::conflicts() const noexcept {
    return static_cast<std::size_t>(std::count_if(items.begin(), items.end(), [](const auto& row) { return row.status == BatchItemStatus::Conflict; }));
}
std::string BatchPlan::summary() const {
    std::ostringstream out;
    out << "Preflight: " << ready() << " ready, " << conflicts() << " held, "
        << items.size() - ready() - conflicts() << " skipped. No output has been written.\n\n" << rowsText(items);
    return out.str();
}
std::string BatchReport::summary() const {
    std::ostringstream out;
    out << "Texture batch conversion\ndiscovered: " << discovered << "\nconverted: " << converted
        << "\nskipped: " << skipped << "\nheld: " << conflicts << "\nfailed: " << failed
        << "\ncancelled: " << (cancelled ? "yes" : "no") << "\n\n" << rowsText(items);
    return out.str();
}

namespace {
BatchPlan buildPlan(const fs::path& inputDirectory, const fs::path& outputDirectory, const BatchOptions& options,
                    const std::vector<fs::path>& excludedInputs, const BatchPlan* previous) {
    if (!fs::is_directory(inputDirectory)) throw TextureError("Batch input is not a directory: " + pathToUtf8(inputDirectory));
    if (outputDirectory.empty()) throw TextureError("Batch output directory is empty");
    const auto ext = normalizedExtension(options.outputExtension);
    if ((!pixelExtension(ext) || ext == "txb") && ext != "txi") throw TextureError("Unsupported batch output extension: " + ext);
    BatchPlan plan;
    plan.inputDirectory = inputDirectory; plan.outputDirectory = outputDirectory;
    plan.options = options; plan.excludedInputs = excludedInputs;
    plan.reviewedSettings = settingsKey(options);
    if (previous) {
        if (previous->reviewedSettings != plan.reviewedSettings)
            throw TextureError("Batch settings changed after review. Scan again; nothing was written.");
        plan.reviewedInputs = previous->reviewedInputs;
    }
    plan.options.outputExtension = ext;
    plan.options.inputExtension = normalizedExtension(options.inputExtension);
    if (!plan.options.inputExtension.empty() && !pixelExtension(plan.options.inputExtension) && plan.options.inputExtension != "txi")
        throw TextureError("Unsupported batch input type: " + plan.options.inputExtension);
    const auto inRoot = fs::weakly_canonical(fs::absolute(inputDirectory));
    const auto outRoot = fs::weakly_canonical(fs::absolute(outputDirectory));
    const bool excludeOutput = canonicalPathKey(inRoot) != canonicalPathKey(outRoot) && pathIsWithin(inRoot, outRoot);
    std::vector<fs::path> files;
    auto consider = [&](const fs::directory_entry& entry) {
        checkOperation();
        std::error_code ec;
        if (fs::is_regular_file(entry.symlink_status(ec)) && !ec && isSupportedTexturePath(entry.path())) files.push_back(entry.path());
    };
    std::error_code ec;
    if (options.recursive) {
        fs::recursive_directory_iterator it(inputDirectory, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            checkOperation();
            if (excludeOutput && fs::weakly_canonical(it->path()) == outRoot) it.disable_recursion_pending();
            else consider(*it);
            it.increment(ec);
        }
    } else {
        fs::directory_iterator it(inputDirectory, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) { consider(*it); it.increment(ec); }
    }
    if (ec) throw TextureError("Unable to scan input folder: " + ec.message());
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return genericPathToUtf8(a) < genericPathToUtf8(b); });
    std::set<std::string> pixelStems;
    for (const auto& file : files) if (pixelExtension(extensionLower(file))) pixelStems.insert(canonicalPathKey(file.parent_path() / file.stem()));
    std::vector<std::vector<fs::path>> dependencies;
    std::vector<std::vector<fs::path>> destinations;
    std::vector<bool> requested;
    for (const auto& input : files) {
        if (plan.options.inputExtension != "txi" && extensionLower(input) == "txi" && pixelStems.count(canonicalPathKey(input.parent_path() / input.stem()))) continue;
        BatchItemResult row;
        row.input = input;
        row.output = outputDirectory / lexicalRelativePath(input, inputDirectory);
        row.output.replace_extension("." + ext);
        // An in-place same-format operation keeps the existing filename's case.
        // Do not manufacture a second .txi beside its own .TXI source on POSIX.
        if (canonicalPathKey(row.output) == canonicalPathKey(input)) row.output = input;
        row.status = BatchItemStatus::Ready;
        const bool selected = batchInputRequested(input, plan.options, excludedInputs);
        requested.push_back(selected);
        std::vector<fs::path> reads{input}, writes;
        try {
            if (usesTxiSidecar(input)) {
                auto sidecar = input; sidecar.replace_extension(".txi");
                // Protect absence too: creating a TXI changes another input's interpretation.
                reads.push_back(findTxiSidecar(input).value_or(sidecar));
            }
            if (selected) {
                writes = outputMembers(row.output);
                for (const auto& path : writes) {
                    requireContainedOutput(outputDirectory, path);
                    if (fs::exists(path) && !fs::is_regular_file(path)) throw TextureError("Output is not a regular file: " + pathToUtf8(path));
                    if (fs::exists(path) && !options.overwrite) {
                        row.status = BatchItemStatus::Skipped;
                        row.message = "An output image or TXI already exists; overwrite is off";
                    }
                }
            }
        } catch (const OperationCancelled&) { throw; }
        catch (const std::exception& e) { if (selected) hold(row, e.what()); }
        if (!selected) {
            row.status = BatchItemStatus::Skipped;
            row.message = "Not selected for conversion; source remains protected";
        }
        plan.items.push_back(std::move(row));
        dependencies.push_back(std::move(reads));
        destinations.push_back(std::move(writes));
    }
    // All sources stay protected; only requested conversions claim destinations.
    std::map<std::string, std::set<std::size_t>> writers, readers;
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        for (const auto& path : dependencies[i]) readers[canonicalPathKey(path)].insert(i);
        if (requested[i]) for (const auto& path : destinations[i]) writers[canonicalPathKey(path)].insert(i);
    }
    for (const auto& claim : writers) {
        if (claim.second.size() > 1) for (auto i : claim.second)
            hold(plan.items[i], "More than one selected input claims this output; exclude a contender and review again");
        const auto found = readers.find(claim.first);
        if (found != readers.end()) for (auto writer : claim.second) for (auto reader : found->second) if (writer != reader &&
            !(plan.options.inputExtension == "txi" && !requested[reader] &&
              canonicalPathKey(plan.items[writer].input) == claim.first))
            hold(plan.items[writer], "Would overwrite protected input of " + pathToUtf8(plan.items[reader].input));
    }
    // Raw TXIs remain protected by exact normalized name. Avoid an O(N^2)
    // hard-link alias sweep across every discovered input.
    std::map<std::string, std::vector<fs::path>> rawInputs;
    for (const auto& input : files) rawInputs[canonicalPathKey(input)].push_back(input);
    for (std::size_t i = 0; i < plan.items.size(); ++i) if (requested[i]) for (const auto& output : destinations[i]) {
        const auto own = [&](const fs::path& path) { return std::find(dependencies[i].begin(), dependencies[i].end(), path) != dependencies[i].end(); };
        const auto named = rawInputs.find(canonicalPathKey(output));
        if (named != rawInputs.end()) for (const auto& input : named->second) if (!own(input))
            hold(plan.items[i], "Output would replace another discovered input: " + pathToUtf8(input));
    }
    // Retain decoded compatibility results in the review plan so toggling
    // exclusions does not re-read or re-decode every source.
    for (auto& row : plan.items) if (row.status == BatchItemStatus::Ready) {
        auto reviewed = plan.reviewedInputs.find(row.input);
        if (reviewed == plan.reviewedInputs.end()) {
            BatchInputReview review;
            try {
                checkOperation();
                const auto source = readInput(row.input);
                const auto image = decodeInput(source, row.input);
                const auto issue = batchInputIssue(image, row.output, options);
                if (!issue.empty()) throw TextureError(issue);
                review.compatible = true;
                review.preservesSource = preserveBatchInput(image, row.output, options);
                review.discardsAlpha = !review.preservesSource &&
                    kindForExtension(row.output) == TextureFileKind::Jpeg && image.hasAlpha;
                if (review.preservesSource) {
                    review.message = "Exact copy — no recompression or mip changes";
                    for (const auto& warning : image.compatibilityWarnings)
                        review.message += " | Source warning retained: " + warning;
                } else review.message = "Settings checked; final layout validated during encoding";
                if (review.discardsAlpha) review.message += " | Alpha channel will be discarded (including mask data)";
            } catch (const OperationCancelled&) { throw; }
            catch (const std::exception& error) { review.message = error.what(); }
            reviewed = plan.reviewedInputs.emplace(row.input, std::move(review)).first;
        }
        const auto& review = reviewed->second;
        row.discardsAlpha = review.discardsAlpha; row.preservesSource = review.preservesSource;
        if (review.compatible) row.message = review.message;
        else hold(row, review.message);
    }
    return plan;
}
} // namespace

BatchPlan planTextureBatch(const fs::path& input, const fs::path& output, const BatchOptions& options,
                           const std::vector<fs::path>& excluded) {
    return buildPlan(input, output, options, excluded, nullptr);
}
BatchPlan replanTextureBatch(const BatchPlan& previous, const std::vector<fs::path>& excluded) {
    auto result = buildPlan(previous.inputDirectory, previous.outputDirectory, previous.options, excluded, &previous);
    if (result.items.size() != previous.items.size())
        throw TextureError("The input folder changed. Scan again before converting.");
    for (std::size_t i = 0; i < result.items.size(); ++i)
        if (result.items[i].input != previous.items[i].input)
            throw TextureError("The input folder changed. Scan again before converting.");
    return result;
}
std::vector<fs::path> batchOutputMembers(const fs::path& output) { return outputMembers(output); }
bool batchItemWritesOutput(const BatchItemResult& item) {
    return item.status == BatchItemStatus::Ready &&
        (!item.preservesSource || canonicalPathKey(item.input) != canonicalPathKey(item.output));
}

BatchReport executeTextureBatch(const BatchPlan& plan, const BatchProgress& progress) {
    // Execute the exact reviewed plan. Rebuilding it here used to repeat the
    // full directory scan, path checks and source reads before writing.
    if (plan.conflicts() && !plan.options.skipConflicts)
        throw TextureError("Batch preflight found conflicts. Nothing was written. Review the plan and explicitly choose to process only independent ready items.\n" + plan.summary());
    BatchReport report;
    report.discovered = plan.items.size();
    for (std::size_t index = 0; index < plan.items.size(); ++index) {
        auto row = plan.items[index];
        try {
            checkOperation();
            if (progress && !progress(index, plan.items.size(), row.input)) { report.cancelled = true; break; }
            const bool excluded=std::find(plan.excludedInputs.begin(),plan.excludedInputs.end(),row.input)!=plan.excludedInputs.end();
            if (excluded && row.status == BatchItemStatus::Ready) {
                row.status=BatchItemStatus::Skipped;row.message="Excluded in reviewed plan";++report.skipped;
            } else if (row.status == BatchItemStatus::Conflict) ++report.conflicts;
            else if (row.status == BatchItemStatus::Skipped) ++report.skipped;
            else {
                for (const auto& member : outputMembers(row.output)) {
                    requireContainedOutput(plan.outputDirectory, member);
                    if (fs::exists(member) && !plan.options.overwrite)
                        throw TextureError("Output appeared after review; it was not overwritten: " + pathToUtf8(member));
                }
                // One source read per conversion. The reviewed plan already
                // established format compatibility; no post-encode reread is
                // needed for a normal local modding workflow.
                const auto source = readInput(row.input);
                const auto result = encodeBatchTexture(source.bytes, source.sidecar, row.input, row.output, plan.options);
                checkOperation();
                if (!result.preserved || canonicalPathKey(row.input) != canonicalPathKey(row.output)) {
                    saveEncodedTexture(result.encoded.image, result.encoded.sidecar, row.output);
                    row.wroteOutput = true;
                }
                row.status = BatchItemStatus::Converted;
                row.message = result.preserved ? "Copied original bytes; source compatibility is unchanged" : "Created (encoded)";
                if (result.discardedAlpha) row.message += " | Alpha channel discarded (including mask data)";
                ++report.converted;
            }
        } catch (const OperationCancelled&) { report.cancelled = true; break; }
        catch (const std::exception& e) { row.status = BatchItemStatus::Failed; row.message = e.what(); ++report.failed; }
        report.items.push_back(std::move(row));
    }
    if (!report.cancelled && progress) progress(plan.items.size(), plan.items.size(), {});
    return report;
}

BatchReport batchConvertTextures(const fs::path& input, const fs::path& output, const BatchOptions& options, const BatchProgress& progress) {
    return executeTextureBatch(planTextureBatch(input, output, options), progress);
}
} // namespace neotpc::texture
