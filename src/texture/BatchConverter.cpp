#include "texture/BatchConverter.hpp"

#include "texture/Error.hpp"
#include "texture/FileUtil.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace neotpc::texture {
namespace {

bool isInputPixelExtension(const std::string& extension) {
    return extension == "tga" || extension == "tpc" || extension == "dds" ||
           extension == "png" || extension == "jpg" || extension == "jpeg" ||
           extension == "jpe" || extension == "bmp" || extension == "txb";
}

bool isOutputExtension(const std::string& extension) {
    return (isInputPixelExtension(extension) && extension != "txb") || extension == "txi";
}

std::string pathKey(const fs::path& path) {
    auto key = genericPathToUtf8(path.lexically_normal());
#if defined(_WIN32)
    key = asciiLower(std::move(key));
#endif
    return key;
}

static std::string normalizeTextureExtension(std::string extension) {
    extension = asciiLower(std::move(extension));
    while (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
    if (extension == "jpeg" || extension == "jpe") return "jpg";
    return extension;
}

bool hasPixelSidecarPartner(const fs::path& txiPath) {
    static const char* extensions[] = {".tga", ".tpc", ".dds", ".png", ".jpg", ".jpeg", ".jpe", ".bmp"};
    for (const char* extension : extensions) {
        auto candidate = txiPath;
        candidate.replace_extension(extension);
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) return true;
    }
    return false;
}

fs::path collisionPath(const fs::path& initial,
                       const fs::path& source,
                       std::set<std::string>& claimed) {
    if (claimed.insert(pathKey(initial)).second) return initial;

    const auto sourceExtension = normalizeTextureExtension(extensionLower(source));
    const auto outputExtension = initial.extension();
    const auto parent = initial.parent_path();

    auto collisionStem = initial.stem();
    collisionStem += fs::path("." + sourceExtension).native();

    auto candidateName = collisionStem;
    candidateName += outputExtension.native();
    auto candidate = parent / candidateName;
    unsigned suffix = 2;
    while (!claimed.insert(pathKey(candidate)).second) {
        candidateName = collisionStem;
        candidateName += fs::path("-" + std::to_string(suffix++)).native();
        candidateName += outputExtension.native();
        candidate = parent / candidateName;
    }
    return candidate;
}

std::string oneLine(std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\t', ' ');
    return value;
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

    ec.clear();
    const fs::file_status status = fs::symlink_status(absoluteOutput, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw TextureError("Unable to inspect batch output path: " + ec.message());
    }
    if (!ec && fs::is_symlink(status)) {
        throw TextureError("Refusing to replace a symlinked batch output: " + pathToUtf8(output));
    }
}

} // namespace

bool isSupportedTexturePath(const fs::path& path) {
    const auto extension = extensionLower(path);
    return isInputPixelExtension(extension) || extension == "txi";
}

std::string BatchReport::summary() const {
    std::ostringstream out;
    out << "Texture batch conversion\n"
        << "discovered: " << discovered << '\n'
        << "converted: " << converted << '\n'
        << "skipped: " << skipped << '\n'
        << "failed: " << failed << '\n'
        << "cancelled: " << (cancelled ? "yes" : "no") << '\n';
    if (!items.empty()) {
        out << "\nstatus\tinput\toutput\tmessage\n";
        for (const auto& item : items) {
            const char* status = item.status == BatchItemStatus::Converted ? "converted" :
                                 item.status == BatchItemStatus::Skipped ? "skipped" : "failed";
            out << status << '\t' << genericPathToUtf8(item.input) << '\t'
                << genericPathToUtf8(item.output) << '\t' << oneLine(item.message) << '\n';
        }
    }
    return out.str();
}

BatchReport batchConvertTextures(const fs::path& inputDirectory,
                                 const fs::path& outputDirectory,
                                 const BatchOptions& options,
                                 const BatchProgress& progress) {
    if (!fs::is_directory(inputDirectory)) {
        throw TextureError("Batch input is not a directory: " + pathToUtf8(inputDirectory));
    }
    if (outputDirectory.empty()) throw TextureError("Batch output directory is empty");

    const auto outputExtension = normalizeTextureExtension(options.outputExtension);
    if (!isOutputExtension(outputExtension)) {
        throw TextureError("Unsupported batch output extension: " + outputExtension);
    }

    std::vector<fs::path> inputs;
    const auto consider = [&](const fs::directory_entry& entry) {
        std::error_code ec;
        const fs::file_status status = entry.symlink_status(ec);
        if (ec || !fs::is_regular_file(status) || !isSupportedTexturePath(entry.path())) return;
        if (extensionLower(entry.path()) == "txi" && hasPixelSidecarPartner(entry.path())) return;
        inputs.push_back(entry.path());
    };

    std::error_code iterationError;
    if (options.recursive) {
        fs::recursive_directory_iterator it(inputDirectory, fs::directory_options::skip_permission_denied, iterationError);
        fs::recursive_directory_iterator end;
        while (!iterationError && it != end) {
            consider(*it);
            it.increment(iterationError);
        }
    } else {
        fs::directory_iterator it(inputDirectory, fs::directory_options::skip_permission_denied, iterationError);
        fs::directory_iterator end;
        while (!iterationError && it != end) {
            consider(*it);
            it.increment(iterationError);
        }
    }
    if (iterationError) {
        throw TextureError("Unable to scan batch input: " + iterationError.message());
    }

    std::sort(inputs.begin(), inputs.end(), [](const fs::path& left, const fs::path& right) {
        return pathKey(left) < pathKey(right);
    });

    BatchReport report;
    report.discovered = inputs.size();
    std::set<std::string> claimedOutputs;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        if (progress && !progress(index + 1, inputs.size(), input)) {
            report.cancelled = true;
            break;
        }

        const auto relative = lexicalRelativePath(input, inputDirectory);
        auto output = outputDirectory / relative;
        output.replace_extension("." + outputExtension);
        output = collisionPath(output, input, claimedOutputs);

        BatchItemResult result;
        result.input = input;
        result.output = output;
        try {
            std::error_code existsError;
            const bool exists = fs::exists(output, existsError);
            if (existsError) throw TextureError("Unable to inspect output: " + existsError.message());
            if (exists && !options.overwrite) {
                result.status = BatchItemStatus::Skipped;
                result.message = "Output exists (use overwrite to replace it)";
                ++report.skipped;
            } else {
                requireContainedOutput(outputDirectory, output);
                auto texture = loadTexture(input);
                if (outputExtension != "txi" && !texture.hasPixels()) {
                    throw TextureError("TXI-only input has no pixels to encode");
                }
                saveTexture(texture, output, options.saveOptions);
                result.status = BatchItemStatus::Converted;
                result.message = "OK";
                ++report.converted;
            }
        } catch (const std::exception& error) {
            result.status = BatchItemStatus::Failed;
            result.message = error.what();
            ++report.failed;
        }
        report.items.push_back(std::move(result));
    }
    return report;
}

} // namespace neotpc::texture
