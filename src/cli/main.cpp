#include "texture/BatchConverter.hpp"
#include "texture/FileUtil.hpp"
#include "texture/Image.hpp"
#include "texture/Txi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct ConvertState {
    neotpc::texture::TextureSaveOptions options;
    std::optional<std::uint8_t> setAlpha;
    std::optional<double> scaleAlpha;
    bool invertAlpha = false;
    std::optional<fs::path> txiFile;
    std::vector<std::pair<std::string, std::string>> txiValues;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string nextValue(const std::vector<std::string>& args, std::size_t& index, const std::string& option) {
    if (index + 1 >= args.size()) fail(option + " requires a value");
    return args[++index];
}

unsigned parseUnsigned(const std::string& value, unsigned minimum, unsigned maximum, const std::string& option) {
    std::size_t consumed = 0;
    unsigned long parsed = 0;
    try {
        parsed = std::stoul(value, &consumed, 10);
    } catch (...) {
        fail(option + " expects an integer");
    }
    if (consumed != value.size() || parsed < minimum || parsed > maximum) {
        fail(option + " expects an integer from " + std::to_string(minimum) + " through " + std::to_string(maximum));
    }
    return static_cast<unsigned>(parsed);
}

double parseNumber(const std::string& value, const std::string& option) {
    std::size_t consumed = 0;
    double parsed = 0.0;
    try {
        parsed = std::stod(value, &consumed);
    } catch (...) {
        fail(option + " expects a number");
    }
    if (consumed != value.size() || !std::isfinite(parsed)) fail(option + " expects a finite number");
    return parsed;
}

bool parseConvertOption(const std::vector<std::string>& args, std::size_t& index, ConvertState& state) {
    const auto& option = args[index];
    if (option == "--compression") {
        state.options.compression = neotpc::texture::textureCompressionFromString(nextValue(args, index, option));
    } else if (option == "--dxt-quality") {
        state.options.dxtQuality = neotpc::texture::dxtCompressionQualityFromString(nextValue(args, index, option));
    } else if (option == "--dxt-metric") {
        state.options.dxtMetric = neotpc::texture::dxtErrorMetricFromString(nextValue(args, index, option));
    } else if (option == "--weight-color-by-alpha") {
        state.options.weightColorByAlpha = true;
    } else if (option == "--dxt1-alpha-threshold") {
        state.options.dxt1AlphaThreshold = static_cast<std::uint8_t>(
            parseUnsigned(nextValue(args, index, option), 0, 255, option));
    } else if (option == "--jpeg-quality") {
        state.options.jpegQuality = static_cast<std::uint8_t>(
            parseUnsigned(nextValue(args, index, option), 1, 100, option));
    } else if (option == "--no-mipmaps") {
        state.options.generateMipmaps = false;
    } else if (option == "--mipmaps") {
        state.options.generateMipmaps = true;
    } else if (option == "--bicubic") {
        state.options.bicubicMipmaps = true;
    } else if (option == "--flip-x" || option == "--flip-x-on-save") {
        state.options.flipXOnSave = true;
    } else if (option == "--flip-y" || option == "--flip-y-on-save") {
        state.options.flipYOnSave = true;
    } else if (option == "--alpha-blending") {
        state.options.alphaBlending = static_cast<float>(parseNumber(nextValue(args, index, option), option));
    } else if (option == "--set-alpha") {
        state.setAlpha = static_cast<std::uint8_t>(parseUnsigned(nextValue(args, index, option), 0, 255, option));
    } else if (option == "--scale-alpha") {
        state.scaleAlpha = parseNumber(nextValue(args, index, option), option);
        if (*state.scaleAlpha < 0.0) fail(option + " must not be negative");
    } else if (option == "--invert-alpha") {
        state.invertAlpha = true;
    } else if (option == "--txi") {
        state.txiFile = fs::u8path(nextValue(args, index, option));
    } else if (option == "--set-txi") {
        const auto key = nextValue(args, index, option);
        const auto value = nextValue(args, index, option);
        state.txiValues.emplace_back(key, value);
    } else {
        return false;
    }
    return true;
}

void applyConvertState(neotpc::texture::TextureData& texture, const ConvertState& state) {
    if (state.txiFile) {
        const auto bytes = neotpc::texture::readFileBytes(*state.txiFile);
        texture.txi.assign(bytes.begin(), bytes.end());
    }
    for (const auto& entry : state.txiValues) {
        texture.txi = neotpc::texture::setTxiValue(texture.txi, entry.first, entry.second);
    }
    if (state.setAlpha) neotpc::texture::setTextureAlpha(texture, *state.setAlpha);
    if (state.scaleAlpha) neotpc::texture::scaleTextureAlpha(texture, *state.scaleAlpha);
    if (state.invertAlpha) neotpc::texture::invertTextureAlpha(texture);
}

void printUsage(std::ostream& out) {
    out <<
        "NeoTPC CLI\n"
        "Usage:\n"
        "  neotpc-cli info <texture>\n"
        "  neotpc-cli convert <input> <output> [options]\n"
        "  neotpc-cli split <input.tpc> <output.tga>\n"
        "  neotpc-cli combine <input.tga> <output.tpc> [options]\n"
        "  neotpc-cli txi-export <texture> <output.txi>\n"
        "  neotpc-cli txi-replace <input.tpc> <metadata.txi>\n"
        "  neotpc-cli batch <input-dir> <output-dir> --format <ext> [options]\n"
        "  neotpc-cli txi-validate <texture-or-txi>\n"
        "\n"
        "Input formats: tpc, txb (read-only), tga, dds, png, jpg/jpeg, bmp, txi\n"
        "Output formats: tpc, tga, dds, png, jpg/jpeg, bmp, txi\n"
        "Conversion options:\n"
        "  --compression auto|none|grey|dxt1|dxt3|dxt5|swizzled-bgra\n"
        "  --dxt-quality fast|normal|high\n"
        "  --dxt-metric perceptual|uniform\n"
        "  --weight-color-by-alpha\n"
        "  --dxt1-alpha-threshold 0..255\n"
        "  --jpeg-quality 1..100\n"
        "  --no-mipmaps | --mipmaps | --bicubic\n"
        "  --flip-x | --flip-y\n"
        "  --alpha-blending <number>\n"
        "  --set-alpha 0..255 | --scale-alpha <factor> | --invert-alpha\n"
        "  --txi <metadata.txi> | --set-txi <key> <value>\n"
        "Batch options:\n"
        "  --recursive | --no-recursive | --overwrite\n";
}

int commandInfo(const std::vector<std::string>& args) {
    if (args.size() != 3) fail("info requires one texture path");
    const auto texture = neotpc::texture::loadTexture(fs::u8path(args[2]));
    std::cout << neotpc::texture::textureSummary(texture) << '\n';
    if (!texture.txi.empty()) std::cout << neotpc::texture::txiValidationReport(texture.txi) << '\n';
    std::cout << neotpc::texture::imageCodecSupportReport();
    return 0;
}

int commandValidate(const std::vector<std::string>& args) {
    if (args.size() != 3) fail("txi-validate requires one texture or TXI path");
    const auto texture = neotpc::texture::loadTexture(fs::u8path(args[2]));
    const auto issues = neotpc::texture::validateTxiText(texture.txi);
    std::cout << neotpc::texture::txiValidationReport(texture.txi);
    return std::any_of(issues.begin(), issues.end(), [](const neotpc::texture::TxiValidationIssue& issue) {
        return issue.severity == neotpc::texture::TxiIssueSeverity::Error;
    }) ? 2 : 0;
}

int commandConvert(const std::vector<std::string>& args) {
    if (args.size() < 4) fail("convert requires an input and output path");
    ConvertState state;
    for (std::size_t index = 4; index < args.size(); ++index) {
        if (!parseConvertOption(args, index, state)) fail("Unknown conversion option: " + args[index]);
    }
    auto texture = neotpc::texture::loadTexture(fs::u8path(args[2]));
    applyConvertState(texture, state);
    neotpc::texture::saveTexture(texture, fs::u8path(args[3]), state.options);
    std::cout << "Converted " << args[2] << " -> " << args[3] << '\n';
    return 0;
}

int commandSplit(const std::vector<std::string>& args) {
    if (args.size() != 4) fail("split requires an input TPC and output TGA path");
    const auto pair = neotpc::texture::splitTpcToTgaTxi(
        fs::u8path(args[2]), fs::u8path(args[3]));
    std::cout << "Split " << args[2] << " -> "
              << neotpc::texture::pathToUtf8(pair.tga) << " + "
              << neotpc::texture::pathToUtf8(pair.txi) << '\n';
    return 0;
}

int commandCombine(const std::vector<std::string>& args) {
    if (args.size() < 4) fail("combine requires an input TGA and output TPC path");
    ConvertState state;
    for (std::size_t index = 4; index < args.size(); ++index) {
        if (!parseConvertOption(args, index, state)) fail("Unknown combine option: " + args[index]);
    }

    const auto input = fs::u8path(args[2]);
    const auto output = fs::u8path(args[3]);
    if (neotpc::texture::extensionLower(input) != "tga") {
        fail("combine input must use the .tga extension");
    }
    if (neotpc::texture::extensionLower(output) != "tpc") {
        fail("combine output must use the .tpc extension");
    }

    // Build the final TXI text before decoding the TGA so cube/animation
    // directives are applied to the source canvas during layout inference.
    std::string finalTxi;
    if (state.txiFile) {
        const auto txiBytes = neotpc::texture::readFileBytes(*state.txiFile);
        finalTxi.assign(txiBytes.begin(), txiBytes.end());
    } else {
        auto sidecar = input;
        sidecar.replace_extension(".txi");
        std::error_code ec;
        if (fs::is_regular_file(sidecar, ec)) {
            const auto txiBytes = neotpc::texture::readFileBytes(sidecar);
            finalTxi.assign(txiBytes.begin(), txiBytes.end());
        }
    }
    for (const auto& entry : state.txiValues) {
        finalTxi = neotpc::texture::setTxiValue(finalTxi, entry.first, entry.second);
    }

    const auto imageBytes = neotpc::texture::readFileBytes(input);
    auto texture = neotpc::texture::loadTextureBytes(imageBytes, input, std::move(finalTxi));
    if (texture.kind != neotpc::texture::TextureFileKind::Tga) {
        fail("combine input is not a TGA image");
    }
    if (state.setAlpha) neotpc::texture::setTextureAlpha(texture, *state.setAlpha);
    if (state.scaleAlpha) neotpc::texture::scaleTextureAlpha(texture, *state.scaleAlpha);
    if (state.invertAlpha) neotpc::texture::invertTextureAlpha(texture);
    neotpc::texture::saveTexture(texture, output, state.options);
    std::cout << "Combined " << args[2] << " -> " << args[3] << '\n';
    return 0;
}

int commandTxiExport(const std::vector<std::string>& args) {
    if (args.size() != 4) fail("txi-export requires a texture and output TXI path");
    const auto texture = neotpc::texture::loadTexture(fs::u8path(args[2]));
    const auto output = fs::u8path(args[3]);
    if (neotpc::texture::extensionLower(output) != "txi") {
        fail("txi-export output must use the .txi extension");
    }
    neotpc::texture::saveTexture(texture, output);
    std::cout << "Exported TXI " << args[2] << " -> " << args[3] << '\n';
    return 0;
}

int commandTxiReplace(const std::vector<std::string>& args) {
    if (args.size() != 4) fail("txi-replace requires a TPC and TXI path");
    const auto txiBytes = neotpc::texture::readFileBytes(fs::u8path(args[3]));
    neotpc::texture::replaceTpcEmbeddedTxi(
        fs::u8path(args[2]), std::string(txiBytes.begin(), txiBytes.end()));
    std::cout << "Replaced embedded TXI in " << args[2] << '\n';
    return 0;
}

int commandBatch(const std::vector<std::string>& args) {
    if (args.size() < 4) fail("batch requires input and output directories");
    ConvertState state;
    neotpc::texture::BatchOptions batch;
    bool sawFormat = false;
    for (std::size_t index = 4; index < args.size(); ++index) {
        const auto& option = args[index];
        if (option == "--format") {
            batch.outputExtension = nextValue(args, index, option);
            sawFormat = true;
        } else if (option == "--recursive") {
            batch.recursive = true;
        } else if (option == "--no-recursive") {
            batch.recursive = false;
        } else if (option == "--overwrite") {
            batch.overwrite = true;
        } else if (!parseConvertOption(args, index, state)) {
            fail("Unknown batch option: " + args[index]);
        }
    }
    if (!sawFormat) fail("batch requires --format <ext>");
    if (state.setAlpha || state.scaleAlpha || state.invertAlpha || state.txiFile || !state.txiValues.empty()) {
        fail("Pixel/TXI mutation options are available for single-file convert, not batch");
    }
    batch.saveOptions = state.options;
    const auto report = neotpc::texture::batchConvertTextures(fs::u8path(args[2]), fs::u8path(args[3]), batch);
    std::cout << report.summary();
    return report.ok() ? 0 : 2;
}

int run(const std::vector<std::string>& args) {
    if (args.size() < 2 || args[1] == "--help" || args[1] == "-h" || args[1] == "help") {
        printUsage(std::cout);
        return args.size() < 2 ? 1 : 0;
    }
    if (args[1] == "--version") {
#ifdef NEOTPC_VERSION
        std::cout << NEOTPC_VERSION << '\n';
#else
        std::cout << "development\n";
#endif
        return 0;
    }
    if (args[1] == "info") return commandInfo(args);
    if (args[1] == "convert") return commandConvert(args);
    if (args[1] == "split") return commandSplit(args);
    if (args[1] == "combine") return commandCombine(args);
    if (args[1] == "txi-export") return commandTxiExport(args);
    if (args[1] == "txi-replace") return commandTxiReplace(args);
    if (args[1] == "batch") return commandBatch(args);
    if (args[1] == "txi-validate") return commandValidate(args);
    fail("Unknown command: " + args[1]);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) args.emplace_back(argv[index]);
    try {
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n\n";
        printUsage(std::cerr);
        return 1;
    }
}
