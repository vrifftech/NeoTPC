#include "texture/BatchConverter.hpp"
#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"
#include "texture/Operation.hpp"

#include <algorithm>
#include <map>
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
    std::error_code ec;
    const fs::path absoluteRoot = fs::absolute(outputRoot, ec);
    if (ec) throw TextureError("Unable to resolve batch output directory: " + ec.message());
    const fs::path canonicalRoot = fs::weakly_canonical(absoluteRoot, ec);
    if (ec) throw TextureError("Unable to resolve batch output directory: " + ec.message());
    const fs::path absoluteOutput = fs::absolute(output, ec);
    if (ec) throw TextureError("Unable to resolve batch output path: " + ec.message());
    const fs::path canonicalOutput = fs::weakly_canonical(absoluteOutput, ec);
    if (ec) throw TextureError("Unable to resolve batch output path: " + ec.message());
    const fs::path canonicalParent = fs::weakly_canonical(absoluteOutput.parent_path(), ec);
    if (ec) throw TextureError("Unable to resolve batch output parent: " + ec.message());
    if (!pathIsWithin(canonicalRoot, canonicalParent) ||
        !pathIsWithin(canonicalRoot, canonicalOutput)) {
        throw TextureError("Refusing batch output outside the selected output directory: " +
                           pathToUtf8(output));
    }

    // Resource names are case-insensitive even on case-sensitive hosts. Do not
    // create a second differently-cased name beside an existing resource.
    ec.clear();
    fs::directory_iterator siblings(absoluteOutput.parent_path(),ec), siblingEnd;
    if(ec && ec != std::errc::no_such_file_or_directory) throw TextureError("Unable to inspect output directory: " + ec.message());
    while(!ec && siblings!=siblingEnd) {
        checkOperation();
        if(siblings->path().filename()!=absoluteOutput.filename() &&
           asciiLower(pathToUtf8(siblings->path().filename()))==asciiLower(pathToUtf8(absoluteOutput.filename()))) {
            std::error_code sameError;
            if(!fs::equivalent(siblings->path(),absoluteOutput,sameError) || sameError)
                throw TextureError("Existing output differs only by case; resolve it before converting: " + pathToUtf8(siblings->path()));
        }
        siblings.increment(ec);
    }
    if(ec && ec != std::errc::no_such_file_or_directory) throw TextureError("Unable to inspect output directory: " + ec.message());
    ec.clear();
    const fs::file_status status = fs::symlink_status(absoluteOutput, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw TextureError("Unable to inspect batch output path: " + ec.message());
    }
    if (!ec && fs::is_symlink(status)) {
        throw TextureError("Refusing to replace a symlinked batch output: " + pathToUtf8(output));
    }
}

std::vector<fs::path> outputMembers(const fs::path& image) {
    std::vector<fs::path> out{image};
    if (usesTxiSidecar(image)) {
        auto sidecar = findTxiSidecar(image);
        if (sidecar) out.push_back(*sidecar);
        else { auto target = image; target.replace_extension(".txi"); out.push_back(target); }
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
} // namespace

bool isSupportedTexturePath(const fs::path& path) {
    return pixelExtension(extensionLower(path)) || extensionLower(path) == "txi";
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

BatchPlan planTextureBatch(const fs::path& inputDirectory, const fs::path& outputDirectory, const BatchOptions& options) {
    if (!fs::is_directory(inputDirectory)) throw TextureError("Batch input is not a directory: " + pathToUtf8(inputDirectory));
    if (outputDirectory.empty()) throw TextureError("Batch output directory is empty");
    const auto ext = normalizedExtension(options.outputExtension);
    if ((!pixelExtension(ext) || ext == "txb") && ext != "txi") throw TextureError("Unsupported batch output extension: " + ext);
    BatchPlan plan{inputDirectory, outputDirectory, options, {}};
    plan.options.outputExtension = ext;
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
    for (const auto& input : files) {
        if (extensionLower(input) == "txi" && pixelStems.count(canonicalPathKey(input.parent_path() / input.stem()))) continue;
        BatchItemResult row;
        row.input = input;
        row.output = outputDirectory / lexicalRelativePath(input, inputDirectory);
        row.output.replace_extension("." + ext);
        row.status = BatchItemStatus::Ready;
        std::vector<fs::path> reads{input}, writes{row.output};
        try {
            if (usesTxiSidecar(input)) if (auto sidecar = findTxiSidecar(input)) reads.push_back(*sidecar);
            writes = outputMembers(row.output);
            for (const auto& path : writes) {
                requireContainedOutput(outputDirectory, path);
                if (fs::exists(path) && !fs::is_regular_file(path)) throw TextureError("Output is not a regular file: " + pathToUtf8(path));
                if (fs::exists(path) && !options.overwrite) {
                    row.status = BatchItemStatus::Skipped;
                    row.message = "An output image or TXI already exists; overwrite is off";
                }
            }
        } catch (const OperationCancelled&) { throw; }
        catch (const std::exception& e) { hold(row, e.what()); }
        plan.items.push_back(std::move(row));
        dependencies.push_back(std::move(reads));
        destinations.push_back(std::move(writes));
    }
    // Claim every output member, including a sidecar that might be deleted by a
    // metadata-free output. Never suffix resource names or select a winner.
    std::map<std::string, std::set<std::size_t>> writers, readers;
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        for (const auto& path : dependencies[i]) readers[canonicalPathKey(path)].insert(i);
        for (const auto& path : destinations[i]) writers[canonicalPathKey(path)].insert(i);
    }
    for (const auto& claim : writers) {
        if (claim.second.size() > 1) {
            for (auto i : claim.second) hold(plan.items[i], "More than one input claims this output resource; no input was renamed or preferred");
        }
        const auto found = readers.find(claim.first);
        if (found != readers.end()) {
            for (auto writer : claim.second) for (auto reader : found->second) if (writer != reader) {
                hold(plan.items[writer], "Would overwrite input needed by " + pathToUtf8(plan.items[reader].input));
                hold(plan.items[reader], "Input overlaps another planned output");
            }
        }
    }
    // Existing hard-link aliases need an identity check beyond normalized paths.
    for (std::size_t i = 0; i < plan.items.size(); ++i) for (const auto& output : destinations[i]) {
        ec.clear();
        if (!fs::exists(output, ec) || ec || fs::hard_link_count(output, ec) < 2 || ec) continue;
        for (std::size_t j = 0; j < plan.items.size(); ++j) if (i != j) for (const auto& input : dependencies[j]) {
            ec.clear();
            if (fs::equivalent(output, input, ec) && !ec) {
                hold(plan.items[i], "Output is a hard-link alias of another input");
                hold(plan.items[j], "Input has a hard-link alias in the output plan");
            }
        }
    }
    return plan;
}

BatchReport executeTextureBatch(const BatchPlan& plan, const BatchProgress& progress) {
    if (plan.conflicts() && !plan.options.skipConflicts)
        throw TextureError("Batch preflight found conflicts. Nothing was written. Review the plan and explicitly choose to process only independent ready items.\n" + plan.summary());
    // Recheck the full plan immediately before any write: new files/aliases can
    // change both ownership and sidecar names while the confirmation is open.
    auto verified = planTextureBatch(plan.inputDirectory, plan.outputDirectory, plan.options);
    if (verified.items.size() != plan.items.size()) throw TextureError("The input folder changed after preflight. Scan it again; nothing was written.");
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        const auto& a = plan.items[i]; const auto& b = verified.items[i];
        if (a.input != b.input || a.output != b.output || a.status != b.status)
            throw TextureError("The batch plan changed after preflight. Scan it again; nothing was written.");
    }
    BatchReport report;
    report.discovered = plan.items.size();
    for (std::size_t index = 0; index < plan.items.size(); ++index) {
        auto row = plan.items[index];
        try {
            checkOperation();
            if (progress && !progress(index, plan.items.size(), row.input)) { report.cancelled = true; break; }
            if (row.status == BatchItemStatus::Conflict) ++report.conflicts;
            else if (row.status == BatchItemStatus::Skipped) ++report.skipped;
            else {
                for (const auto& member : outputMembers(row.output)) {
                    requireContainedOutput(plan.outputDirectory, member);
                    if (fs::exists(member) && !plan.options.overwrite) throw TextureError("Output appeared after preflight; it was not overwritten: " + pathToUtf8(member));
                }
                const auto texture = loadTexture(row.input);
                if (extensionLower(row.output) != "txi" && !texture.hasPixels()) throw TextureError("TXI-only input has no pixels to encode");
                saveTexture(texture, row.output, plan.options.saveOptions);
                row.status = BatchItemStatus::Converted;
                row.message = "Created";
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
