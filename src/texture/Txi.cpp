#include "texture/Txi.hpp"

#include "texture/FileUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace neotpc::texture {
namespace {

std::string trimTxi(const std::string& input) {
    std::size_t first = 0;
    while (first < input.size() &&
           (input[first] == '\0' || std::isspace(static_cast<unsigned char>(input[first])))) {
        ++first;
    }
    std::size_t last = input.size();
    while (last > first &&
           (input[last - 1] == '\0' || std::isspace(static_cast<unsigned char>(input[last - 1])))) {
        --last;
    }
    return input.substr(first, last - first);
}

std::optional<long> parseDecimalInteger(const std::string& value) {
    const std::string trimmed = trimTxi(value);
    if (trimmed.empty()) return std::nullopt;
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(trimmed.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0') return std::nullopt;
    return parsed;
}

std::optional<long> parseIntegerLike(const std::string& value) {
    const std::string lower = asciiLower(trimTxi(value));
    if (lower == "true") return 1;
    if (lower == "false") return 0;
    return parseDecimalInteger(lower);
}

std::optional<double> parseFloatValue(const std::string& value) {
    const std::string trimmed = trimTxi(value);
    if (trimmed.empty()) return std::nullopt;
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(trimmed.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0') return std::nullopt;
    return parsed;
}

bool parseBoolean(const std::string& value) {
    const std::string lower = asciiLower(trimTxi(value));
    return lower == "true" || lower == "false" || lower == "1" || lower == "0";
}

std::optional<std::vector<double>> parseVector3(const std::string& value) {
    std::istringstream in(value);
    std::string x, y, z, extra;
    if (!(in >> x >> y >> z) || (in >> extra)) return std::nullopt;
    const auto xv = parseFloatValue(x);
    const auto yv = parseFloatValue(y);
    const auto zv = parseFloatValue(z);
    if (!xv || !yv || !zv) return std::nullopt;
    return std::vector<double>{*xv, *yv, *zv};
}

bool parseSingleFloat(const std::string& value) {
    std::istringstream in(value);
    std::string token, extra;
    if (!(in >> token) || (in >> extra)) return false;
    return parseFloatValue(token).has_value();
}

bool oneWhitespaceFreeToken(const std::string& value) {
    const std::string trimmed = trimTxi(value);
    if (trimmed.empty()) return false;
    return std::none_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

bool exactAllowedValue(const TxiDirectiveInfo& directive, const std::string& value) {
    const std::string trimmed = trimTxi(value);
    return std::find(directive.allowedValues.begin(), directive.allowedValues.end(), trimmed) !=
           directive.allowedValues.end();
}

bool startsWithIgnoreCase(const std::string& candidate, std::string_view prefix) {
    if (prefix.size() > candidate.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const unsigned char left = static_cast<unsigned char>(candidate[index]);
        const unsigned char right = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

bool beginsEndList(const std::string& line) {
    return startsWithIgnoreCase(trimTxi(line), "endlist");
}

bool isListKind(TxiDirectiveValueKind kind) {
    return kind == TxiDirectiveValueKind::FloatList ||
           kind == TxiDirectiveValueKind::Vector3List;
}

void sortAndUnique(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::vector<std::string> valueSuggestions(const TxiDirectiveInfo& directive) {
    if (!directive.allowedValues.empty()) return directive.allowedValues;
    if (directive.valueKind == TxiDirectiveValueKind::Boolean) {
        return {"true", "false", "1", "0"};
    }
    return {};
}

bool isBaseControllerKey(const std::string& key) {
    static const std::set<std::string> keys = {
        "channelscale", "channeltranslate", "distort", "distortangle",
        "distortionamplitude", "speed", "channelscale0", "channelscale1",
        "channelscale2", "channelscale3", "channeltranslate0", "channeltranslate1",
        "channeltranslate2", "channeltranslate3"
    };
    return keys.count(key) != 0;
}

bool isWaterControllerKey(const std::string& key) {
    static const std::set<std::string> keys = {
        "forcecyclespeed", "anglecyclespeed", "waterwidth", "waterheight"
    };
    return keys.count(key) != 0;
}

bool isArturoControllerKey(const std::string& key) {
    static const std::set<std::string> keys = {"arturowidth", "arturoheight"};
    return keys.count(key) != 0;
}

bool isNeoTpcCompatibilityDirective(const std::string& key) {
    return key == "compresstexture";
}

void addIssue(std::vector<TxiValidationIssue>& issues,
              TxiIssueSeverity severity,
              std::size_t line,
              const std::string& key,
              std::string message) {
    issues.push_back(TxiValidationIssue{severity, line, key, std::move(message)});
}

std::string enumValueDescription(const std::string& directive, const std::string& value) {
    if (directive == "blending") {
        if (value == "additive") return "Adds source color to the destination for glow or light effects.";
        if (value == "punchthrough") return "Uses hard alpha cutout behavior for grates, hair, and similar edges.";
    }
    if (directive == "proceduretype") {
        static const std::map<std::string, std::string> descriptions = {
            {"water", "Animated ripple or water height field."},
            {"life", "Cellular-automaton or life-style evolving pattern."},
            {"perlin", "Animated Perlin-noise height field."},
            {"arturo", "Arturo sine/interpolation procedural pattern."},
            {"wave", "Randomized animated wave or height-field texture."},
            {"cycle", "Cycles through the numx by numy frame grid at fps."},
            {"random", "Random procedural height or noise field."},
            {"ringtexdistort", "Radial or ring-shaped distortion effect."},
            {"dirty", "KOTOR 2 dirt/noise controller using parameter set 1."},
            {"dirty2", "KOTOR 2 dirt/noise controller using parameter set 2."},
            {"dirty3", "KOTOR 2 dirt/noise controller using parameter set 3."},
        };
        const auto found = descriptions.find(value);
        if (found != descriptions.end()) return found->second;
    }
    if (directive == "isbumpmap") {
        if (value == "0") return "Ordinary texture; not treated as bump data.";
        if (value == "1") return "Legacy one-byte height bump representation.";
        if (value == "2") return "Four-byte authored normal or bump representation.";
    }
    return {};
}

} // namespace

std::string txiDirectiveValueKindToString(TxiDirectiveValueKind kind) {
    switch (kind) {
    case TxiDirectiveValueKind::Boolean: return "boolean";
    case TxiDirectiveValueKind::Integer: return "integer";
    case TxiDirectiveValueKind::SignedShort: return "signed 16-bit integer";
    case TxiDirectiveValueKind::Float: return "float";
    case TxiDirectiveValueKind::Vector3: return "vector3";
    case TxiDirectiveValueKind::ResourceName: return "resource name";
    case TxiDirectiveValueKind::Enum: return "enum";
    case TxiDirectiveValueKind::FloatList: return "float list";
    case TxiDirectiveValueKind::Vector3List: return "vector3 list";
    }
    return "unknown";
}

std::string txiIssueSeverityToString(TxiIssueSeverity severity) {
    switch (severity) {
    case TxiIssueSeverity::Info: return "info";
    case TxiIssueSeverity::Warning: return "warning";
    case TxiIssueSeverity::Error: return "error";
    }
    return "unknown";
}

const std::vector<TxiDirectiveInfo>& txiDirectiveCatalog() {
    static const std::vector<TxiDirectiveInfo> catalog = {
        {"bumpmaptexture", "Material", TxiDirectiveValueKind::ResourceName, "resource name · one whitespace-free token", "empty", "Names the bump or normal-map texture used by the material.", "The referenced texture normally declares isbumpmap 1 or 2.", {}},
        {"bumpyshinytexture", "Material", TxiDirectiveValueKind::ResourceName, "resource name · one whitespace-free token", "empty", "Names the auxiliary shiny texture used by the legacy bumpy-shiny path.", "This is separate from bumpmaptexture.", {}},
        {"envmaptexture", "Material", TxiDirectiveValueKind::ResourceName, "resource name · one whitespace-free token", "empty", "Names the environment-map texture used for reflections.", "Use with isenvironmentmapped and envmapalpha where applicable.", {}},
        {"blending", "Material", TxiDirectiveValueKind::Enum, "enum · additive | punchthrough", "normal blend", "Selects additive blending or hard alpha cutout behavior.", "Any other or case-mismatched token leaves the normal blend state unchanged.", {"additive", "punchthrough"}},
        {"decal", "Material", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "false", "Marks the material as a decal or overlay.", "Affects material classification and draw ordering.", {}},
        {"renderbmlmtype", "Material", TxiDirectiveValueKind::Integer, "integer · KOTOR 1: 0 or nonzero", "1", "Selects between two KOTOR 1 bump-map-plus-lightmap render paths.", "KOTOR 2 ignores this setting; NeoTPC preserves it.", {}},
        {"wateralpha", "Material", TxiDirectiveValueKind::Float, "float · normally 0..1", "1.0", "Sets water or environment-bump transparency.", "Values below 1 select a translucent render path; 1 is opaque.", {}},
        {"proceduretype", "Basic texture", TxiDirectiveValueKind::Enum, "enum · water | life | perlin | arturo | wave | cycle | random | ringtexdistort | dirty | dirty2 | dirty3", "none", "Constructs the procedural texture controller.", "dirty, dirty2, and dirty3 are KOTOR 2-only values. Controller-specific directives are meaningful only after this line.", {"water", "life", "perlin", "arturo", "wave", "cycle", "random", "ringtexdistort", "dirty", "dirty2", "dirty3"}},
        {"filerange", "Basic texture", TxiDirectiveValueKind::SignedShort, "signed 16-bit integer · 0 single image; >0 sequence count", "0", "Loads a numbered image sequence and stacks the images into one texture.", "Negative values are not operationally useful.", {}},
        {"defaultwidth", "Basic texture", TxiDirectiveValueKind::SignedShort, "signed 16-bit integer · >0", "2", "Sets the fallback or procedural texture width.", "Frame width and height are independent metadata, but game TPC encoding requires square frames. A rectangular atlas with square cells is supported.", {}},
        {"defaultheight", "Basic texture", TxiDirectiveValueKind::SignedShort, "signed 16-bit integer · >0", "2", "Sets the fallback or procedural texture height.", "Frame height and width are independent metadata, but game TPC encoding requires square frames. A rectangular atlas with square cells is supported.", {}},
        {"downsamplemax", "Basic texture", TxiDirectiveValueKind::SignedShort, "signed 16-bit integer · >=0", "15", "Sets the highest dynamic downsample level.", "Normally greater than or equal to downsamplemin.", {}},
        {"downsamplemin", "Basic texture", TxiDirectiveValueKind::SignedShort, "signed 16-bit integer · >=0", "0", "Sets the lowest dynamic downsample level.", "Normally less than or equal to downsamplemax.", {}},
        {"mipmap", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "true", "Enables mipmap generation, upload, and memory accounting.", "", {}},
        {"filter", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "true", "Selects filtered rather than nearest-neighbor texture sampling.", "", {}},
        {"maptexelstopixels", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "false", "Requests texel-to-pixel alignment for screen-space or GUI textures.", "", {}},
        {"gamma", "Basic texture", TxiDirectiveValueKind::Float, "float · >0; 1.0 is neutral", "1.0", "Applies gamma correction while constructing the texture.", "", {}},
        {"isbumpmap", "Basic texture", TxiDirectiveValueKind::Integer, "integer · 0 none | 1 height bump | 2 authored normal/bump form", "0", "Classifies the texture as ordinary color data or one of the engine bump-map forms.", "Treat this as an enum, not a checkbox.", {"0", "1", "2"}},
        {"clamp", "Basic texture", TxiDirectiveValueKind::SignedShort, "signed 16-bit integer · 0 repeat | nonzero clamp", "0", "Controls texture wrapping versus edge clamping.", "", {}},
        {"alphamean", "Basic texture", TxiDirectiveValueKind::Float, "float · -1 auto; otherwise normally 0..1", "-1", "Overrides or requests mean-alpha calculation.", "", {}},
        {"isdiffusebumpmap", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "true", "Enables the diffuse-lighting contribution of a bump map.", "Only meaningful for a valid bump or normal map.", {}},
        {"isspecularbumpmap", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "true", "Enables the specular-lighting contribution of a bump map.", "Only meaningful for a valid bump or normal map.", {}},
        {"bumpmapscaling", "Basic texture", TxiDirectiveValueKind::Float, "float · normally >=0; 1.0 is neutral", "1.0", "Scales source height data during bump or normal conversion.", "", {}},
        {"specularcolor", "Basic texture", TxiDirectiveValueKind::Vector3, "vector3 · r g b, normally 0..1", "1 1 1", "Sets the specular reflection tint or multiplier.", "", {}},
        {"numx", "Basic texture", TxiDirectiveValueKind::SignedShort, "signed 16-bit integer · >0", "1", "Sets the horizontal tile or frame count.", "Cycle animation uses numx × numy frames.", {}},
        {"numy", "Basic texture", TxiDirectiveValueKind::SignedShort, "signed 16-bit integer · >0", "1", "Sets the vertical tile or frame count.", "Cycle animation uses numx × numy frames.", {}},
        {"cube", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "false", "Selects cube-map construction and upload.", "Cube faces must have compatible square dimensions.", {}},
        {"bumpintensity", "Basic texture", TxiDirectiveValueKind::Float, "float · normally >=0; 1.0 is neutral", "1.0", "Sets the overall bump strength.", "", {}},
        {"temporary", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "false", "Marks the texture as temporary or dynamic for lifetime and memory handling.", "", {}},
        {"useglobalalpha", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "false", "Uses global or material alpha behavior in addition to texture-local alpha.", "", {}},
        {"isenvironmentmapped", "Basic texture", TxiDirectiveValueKind::Boolean, "boolean · true | false | 1 | 0", "false", "Enables environment-map rendering behavior.", "Use with envmaptexture and envmapalpha.", {}},
        {"envmapalpha", "Basic texture", TxiDirectiveValueKind::Float, "float · normally 0..1", "1.0", "Sets environment-map alpha or strength.", "", {}},
        {"diffusebumpintensity", "Basic texture", TxiDirectiveValueKind::Float, "float · normally >=0; 1.0 is neutral", "1.0", "Scales diffuse bump-lighting strength.", "Only effective when isdiffusebumpmap is enabled.", {}},
        {"specularbumpintensity", "Basic texture", TxiDirectiveValueKind::Float, "float · normally >=0; 1.0 is neutral", "1.0", "Scales specular bump-lighting strength.", "Only effective when isspecularbumpmap is enabled.", {}},
        {"channelscale", "Base controller", TxiDirectiveValueKind::FloatList, "float list · count + rows, or rows followed by endlist", "empty", "Scales the generated signal into output channels.", "Entries map to R, G, B, and A in order.", {}},
        {"channeltranslate", "Base controller", TxiDirectiveValueKind::FloatList, "float list · count + rows, or rows followed by endlist", "empty", "Adds a per-channel bias after procedural signal scaling.", "Entries map to R, G, B, and A in order.", {}},
        {"distort", "Base controller", TxiDirectiveValueKind::Integer, "integer · 0 off | nonzero on", "0", "Enables post-generation spatial distortion.", "", {}},
        {"distortangle", "Base controller", TxiDirectiveValueKind::Integer, "integer · 0 signed offsets | nonzero angle mode", "1", "Selects how distortion bytes are interpreted as displacement vectors.", "", {}},
        {"distortionamplitude", "Base controller", TxiDirectiveValueKind::Float, "float · normally >=0, in texels", "5.0", "Sets spatial-distortion strength.", "", {}},
        {"speed", "Base controller", TxiDirectiveValueKind::Float, "float · 0 paused; normally >=0", "1.0", "Sets procedural animation or simulation speed.", "", {}},
        {"channelscale0", "Base controller", TxiDirectiveValueKind::Float, "float · >=0; R channel", "1.0 after indexed initialization", "Overrides generated-signal scale for the R channel.", "The first indexed channel directive initializes all four channel entries to 1.0.", {}},
        {"channelscale1", "Base controller", TxiDirectiveValueKind::Float, "float · >=0; G channel", "1.0 after indexed initialization", "Overrides generated-signal scale for the G channel.", "The first indexed channel directive initializes all four channel entries to 1.0.", {}},
        {"channelscale2", "Base controller", TxiDirectiveValueKind::Float, "float · >=0; B channel", "1.0 after indexed initialization", "Overrides generated-signal scale for the B channel.", "The first indexed channel directive initializes all four channel entries to 1.0.", {}},
        {"channelscale3", "Base controller", TxiDirectiveValueKind::Float, "float · >=0; A channel", "1.0 after indexed initialization", "Overrides generated-signal scale for the A channel.", "The first indexed channel directive initializes all four channel entries to 1.0.", {}},
        {"channeltranslate0", "Base controller", TxiDirectiveValueKind::Float, "float · >=0; R channel", "1.0 after indexed initialization", "Overrides generated-signal translation for the R channel.", "The first indexed channel directive initializes all four channel entries to 1.0.", {}},
        {"channeltranslate1", "Base controller", TxiDirectiveValueKind::Float, "float · >=0; G channel", "1.0 after indexed initialization", "Overrides generated-signal translation for the G channel.", "The first indexed channel directive initializes all four channel entries to 1.0.", {}},
        {"channeltranslate2", "Base controller", TxiDirectiveValueKind::Float, "float · >=0; B channel", "1.0 after indexed initialization", "Overrides generated-signal translation for the B channel.", "The first indexed channel directive initializes all four channel entries to 1.0.", {}},
        {"channeltranslate3", "Base controller", TxiDirectiveValueKind::Float, "float · >=0; A channel", "1.0 after indexed initialization", "Overrides generated-signal translation for the A channel.", "The first indexed channel directive initializes all four channel entries to 1.0.", {}},
        {"forcecyclespeed", "Water controller", TxiDirectiveValueKind::Float, "float · normally >=0", "57.29578", "Sets the cyclic force-injection rate for procedural water.", "Requires proceduretype water before this line.", {}},
        {"anglecyclespeed", "Water controller", TxiDirectiveValueKind::Float, "float · normally >=0", "2.86479", "Sets the water force-direction cycle rate.", "Requires proceduretype water before this line.", {}},
        {"waterwidth", "Water controller", TxiDirectiveValueKind::Integer, "integer · >0; 2^n-1 recommended", "31", "Sets the horizontal procedural-water simulation extent or mask.", "Requires proceduretype water before this line.", {}},
        {"waterheight", "Water controller", TxiDirectiveValueKind::Integer, "integer · >0; 2^n-1 recommended", "31", "Sets the vertical procedural-water simulation extent or mask.", "Requires proceduretype water before this line.", {}},
        {"arturowidth", "Arturo controller", TxiDirectiveValueKind::Integer, "integer · >0; 2^n-1 recommended", "15", "Sets the horizontal extent of the Arturo procedural image.", "Requires proceduretype arturo before this line.", {}},
        {"arturoheight", "Arturo controller", TxiDirectiveValueKind::Integer, "integer · >0; 2^n-1 recommended", "15", "Sets the vertical extent of the Arturo procedural image.", "Requires proceduretype arturo before this line.", {}},
        {"fps", "Cycle controller", TxiDirectiveValueKind::Float, "float · >0", "1.0", "Sets the playback rate for cycle animation.", "Requires proceduretype cycle; zero causes division by zero in the engine calculation.", {}},
        {"numchars", "Font metadata", TxiDirectiveValueKind::Integer, "integer · >=0", "0", "Sets the number of glyph records in the font atlas.", "Coordinate lists should contain at least this many entries.", {}},
        {"fontheight", "Font metadata", TxiDirectiveValueKind::Float, "float · normally >=0", "0", "Sets the core font or glyph height metric.", "", {}},
        {"baselineheight", "Font metadata", TxiDirectiveValueKind::Float, "float · normally >=0", "0", "Sets the baseline and vertical placement metric.", "", {}},
        {"texturewidth", "Font metadata", TxiDirectiveValueKind::Float, "float · >0", "0", "Sets the atlas-width metric used for glyph scaling and spacing.", "", {}},
        {"spacingr", "Font metadata", TxiDirectiveValueKind::Float, "float · normally >=0", "0", "Adds right-side horizontal glyph spacing.", "", {}},
        {"spacingb", "Font metadata", TxiDirectiveValueKind::Float, "float · normally >=0", "0", "Adds bottom or vertical spacing and contributes to ideal line height.", "", {}},
        {"upperleftcoords", "Font metadata", TxiDirectiveValueKind::Vector3List, "vector3 list · count + x y z rows, or rows followed by endlist", "empty", "Stores each glyph's upper-left atlas coordinate and third metadata component.", "X and Y are normally 0..1 and should not exceed the matching lower-right coordinate.", {}},
        {"lowerrightcoords", "Font metadata", TxiDirectiveValueKind::Vector3List, "vector3 list · count + x y z rows, or rows followed by endlist", "empty", "Stores each glyph's lower-right atlas coordinate and third metadata component.", "X and Y are normally 0..1 and should not precede the matching upper-left coordinate.", {}},
    };
    return catalog;
}

std::optional<TxiDirectiveInfo> findTxiDirective(const std::string& key) {
    const std::string lower = asciiLower(trimTxi(key));
    for (const auto& directive : txiDirectiveCatalog()) {
        if (directive.name == lower) return directive;
    }
    return std::nullopt;
}

std::vector<TxiEntry> parseTxiEntries(const std::string& txi) {
    std::vector<TxiEntry> entries;
    std::istringstream in(txi);
    std::string line;
    std::size_t lineNumber = 0;
    const TxiDirectiveInfo* activeList = nullptr;
    std::optional<std::size_t> rowsRemaining;
    bool openList = false;

    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trimTxi(line);

        if (activeList != nullptr) {
            TxiEntry entry;
            entry.lineNumber = lineNumber;
            entry.key = activeList->name;
            entry.value = trimmed;
            if (openList && beginsEndList(trimmed)) {
                entry.listTerminator = true;
                activeList = nullptr;
                openList = false;
            } else {
                entry.listData = true;
                if (rowsRemaining && *rowsRemaining > 0) {
                    --(*rowsRemaining);
                    if (*rowsRemaining == 0) {
                        activeList = nullptr;
                        rowsRemaining.reset();
                    }
                }
            }
            entries.push_back(std::move(entry));
            continue;
        }

        TxiEntry entry;
        entry.lineNumber = lineNumber;
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            entry.blankOrComment = true;
            entries.push_back(std::move(entry));
            continue;
        }

        std::istringstream fields(trimmed);
        fields >> entry.key;
        entry.key = asciiLower(entry.key);
        std::getline(fields, entry.value);
        entry.value = trimTxi(entry.value);
        if (entry.key == "decal1") {
            entry.key = "decal";
            if (entry.value.empty()) entry.value = "1";
        }
        entries.push_back(entry);

        const auto directive = findTxiDirective(entry.key);
        if (!directive || !isListKind(directive->valueKind)) continue;
        if (entry.value.empty()) {
            activeList = &*std::find_if(txiDirectiveCatalog().begin(), txiDirectiveCatalog().end(),
                [&](const TxiDirectiveInfo& item) { return item.name == directive->name; });
            openList = true;
            rowsRemaining.reset();
        } else if (const auto count = parseDecimalInteger(entry.value); count && *count >= 0) {
            if (*count != 0) {
                activeList = &*std::find_if(txiDirectiveCatalog().begin(), txiDirectiveCatalog().end(),
                    [&](const TxiDirectiveInfo& item) { return item.name == directive->name; });
                rowsRemaining = static_cast<std::size_t>(*count);
                openList = false;
            }
        }
    }
    return entries;
}

std::vector<TxiValidationIssue> validateTxiText(const std::string& txi) {
    std::vector<TxiValidationIssue> issues;
    std::istringstream in(txi);
    std::string line;
    std::size_t lineNumber = 0;
    std::string currentProcedure;
    std::optional<long> downsampleMin;
    std::optional<long> downsampleMax;
    std::optional<long> numChars;
    std::optional<std::size_t> upperLeftRows;
    std::optional<std::size_t> lowerRightRows;

    struct ActiveList {
        TxiDirectiveInfo directive;
        std::size_t startLine = 0;
        std::optional<std::size_t> remaining;
        bool openEnded = false;
        std::size_t rowsSeen = 0;
    };
    std::optional<ActiveList> activeList;

    auto finishList = [&]() {
        if (!activeList) return;
        if (activeList->directive.name == "upperleftcoords") upperLeftRows = activeList->rowsSeen;
        if (activeList->directive.name == "lowerrightcoords") lowerRightRows = activeList->rowsSeen;
        activeList.reset();
    };

    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trimTxi(line);

        if (activeList) {
            if (activeList->openEnded && beginsEndList(trimmed)) {
                finishList();
                continue;
            }
            if (!activeList->openEnded && beginsEndList(trimmed)) {
                addIssue(issues, TxiIssueSeverity::Error, lineNumber, activeList->directive.name,
                         "endlist appeared before the counted list was complete.");
                finishList();
                continue;
            }

            bool valid = false;
            if (activeList->directive.valueKind == TxiDirectiveValueKind::FloatList) {
                valid = parseSingleFloat(trimmed);
                if (!valid) {
                    addIssue(issues, TxiIssueSeverity::Error, lineNumber, activeList->directive.name,
                             "Expected one float on this list row.");
                }
            } else {
                valid = parseVector3(trimmed).has_value();
                if (!valid) {
                    addIssue(issues, TxiIssueSeverity::Error, lineNumber, activeList->directive.name,
                             "Expected three floats (x y z) on this list row.");
                }
            }
            (void)valid;
            ++activeList->rowsSeen;
            if (activeList->remaining && *activeList->remaining > 0) {
                --(*activeList->remaining);
                if (*activeList->remaining == 0) finishList();
            }
            continue;
        }

        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

        std::istringstream fields(trimmed);
        std::string key;
        fields >> key;
        key = asciiLower(key);
        std::string value;
        std::getline(fields, value);
        value = trimTxi(value);
        if (key == "decal1") {
            key = "decal";
            if (value.empty()) value = "1";
        }

        if (key == "endlist") {
            addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                     "endlist does not have a matching open list directive.");
            continue;
        }

        if (isNeoTpcCompatibilityDirective(key)) {
            if (!parseBoolean(value)) {
                addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                         "Expected true, false, 1, or 0.");
            }
            continue;
        }

        const auto directive = findTxiDirective(key);
        if (!directive) {
            addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                     "Unknown TXI directive. NeoTPC will preserve it, but the TXI dictionary cannot validate or complete it.");
            continue;
        }

        if (isListKind(directive->valueKind)) {
            if (isBaseControllerKey(key) && currentProcedure.empty()) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "Controller directive appears before proceduretype and will not have a controller to modify.");
            }
            if (value.empty()) {
                activeList = ActiveList{*directive, lineNumber, std::nullopt, true, 0};
            } else if (const auto count = parseDecimalInteger(value); count && *count >= 0) {
                if (*count == 0) {
                    if (directive->name == "upperleftcoords") upperLeftRows = 0;
                    if (directive->name == "lowerrightcoords") lowerRightRows = 0;
                } else {
                    activeList = ActiveList{*directive, lineNumber,
                                            static_cast<std::size_t>(*count), false, 0};
                }
            } else {
                addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                         "Expected a non-negative row count, or no value followed by rows and endlist.");
            }
            continue;
        }

        std::optional<long> integerValue;
        std::optional<double> floatValue;
        std::optional<std::vector<double>> vectorValue;
        bool parsed = true;

        switch (directive->valueKind) {
        case TxiDirectiveValueKind::Boolean:
            parsed = parseBoolean(value);
            if (!parsed) addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                                  "Expected true, false, 1, or 0.");
            break;
        case TxiDirectiveValueKind::Integer:
        case TxiDirectiveValueKind::SignedShort:
            integerValue = parseIntegerLike(value);
            parsed = integerValue.has_value();
            if (!parsed) {
                addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                         "Expected a decimal integer, true, or false.");
            } else if (directive->valueKind == TxiDirectiveValueKind::SignedShort &&
                       (*integerValue < -32768 || *integerValue > 32767)) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "The engine stores this as signed 16-bit; the parsed value will truncate or wrap.");
            }
            break;
        case TxiDirectiveValueKind::Float:
            floatValue = parseFloatValue(value);
            parsed = floatValue.has_value();
            if (!parsed) addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                                  "Expected a floating-point number.");
            break;
        case TxiDirectiveValueKind::Vector3:
            vectorValue = parseVector3(value);
            parsed = vectorValue.has_value();
            if (!parsed) addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                                  "Expected three floating-point values: x y z.");
            break;
        case TxiDirectiveValueKind::ResourceName:
            parsed = oneWhitespaceFreeToken(value);
            if (!parsed) addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                                  "Expected one whitespace-free resource-name token.");
            break;
        case TxiDirectiveValueKind::Enum:
            parsed = exactAllowedValue(*directive, value);
            if (!parsed) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "Value is not one of the exact lowercase values recognized by the engine.");
            }
            break;
        case TxiDirectiveValueKind::FloatList:
        case TxiDirectiveValueKind::Vector3List:
            break;
        }

        if (!parsed) {
            if (key == "proceduretype") currentProcedure.clear();
            continue;
        }

        if (key == "proceduretype") {
            currentProcedure = value;
        } else if ((isBaseControllerKey(key) || isWaterControllerKey(key) ||
                    isArturoControllerKey(key) || key == "fps") && currentProcedure.empty()) {
            addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                     "Controller directive appears before proceduretype and will not have a controller to modify.");
        } else if (isWaterControllerKey(key) && currentProcedure != "water") {
            addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                     "This directive is only active after proceduretype water.");
        } else if (isArturoControllerKey(key) && currentProcedure != "arturo") {
            addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                     "This directive is only active after proceduretype arturo.");
        } else if (key == "fps" && currentProcedure != "cycle") {
            addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                     "fps is only active after proceduretype cycle.");
        }

        if (integerValue) {
            const long number = *integerValue;
            if (key == "downsamplemin") downsampleMin = number;
            if (key == "downsamplemax") downsampleMax = number;
            if (key == "numchars") numChars = number;

            if ((key == "defaultwidth" || key == "defaultheight" || key == "numx" ||
                 key == "numy" || key == "waterwidth" || key == "waterheight" ||
                 key == "arturowidth" || key == "arturoheight") &&
                number <= 0) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "This value is normally greater than zero.");
            }
            if ((key == "downsamplemin" || key == "downsamplemax" || key == "numchars") &&
                number < 0) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "This value is normally non-negative.");
            }
            if (key == "filerange" && number < 0) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "Negative file ranges are not operationally useful.");
            }
            if (key == "isbumpmap" && number != 0 && number != 1 && number != 2) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "Recognized bump-map forms are 0, 1, and 2.");
            }
        }

        if (floatValue) {
            const double number = *floatValue;
            if (key == "fps" && number <= 0.0) {
                addIssue(issues, TxiIssueSeverity::Error, lineNumber, key,
                         "fps must be greater than zero.");
            } else if (key == "texturewidth" && number <= 0.0) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "texturewidth is normally greater than zero.");
            } else if (key == "gamma" && number <= 0.0) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "Gamma is normally greater than zero; 1.0 is neutral.");
            } else if ((key == "wateralpha" || key == "envmapalpha") &&
                       (number < 0.0 || number > 1.0)) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "This alpha value is normally between 0 and 1.");
            } else if (key == "alphamean" && number != -1.0 &&
                       (number < 0.0 || number > 1.0)) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "Use -1 for automatic alpha mean, or normally a value between 0 and 1.");
            } else if ((key == "bumpmapscaling" || key == "bumpintensity" ||
                        key == "diffusebumpintensity" || key == "specularbumpintensity" ||
                        key == "distortionamplitude" || key == "speed" ||
                        key == "forcecyclespeed" || key == "anglecyclespeed" ||
                        key == "fontheight" || key == "baselineheight" ||
                        key == "spacingr" || key == "spacingb" ||
                        startsWithIgnoreCase(key, "channelscale") ||
                        startsWithIgnoreCase(key, "channeltranslate")) && number < 0.0) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "This value is normally non-negative.");
            }
        }

        if (vectorValue && key == "specularcolor") {
            if (std::any_of(vectorValue->begin(), vectorValue->end(), [](double component) {
                    return component < 0.0 || component > 1.0;
                })) {
                addIssue(issues, TxiIssueSeverity::Warning, lineNumber, key,
                         "Specular color components are normally between 0 and 1.");
            }
        }
    }

    if (activeList) {
        if (activeList->openEnded) {
            addIssue(issues, TxiIssueSeverity::Error, activeList->startLine,
                     activeList->directive.name, "Open list is missing its endlist terminator.");
        } else {
            addIssue(issues, TxiIssueSeverity::Error, activeList->startLine,
                     activeList->directive.name,
                     "Counted list ended early; " + std::to_string(*activeList->remaining) +
                         " row(s) are missing.");
        }
    }

    if (downsampleMin && downsampleMax && *downsampleMin > *downsampleMax) {
        addIssue(issues, TxiIssueSeverity::Warning, 0, "downsamplemin/downsamplemax",
                 "downsamplemin is greater than downsamplemax.");
    }
    if (numChars && *numChars >= 0) {
        if (upperLeftRows && *upperLeftRows < static_cast<std::size_t>(*numChars)) {
            addIssue(issues, TxiIssueSeverity::Warning, 0, "upperleftcoords",
                     "The coordinate list contains fewer rows than numchars.");
        }
        if (lowerRightRows && *lowerRightRows < static_cast<std::size_t>(*numChars)) {
            addIssue(issues, TxiIssueSeverity::Warning, 0, "lowerrightcoords",
                     "The coordinate list contains fewer rows than numchars.");
        }
    }
    return issues;
}

std::string txiValidationReport(const std::string& txi) {
    const auto issues = validateTxiText(txi);
    std::size_t errors = 0, warnings = 0, infos = 0;
    for (const auto& issue : issues) {
        if (issue.severity == TxiIssueSeverity::Error) ++errors;
        else if (issue.severity == TxiIssueSeverity::Warning) ++warnings;
        else ++infos;
    }
    std::size_t directives = 0;
    for (const auto& entry : parseTxiEntries(txi)) {
        if (!entry.blankOrComment && !entry.listData && !entry.listTerminator) ++directives;
    }
    std::ostringstream out;
    out << "TXI validation report\n";
    out << "status: " << errors << " error(s), " << warnings << " warning(s), " << infos
        << " info note(s) across " << directives << " directive(s).\n";
    if (issues.empty()) {
        out << "OK: all directives matched the TXI dictionary.\n";
        return out.str();
    }
    out << "line\tseverity\tkey\tmessage\n";
    for (const auto& issue : issues) {
        out << issue.lineNumber << '\t' << txiIssueSeverityToString(issue.severity) << '\t'
            << issue.key << '\t' << issue.message << '\n';
    }
    return out.str();
}

std::string txiCatalogReport(const std::optional<std::string>& key) {
    std::ostringstream out;
    if (key) {
        const auto directive = findTxiDirective(*key);
        if (!directive) return "Unknown TXI directive: " + *key + "\n";
        out << directive->name << '\n'
            << "category: " << directive->category << '\n'
            << "value: " << directive->valueHint << '\n'
            << "default: " << directive->defaultValue << '\n'
            << "does: " << directive->description << '\n';
        if (!directive->notes.empty()) out << "notes: " << directive->notes << '\n';
        return out.str();
    }

    out << "name\tvalue\tdefault\twhat it does\n";
    for (const auto& directive : txiDirectiveCatalog()) {
        out << directive.name << '\t' << directive.valueHint << '\t'
            << directive.defaultValue << '\t' << directive.description << '\n';
    }
    return out.str();
}

std::string txiKeyReferenceText(bool includeDetails) {
    if (!includeDetails) return txiCatalogReport(std::nullopt);
    std::ostringstream out;
    for (const auto& directive : txiDirectiveCatalog()) {
        out << txiCatalogReport(directive.name) << '\n';
    }
    return out.str();
}

std::string txiKeyReferenceText(const std::string& filter) {
    if (filter.empty()) return txiKeyReferenceText(false);
    const std::string needle = asciiLower(trimTxi(filter));
    if (const auto directive = findTxiDirective(needle)) return txiCatalogReport(directive->name);

    std::ostringstream out;
    for (const auto& directive : txiDirectiveCatalog()) {
        std::string hay = directive.name + " " + directive.category + " " + directive.valueHint +
                          " " + directive.defaultValue + " " + directive.description + " " +
                          directive.notes;
        for (const auto& value : directive.allowedValues) hay += " " + value;
        if (asciiLower(hay).find(needle) != std::string::npos) {
            out << directive.name << '\t' << directive.valueHint << '\t'
                << directive.defaultValue << '\t' << directive.description << '\n';
        }
    }
    const std::string result = out.str();
    return result.empty() ? ("No TXI dictionary matches for: " + filter + "\n") : result;
}

TxiAutocompleteResult txiAutocomplete(std::string_view lineBeforeCaret,
                                      bool includeAllDirectives) {
    TxiAutocompleteResult result;

    std::size_t first = 0;
    while (first < lineBeforeCaret.size() &&
           (lineBeforeCaret[first] == ' ' || lineBeforeCaret[first] == '\t')) {
        ++first;
    }

    if (first == lineBeforeCaret.size()) {
        if (!includeAllDirectives) return result;
        result.kind = TxiAutocompleteKind::Directive;
        for (const auto& directive : txiDirectiveCatalog()) {
            result.suggestions.push_back(directive.name);
        }
        sortAndUnique(result.suggestions);
        return result;
    }

    if (lineBeforeCaret[first] == '#' || lineBeforeCaret[first] == ';') return result;

    std::size_t keyEnd = first;
    while (keyEnd < lineBeforeCaret.size() &&
           lineBeforeCaret[keyEnd] != ' ' && lineBeforeCaret[keyEnd] != '\t') {
        ++keyEnd;
    }

    if (keyEnd == lineBeforeCaret.size()) {
        const std::string_view prefix = lineBeforeCaret.substr(first);
        if (prefix.empty() && !includeAllDirectives) return result;
        result.kind = TxiAutocompleteKind::Directive;
        result.replacementLength = prefix.size();
        for (const auto& directive : txiDirectiveCatalog()) {
            if (prefix.empty() || startsWithIgnoreCase(directive.name, prefix)) {
                result.suggestions.push_back(directive.name);
            }
        }
        sortAndUnique(result.suggestions);
        if (result.suggestions.empty()) result.kind = TxiAutocompleteKind::None;
        return result;
    }

    std::string key(lineBeforeCaret.substr(first, keyEnd - first));
    key = asciiLower(key);
    if (key == "decal1") key = "decal";
    const auto directive = findTxiDirective(key);
    if (!directive) return result;

    std::size_t valueStart = keyEnd;
    while (valueStart < lineBeforeCaret.size() &&
           (lineBeforeCaret[valueStart] == ' ' || lineBeforeCaret[valueStart] == '\t')) {
        ++valueStart;
    }

    std::size_t tokenStart = valueStart;
    for (std::size_t index = valueStart; index < lineBeforeCaret.size(); ++index) {
        const char ch = lineBeforeCaret[index];
        if (ch == ' ' || ch == '\t' || ch == ',' || ch == '(' || ch == ')' ||
            ch == '[' || ch == ']') {
            tokenStart = index + 1;
        }
    }
    const std::string_view prefix = lineBeforeCaret.substr(tokenStart);

    const auto candidates = valueSuggestions(*directive);
    if (candidates.empty()) return result;
    result.kind = TxiAutocompleteKind::Value;
    result.directive = directive->name;
    result.replacementLength = prefix.size();
    for (const auto& candidate : candidates) {
        if (prefix.empty() || startsWithIgnoreCase(candidate, prefix)) {
            result.suggestions.push_back(candidate);
        }
    }
    if (result.suggestions.empty()) result.kind = TxiAutocompleteKind::None;
    return result;
}

std::string txiDirectiveSignature(const std::string& key) {
    const auto directive = findTxiDirective(key);
    return directive ? directive->valueHint : std::string{};
}

std::string txiDirectiveHint(const std::string& key) {
    const auto directive = findTxiDirective(key);
    if (!directive) return {};
    std::ostringstream out;
    out << directive->name << "  " << directive->valueHint << " - " << directive->description;
    if (!directive->defaultValue.empty()) out << " Default: " << directive->defaultValue << '.';
    if (!directive->notes.empty()) out << ' ' << directive->notes;
    return out.str();
}

std::string txiValueHint(const std::string& key, const std::string& value) {
    const auto directive = findTxiDirective(key);
    if (!directive) return {};
    const std::string trimmed = trimTxi(value);
    const std::string detail = enumValueDescription(directive->name, trimmed);

    std::ostringstream out;
    out << directive->name << " = " << trimmed << " - ";
    if (!detail.empty()) {
        out << detail;
    } else if (directive->valueKind == TxiDirectiveValueKind::Boolean && parseBoolean(trimmed)) {
        const std::string lower = asciiLower(trimmed);
        out << ((lower == "1" || lower == "true") ? "Enabled." : "Disabled.");
    } else {
        out << directive->description;
    }
    if (!directive->notes.empty()) out << ' ' << directive->notes;
    return out.str();
}

} // namespace neotpc::texture
