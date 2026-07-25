// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture/Txi.hpp"

#include "texture/FileUtil.hpp"

#include <algorithm>
#include <cctype>
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
           (input[first] == '\0' || std::isspace(static_cast<unsigned char>(input[first])))) ++first;
    std::size_t last = input.size();
    while (last > first &&
           (input[last - 1] == '\0' || std::isspace(static_cast<unsigned char>(input[last - 1])))) --last;
    return input.substr(first, last - first);
}

bool parseIntStrict(const std::string& value) {
    const std::string trimmed = trimTxi(value);
    if (trimmed.empty()) return false;
    char* end = nullptr;
    (void)std::strtol(trimmed.c_str(), &end, 0);
    return end != nullptr && *end == '\0';
}

bool parseFloatStrict(const std::string& value) {
    const std::string trimmed = trimTxi(value);
    if (trimmed.empty()) return false;
    char* end = nullptr;
    (void)std::strtod(trimmed.c_str(), &end);
    return end != nullptr && *end == '\0';
}

bool parseBoolLike(const std::string& value) {
    const std::string lower = asciiLower(trimTxi(value));
    return lower == "0" || lower == "1" || lower == "true" || lower == "false" ||
           lower == "yes" || lower == "no" || lower == "on" || lower == "off";
}

bool parseBoolOrSmallInteger(const std::string& value) {
    if (!parseIntStrict(value)) return parseBoolLike(value);
    char* end = nullptr;
    const long parsed = std::strtol(trimTxi(value).c_str(), &end, 0);
    return parsed >= 0 && parsed <= 255;
}

bool validResRef(const std::string& value) {
    const std::string trimmed = trimTxi(value);
    if (trimmed.empty() || trimmed.size() > 16) return false;
    for (unsigned char ch : trimmed) {
        if (!(std::isalnum(ch) || ch == '_' || ch == '-')) return false;
    }
    return true;
}

bool parseNumericList(std::string value) {
    for (char& ch : value) {
        if (ch == ',' || ch == '(' || ch == ')' || ch == '[' || ch == ']') ch = ' ';
    }
    std::istringstream in(value);
    std::string token;
    bool any = false;
    while (in >> token) {
        any = true;
        if (!parseFloatStrict(token)) return false;
    }
    return any;
}

std::optional<std::size_t> parseCoordinateCount(const std::string& value) {
    const std::string trimmed = trimTxi(value);
    if (!parseIntStrict(trimmed)) return std::nullopt;
    char* end = nullptr;
    const long parsed = std::strtol(trimmed.c_str(), &end, 0);
    if (parsed < 0) return std::nullopt;
    return static_cast<std::size_t>(parsed);
}

bool parseCoordinateRow(std::string value) {
    for (char& ch : value) {
        if (ch == ',' || ch == '(' || ch == ')' || ch == '[' || ch == ']') ch = ' ';
    }
    std::istringstream in(value);
    std::string x, y, character, extra;
    if (!(in >> x >> y >> character) || (in >> extra)) return false;
    return parseFloatStrict(x) && parseFloatStrict(y) && parseIntStrict(character);
}

bool containsAllowed(const std::vector<std::string>& values, const std::string& value) {
    const std::string lower = asciiLower(trimTxi(value));
    return std::any_of(values.begin(), values.end(), [&](const std::string& candidate) {
        return asciiLower(candidate) == lower;
    });
}

bool isControllerKey(const std::string& key) {
    static const std::set<std::string> keys = {
        "channelscale", "channeltranslate", "distort", "distortangle", "distortionamplitude", "speed",
        "channelscale0", "channelscale1", "channelscale2", "channelscale3",
        "channeltranslate0", "channeltranslate1", "channeltranslate2", "channeltranslate3",
        "fps"
    };
    return keys.count(key) != 0;
}

bool isWaterControllerKey(const std::string& key) {
    static const std::set<std::string> keys = {"forcecyclespeed", "anglecyclespeed", "waterwidth", "waterheight"};
    return keys.count(key) != 0;
}

bool isArturoControllerKey(const std::string& key) {
    static const std::set<std::string> keys = {"arturowidth", "arturoheight"};
    return keys.count(key) != 0;
}

void addIssue(std::vector<TxiValidationIssue>& issues,
              TxiIssueSeverity severity,
              std::size_t line,
              const std::string& key,
              std::string message) {
    issues.push_back(TxiValidationIssue{severity, line, key, std::move(message)});
}

const std::map<std::string, std::string>& valueTokenParents() {
    static const std::map<std::string, std::string> parents = [] {
        std::map<std::string, std::string> out;
        for (const auto& value : std::vector<std::string>{"additive", "punchthrough"}) out[value] = "blending";
        for (const auto& value : std::vector<std::string>{"dirty", "dirty2", "dirty3", "water", "life", "perlin", "arturo", "wave", "cycle", "random", "ringtexdistort"}) out[value] = "proceduretype";
        return out;
    }();
    return parents;
}

} // namespace

std::string txiDirectiveValueKindToString(TxiDirectiveValueKind kind) {
    switch (kind) {
    case TxiDirectiveValueKind::Boolean: return "boolean";
    case TxiDirectiveValueKind::BooleanOrInteger: return "boolean-or-integer";
    case TxiDirectiveValueKind::Integer: return "integer";
    case TxiDirectiveValueKind::Float: return "float";
    case TxiDirectiveValueKind::ResRef: return "resref";
    case TxiDirectiveValueKind::Enum: return "enum";
    case TxiDirectiveValueKind::NumericList: return "numeric-list";
    case TxiDirectiveValueKind::CoordinateBlockCount: return "coordinate-block-count";
    case TxiDirectiveValueKind::FreeText: return "text";
    case TxiDirectiveValueKind::ValueToken: return "value-token";
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
        {"bumpmaptexture", "material key", "Material::ParseField", "active", "none", "base/diffuse texture TXI", "Tells the material which extra image holds bumps or normals.", "Referenced texture must identify itself with isbumpmap 1 for legacy bump or isbumpmap 2 for patched authored normal map.", TxiDirectiveValueKind::ResRef, {}},
        {"bumpyshinytexture", "material key", "Material::ParseField", "active", "none", "base/diffuse texture TXI", "Adds the shiny/reflection texture slot for bumpy-shiny effects.", "This is not the normal-map association key. Use alongside bumpmaptexture for legacy water/bumpy-shiny.", TxiDirectiveValueKind::ResRef, {}},
        {"envmaptexture", "material key", "Material::ParseField", "active", "none", "base/diffuse texture TXI", "Makes the surface use a fake reflection texture.", "Custom env textures usually need cube 1 on the env texture itself.", TxiDirectiveValueKind::ResRef, {}},
        {"blending", "material key", "Material::ParseField", "active", "default blend src-alpha / one-minus-src-alpha", "base/diffuse texture TXI", "Controls transparency/glow style.", "additive and punchthrough are values, not standalone keys.", TxiDirectiveValueKind::Enum, {"additive", "punchthrough"}},
        {"additive", "blending value", "Material::ParseField", "active value", "n/a", "value of blending on base/diffuse TXI", "Glow/add light on top of the background.", "Dark RGB adds little; alpha scales the add.", TxiDirectiveValueKind::ValueToken, {}},
        {"punchthrough", "blending value", "Material::ParseField", "active value", "n/a", "value of blending on base/diffuse TXI", "Hard holes/cutouts such as hair or grates.", "Alpha is used as a cutout/mask rather than smooth glass.", TxiDirectiveValueKind::ValueToken, {}},
        {"decal", "material key", "Material::ParseField", "active", "0", "base/diffuse texture TXI", "Draw like a sticker/overlay.", "Often paired with alpha.", TxiDirectiveValueKind::Boolean, {}},
        {"candownsample", "texture key", "legacy engine/tool parser", "active", "1", "texture itself", "Allows the engine or platform tools to reduce this texture's resolution.", "Use 0 for assets whose exact pixel dimensions must be preserved.", TxiDirectiveValueKind::Boolean, {}},
        {"caretindent", "font key", "font texture parser", "active for font textures", "0", "font atlas TXI", "Sets the text-caret inset used by the font.", "Font-atlas metadata; preserve values from shipped fonts when unsure.", TxiDirectiveValueKind::Float, {}},
        {"codepage", "font key", "font texture parser", "active for font textures", "0", "font atlas TXI", "Identifies the character code page represented by the atlas.", "Works with double-byte and glyph-sheet metadata.", TxiDirectiveValueKind::Integer, {}},
        {"cols", "font key", "font texture parser", "active for font textures", "0", "font atlas TXI", "Number of glyph columns in the atlas.", "Use with rows and numcharspersheet.", TxiDirectiveValueKind::Integer, {}},
        {"controllerscript", "texture key", "legacy engine/tool parser", "active in supported runtimes", "none", "procedural texture TXI", "Names a script/controller associated with the texture.", "Runtime support varies; preserve custom values verbatim.", TxiDirectiveValueKind::FreeText, {}},
        {"dbmapping", "texture key", "legacy engine/tool parser", "active in supported runtimes", "0", "texture itself", "Enables legacy database/resource mapping behavior.", "Obscure compatibility flag; normally copied from an existing asset.", TxiDirectiveValueKind::BooleanOrInteger, {}},
        {"defaultbpp", "texture key", "legacy engine/tool parser", "active", "32", "texture itself", "Declares the preferred generated texture bit depth.", "Mostly relevant to legacy conversion/platform paths.", TxiDirectiveValueKind::Integer, {}},
        {"downsamplefactor", "texture key", "legacy engine/tool parser", "active", "1", "texture itself", "Sets an explicit texture downsample scale.", "Interacts with candownsample and min/max size limits.", TxiDirectiveValueKind::Float, {}},
        {"renderbmlmtype", "material key", "Material::ParseField", "parser-confirmed; downstream use not found", "1", "base/diffuse texture TXI", "Obscure legacy renderer selector.", "Copy vanilla only; do not use to solve normal-map issues.", TxiDirectiveValueKind::Integer, {}},
        {"wateralpha", "material key", "Material::ParseField", "active", "1.0", "base/diffuse texture TXI", "Make water/surface treated as translucent.", "Render ordering effect, not a bump/normal association.", TxiDirectiveValueKind::Float, {}},
        {"proceduretype", "texture key", "CAurTextureBasic::ParseField", "active", "none", "procedural/animated texture TXI", "Turns a texture into an animated/generated texture.", "Must appear before controller-specific keys such as speed, distort, fps, waterwidth.", TxiDirectiveValueKind::Enum, {"dirty", "dirty2", "dirty3", "water", "life", "perlin", "arturo", "wave", "cycle", "random", "ringtexdistort"}},
        {"dirty", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"dirty2", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"dirty3", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"water", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"life", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"perlin", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"arturo", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"wave", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"cycle", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"random", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"ringtexdistort", "proceduretype value", "CAurTextureBasic::ParseField", "active value", "n/a", "value of proceduretype", "Chooses the kind of animation/generated texture.", "Value, not standalone key.", TxiDirectiveValueKind::ValueToken, {}},
        {"filerange", "texture key", "CAurTextureBasic::ParseField", "active", "0", "animated/multi-file texture TXI", "Use multiple numbered files as frames.", "Alternative to atlas animation; interacts with numy/height.", TxiDirectiveValueKind::Integer, {}},
        {"defaultwidth", "texture key", "CAurTextureBasic::ParseField", "active/fallback", "2", "animated/procedural texture TXI", "Default generated frame width.", "Use with defaultheight, numx/numy, proceduretype when needed.", TxiDirectiveValueKind::Integer, {}},
        {"defaultheight", "texture key", "CAurTextureBasic::ParseField", "active/fallback", "2", "animated/procedural texture TXI", "Default generated frame height.", "Use with defaultwidth.", TxiDirectiveValueKind::Integer, {}},
        {"downsamplemax", "texture key", "CAurTextureBasic::ParseField", "active", "15", "texture itself", "How far the engine may shrink the texture.", "Lower values preserve quality but use more memory.", TxiDirectiveValueKind::Integer, {}},
        {"downsamplemin", "texture key", "CAurTextureBasic::ParseField", "active", "0", "texture itself", "Start already shrunk.", "Rarely needed for Override mods.", TxiDirectiveValueKind::Integer, {}},
        {"mipmap", "texture key", "CAurTextureBasic::ParseField", "active", "1", "texture itself", "Use smaller copies at distance to reduce shimmer.", "For authored normal maps, mipmaps reduce shimmer but bad mip generation can soften normal detail.", TxiDirectiveValueKind::Boolean, {}},
        {"filter", "texture key", "CAurTextureBasic::ParseField", "active", "1", "texture itself", "Smooth between texture pixels instead of blocky sampling.", "Quality control, not a render-path selector. Does not fix invisible surfaces.", TxiDirectiveValueKind::Boolean, {}},
        {"maptexelstopixels", "texture key", "CAurTextureBasic::ParseField", "parser-confirmed; consumer low confidence", "0", "UI/exact-pixel texture itself", "Try to align texels to pixels.", "Likely UI/special-case; avoid for model textures unless copying vanilla.", TxiDirectiveValueKind::Boolean, {}},
        {"gamma", "texture key", "CAurTextureBasic::ParseField", "active", "1.0", "texture itself", "Change brightness curve before upload.", "Do not apply to RGB normal maps; it corrupts vector directions.", TxiDirectiveValueKind::Float, {}},
        {"isdoublebyte", "font key", "font texture parser", "active for font textures", "0", "font atlas TXI", "Marks a font atlas as containing double-byte characters.", "Use with codepage and glyph-count metadata.", TxiDirectiveValueKind::Boolean, {}},
        {"islightmap", "texture key", "legacy engine/tool parser", "active", "0", "lightmap texture TXI", "Marks the texture as a lightmap rather than ordinary color art.", "Lightmaps have different color/quality expectations than diffuse textures.", TxiDirectiveValueKind::Boolean, {}},
        {"envmapalpha", "texture key", "CAurTextureBasic::ParseField", "parser-confirmed; consumer low confidence", "1.0", "env-related texture itself", "Possible env/reflection strength knob.", "Use base texture alpha/envmaptexture first; treat as obscure.", TxiDirectiveValueKind::Float, {}},
        {"isbumpmap", "texture key", "CAurTextureBasic::ParseField", "active", "0", "bump or normal texture TXI", "Marks this texture as a bump/detail texture rather than a color texture.", "Type 1 works with bumpmapscaling; type 2 should be RGB/RGBA normal map and should not use bumpmapscaling.", TxiDirectiveValueKind::BooleanOrInteger, {}},
        {"clamp", "texture key", "CAurTextureBasic::ParseField", "active", "0", "texture itself", "Do not tile past texture edges.", "Use on UI/decals/masks; avoid on tiling floors/walls unless intended.", TxiDirectiveValueKind::BooleanOrInteger, {}},
        {"alphamean", "texture key", "CAurTextureBasic::ParseField", "active", "-1.0 auto", "texture itself", "Tell engine how transparent it is on average.", "Rarely set manually; can affect transparent sort behavior.", TxiDirectiveValueKind::Float, {}},
        {"isdiffusebumpmap", "texture key", "CAurTextureBasic::ParseField", "active", "1", "bump/normal texture TXI", "Let bumps affect ordinary light/dark shading.", "Not required if default true; put on bump/normal texture, not base texture.", TxiDirectiveValueKind::BooleanOrInteger, {}},
        {"isspecularbumpmap", "texture key", "CAurTextureBasic::ParseField", "active", "1", "bump/normal texture TXI", "Let bumps affect shiny highlights.", "Diffuse alpha inversely masks stock normal-map specular; alpha 255 suppresses it.", TxiDirectiveValueKind::BooleanOrInteger, {}},
        {"specularcolor", "texture key", "CAurTextureBasic::ParseField", "active", "1 1 1", "bump/normal texture TXI", "Tint the shine color.", "Works with isspecularbumpmap/specularbumpintensity.", TxiDirectiveValueKind::NumericList, {}},
        {"numx", "texture key", "CAurTextureBasic::ParseField", "active", "1", "animated atlas / GUI texture", "Number of frames across the image.", "Use with numy and fps for proceduretype cycle.", TxiDirectiveValueKind::Integer, {}},
        {"numy", "texture key", "CAurTextureBasic::ParseField", "active", "1", "animated atlas / GUI texture", "Number of frames down the image.", "Use with numx and fps.", TxiDirectiveValueKind::Integer, {}},
        {"cube", "texture key", "CAurTextureBasic::ParseField", "active if hardware/cap allows", "0", "cubemap/env texture TXI", "This texture is a six-sided reflection cube.", "Use on custom env maps referenced by envmaptexture/bumpyshinytexture.", TxiDirectiveValueKind::Boolean, {}},
        {"bumpintensity", "texture key", "CAurTextureBasic::ParseField", "active in legacy paths", "1.0", "legacy bump texture TXI", "Old bump-strength knob.", "For authored normal maps prefer diffusebumpintensity/specularbumpintensity or bake strength into map.", TxiDirectiveValueKind::Float, {}},
        {"temporary", "texture key", "CAurTextureBasic::ParseField", "active", "0", "temporary/internal texture", "Mark as temporary/internal.", "Do not use for ordinary override assets.", TxiDirectiveValueKind::Boolean, {}},
        {"useglobalalpha", "texture key", "CAurTextureBasic::ParseField", "partly active", "0", "texture itself", "Use a shared/global alpha instead of only texture alpha.", "Special-case/UI-ish; use cautiously.", TxiDirectiveValueKind::Boolean, {}},
        {"isenvironmentmapped", "texture key", "CAurTextureBasic::ParseField", "active/obscure", "0", "texture itself", "Mark texture as reflection-related.", "Usually set envmaptexture on base material instead.", TxiDirectiveValueKind::Boolean, {}},
        {"bumpmapscaling", "texture key", "CAurTextureBasic::ParseField", "active for legacy bump only", "1.0", "isbumpmap 1 texture TXI", "Make old height bumps taller/flatter.", "Do not use for isbumpmap 2 authored normals.", TxiDirectiveValueKind::Float, {}},
        {"diffusebumpintensity", "texture key", "CAurTextureBasic::ParseField", "active with patch", "1.0", "bump/normal texture TXI", "Strength of normal detail in ordinary lighting.", "Optional if default 1.0; place on normal/bump texture.", TxiDirectiveValueKind::Float, {}},
        {"specularbumpintensity", "texture key", "CAurTextureBasic::ParseField", "active", "1.0", "bump/normal texture TXI", "Strength of shiny normal-map highlight.", "Diffuse alpha still masks specular inversely in stock/patched normal FP.", TxiDirectiveValueKind::Float, {}},
        {"maxsizehq", "platform texture key", "legacy conversion parser", "active", "0", "texture itself", "Maximum high-quality texture dimension.", "Platform/downsample metadata; preserve shipped values when converting.", TxiDirectiveValueKind::Integer, {}},
        {"maxsizelq", "platform texture key", "legacy conversion parser", "active", "0", "texture itself", "Maximum low-quality texture dimension.", "Platform/downsample metadata; preserve shipped values when converting.", TxiDirectiveValueKind::Integer, {}},
        {"minsizehq", "platform texture key", "legacy conversion parser", "active", "0", "texture itself", "Minimum high-quality texture dimension.", "Platform/downsample metadata; preserve shipped values when converting.", TxiDirectiveValueKind::Integer, {}},
        {"minsizelq", "platform texture key", "legacy conversion parser", "active", "0", "texture itself", "Minimum low-quality texture dimension.", "Platform/downsample metadata; preserve shipped values when converting.", TxiDirectiveValueKind::Integer, {}},
        {"ondemand", "texture key", "legacy engine/tool parser", "active", "0", "texture itself", "Requests lazy/on-demand texture loading.", "Resource-management hint; it does not alter pixels.", TxiDirectiveValueKind::Boolean, {}},
        {"priority", "texture key", "legacy engine/tool parser", "active", "0", "texture itself", "Sets a legacy texture loading priority.", "Resource-management hint; copy known-good values.", TxiDirectiveValueKind::Integer, {}},
        {"channelscale", "controller key", "TextureController::ParseField", "active when controller exists", "empty -> per-use defaults", "after proceduretype on procedural texture TXI", "Scale generated channels.", "Put proceduretype before this line, otherwise no controller exists yet.", TxiDirectiveValueKind::NumericList, {}},
        {"channeltranslate", "controller key", "TextureController::ParseField", "active when controller exists", "empty -> 0 offsets", "after proceduretype on procedural texture TXI", "Shift generated channel brightness.", "Put proceduretype before this line, otherwise no controller exists yet.", TxiDirectiveValueKind::NumericList, {}},
        {"distort", "controller key", "TextureController::ParseField", "active when controller exists", "0", "after proceduretype on procedural texture TXI", "Wobble/warp the procedural texture.", "Put proceduretype before this line, otherwise no controller exists yet.", TxiDirectiveValueKind::Boolean, {}},
        {"distortangle", "controller key", "TextureController::ParseField", "active when controller exists", "1", "after proceduretype on procedural texture TXI", "Change how the wobble direction is calculated.", "Put proceduretype before this line, otherwise no controller exists yet.", TxiDirectiveValueKind::Float, {}},
        {"distortionamplitude", "controller key", "TextureController::ParseField", "active when controller exists", "5.0", "after proceduretype on procedural texture TXI", "How strong the wobble is.", "Put proceduretype before this line, otherwise no controller exists yet.", TxiDirectiveValueKind::Float, {}},
        {"speed", "controller key", "TextureController::ParseField", "active when controller exists", "1.0", "after proceduretype on procedural texture TXI", "How fast the effect animates.", "Put proceduretype before this line, otherwise no controller exists yet.", TxiDirectiveValueKind::Float, {}},
        {"channelscale0", "controller key", "TextureController::ParseField", "active when controller exists", "unset", "after proceduretype on procedural texture TXI", "Scale only channel 0 of generated output.", "Index 0/1/2/3 = R/G/B/A.", TxiDirectiveValueKind::NumericList, {}},
        {"channelscale1", "controller key", "TextureController::ParseField", "active when controller exists", "unset", "after proceduretype on procedural texture TXI", "Scale only channel 1 of generated output.", "Index 0/1/2/3 = R/G/B/A.", TxiDirectiveValueKind::NumericList, {}},
        {"channelscale2", "controller key", "TextureController::ParseField", "active when controller exists", "unset", "after proceduretype on procedural texture TXI", "Scale only channel 2 of generated output.", "Index 0/1/2/3 = R/G/B/A.", TxiDirectiveValueKind::NumericList, {}},
        {"channelscale3", "controller key", "TextureController::ParseField", "active when controller exists", "unset", "after proceduretype on procedural texture TXI", "Scale only channel 3 of generated output.", "Index 0/1/2/3 = R/G/B/A.", TxiDirectiveValueKind::NumericList, {}},
        {"channeltranslate0", "controller key", "TextureController::ParseField", "active when controller exists", "unset", "after proceduretype on procedural texture TXI", "Offset only channel 0 of generated output.", "Index 0/1/2/3 = R/G/B/A.", TxiDirectiveValueKind::NumericList, {}},
        {"channeltranslate1", "controller key", "TextureController::ParseField", "active when controller exists", "unset", "after proceduretype on procedural texture TXI", "Offset only channel 1 of generated output.", "Index 0/1/2/3 = R/G/B/A.", TxiDirectiveValueKind::NumericList, {}},
        {"channeltranslate2", "controller key", "TextureController::ParseField", "active when controller exists", "unset", "after proceduretype on procedural texture TXI", "Offset only channel 2 of generated output.", "Index 0/1/2/3 = R/G/B/A.", TxiDirectiveValueKind::NumericList, {}},
        {"channeltranslate3", "controller key", "TextureController::ParseField", "active when controller exists", "unset", "after proceduretype on procedural texture TXI", "Offset only channel 3 of generated output.", "Index 0/1/2/3 = R/G/B/A.", TxiDirectiveValueKind::NumericList, {}},
        {"forcecyclespeed", "water controller key", "WaterTextureController::ParseField", "active only after proceduretype water", "57.295776", "procedural water texture TXI", "How fast ripple force changes.", "proceduretype water must precede this key.", TxiDirectiveValueKind::Float, {}},
        {"anglecyclespeed", "water controller key", "WaterTextureController::ParseField", "active only after proceduretype water", "2.8647888", "procedural water texture TXI", "How fast ripple direction changes.", "proceduretype water must precede this key.", TxiDirectiveValueKind::Float, {}},
        {"waterwidth", "water controller key", "WaterTextureController::ParseField", "active only after proceduretype water", "31", "procedural water texture TXI", "Ripple grid width.", "proceduretype water must precede this key.", TxiDirectiveValueKind::Integer, {}},
        {"waterheight", "water controller key", "WaterTextureController::ParseField", "active only after proceduretype water", "31", "procedural water texture TXI", "Ripple grid height.", "proceduretype water must precede this key.", TxiDirectiveValueKind::Integer, {}},
        {"arturowidth", "arturo controller key", "ArturoTextureController::ParseField", "active only after proceduretype arturo", "31", "procedural arturo texture TXI", "Generated effect grid width.", "proceduretype arturo must precede this key.", TxiDirectiveValueKind::Integer, {}},
        {"arturoheight", "arturo controller key", "ArturoTextureController::ParseField", "active only after proceduretype arturo", "31", "procedural arturo texture TXI", "Generated effect grid height.", "proceduretype arturo must precede this key.", TxiDirectiveValueKind::Integer, {}},
        {"fps", "cycle controller key", "CycleTIDTextureController::ParseField", "active only after proceduretype cycle", "1.0", "animated cycle texture TXI", "Animation playback speed.", "Also appears in emitter/model parser, but as TXI it is cycle controller only.", TxiDirectiveValueKind::Float, {}},
        {"numchars", "font key", "CAurFontInfo::ParseField", "active for font textures", "0/empty", "font atlas TXI", "Number of glyphs/characters.", "CAurFontInfo is allocated for textures during Init and discarded if no coords are parsed.", TxiDirectiveValueKind::Integer, {}},
        {"fontheight", "font key", "CAurFontInfo::ParseField", "active for font textures", "0/empty", "font atlas TXI", "Font height metric.", "CAurFontInfo is allocated for textures during Init and discarded if no coords are parsed.", TxiDirectiveValueKind::Float, {}},
        {"baselineheight", "font key", "CAurFontInfo::ParseField", "active for font textures", "0/empty", "font atlas TXI", "Where letters sit vertically.", "CAurFontInfo is allocated for textures during Init and discarded if no coords are parsed.", TxiDirectiveValueKind::Float, {}},
        {"texturewidth", "font key", "CAurFontInfo::ParseField", "active for font textures", "0/empty", "font atlas TXI", "Atlas width metric.", "CAurFontInfo is allocated for textures during Init and discarded if no coords are parsed.", TxiDirectiveValueKind::Float, {}},
        {"spacingr", "font key", "CAurFontInfo::ParseField", "active for font textures", "0/empty", "font atlas TXI", "Right-side spacing.", "CAurFontInfo is allocated for textures during Init and discarded if no coords are parsed.", TxiDirectiveValueKind::Float, {}},
        {"spacingb", "font key", "CAurFontInfo::ParseField", "active for font textures", "0/empty", "font atlas TXI", "Bottom spacing.", "CAurFontInfo is allocated for textures during Init and discarded if no coords are parsed.", TxiDirectiveValueKind::Float, {}},
        {"fontwidth", "font key", "font texture parser", "active for font textures", "0/empty", "font atlas TXI", "Font width metric.", "Use with fontheight and atlas coordinate records.", TxiDirectiveValueKind::Float, {}},
        {"numcharspersheet", "font key", "font texture parser", "active for font textures", "0/empty", "font atlas TXI", "Number of characters stored on each font sheet.", "Use with rows, cols, and codepage metadata.", TxiDirectiveValueKind::Integer, {}},
        {"rows", "font key", "font texture parser", "active for font textures", "0/empty", "font atlas TXI", "Number of glyph rows in the atlas.", "Use with cols and numcharspersheet.", TxiDirectiveValueKind::Integer, {}},
        {"upperleftcoords", "font key", "CAurFontInfo::ParseField", "active for font textures", "0/empty", "font atlas TXI", "Starts a counted block of glyph upper-left coordinate records.", "The count is followed by that many rows containing X, Y, and character index.", TxiDirectiveValueKind::CoordinateBlockCount, {}},
        {"lowerrightcoords", "font key", "CAurFontInfo::ParseField", "active for font textures", "0/empty", "font atlas TXI", "Starts a counted block of glyph lower-right coordinate records.", "The count is followed by that many rows containing X, Y, and character index.", TxiDirectiveValueKind::CoordinateBlockCount, {}},
        {"unique", "texture key", "legacy engine/tool parser", "active", "0", "texture itself", "Requests a unique texture instance instead of shared caching.", "Resource-management hint; use only when an existing asset requires it.", TxiDirectiveValueKind::Boolean, {}},
        {"xboxdownsample", "platform texture key", "Xbox conversion parser", "active", "1", "texture itself", "Allows Xbox-specific downsampling.", "Alias xbox_downsample is also accepted.", TxiDirectiveValueKind::Boolean, {}},
        {"xbox_downsample", "platform texture key", "Xbox conversion parser", "active", "1", "texture itself", "Allows Xbox-specific downsampling.", "Alias of xboxdownsample; preserved for tool compatibility.", TxiDirectiveValueKind::Boolean, {}},
        {"compresstexture", "tga2tpc compatibility key", "tga2tpc/Kotor Unified Toolset", "tool option", "1", "TXI sidecar consumed by conversion tools", "Controls whether converter should prefer DXT compression when writing TPC. The Odyssey runtime parser does not need this key.", "Converter compatibility directive; exported/preserved but not an engine render directive.", TxiDirectiveValueKind::Boolean, {}}
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
    std::size_t coordinateRowsRemaining = 0;
    std::string coordinateKey;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        TxiEntry entry;
        entry.lineNumber = lineNumber;
        const std::string trimmed = trimTxi(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            entry.blankOrComment = true;
            entries.push_back(std::move(entry));
            continue;
        }
        if (coordinateRowsRemaining != 0) {
            entry.key = coordinateKey;
            entry.value = trimmed;
            entry.coordinateData = true;
            --coordinateRowsRemaining;
            entries.push_back(std::move(entry));
            continue;
        }
        std::istringstream parts(trimmed);
        parts >> entry.key;
        entry.key = asciiLower(entry.key);
        std::string rest;
        std::getline(parts, rest);
        entry.value = trimTxi(rest);
        if (entry.key == "decal1") {
            entry.key = "decal";
            if (entry.value.empty()) entry.value = "1";
        }
        if (const auto directive = findTxiDirective(entry.key);
            directive && directive->valueKind == TxiDirectiveValueKind::CoordinateBlockCount) {
            if (const auto count = parseCoordinateCount(entry.value)) {
                coordinateRowsRemaining = *count;
                coordinateKey = entry.key;
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<TxiValidationIssue> validateTxiText(const std::string& txi) {
    std::vector<TxiValidationIssue> issues;
    std::map<std::string, std::size_t> firstSeen;
    std::string currentProcedure;
    std::size_t coordinateRowsRemaining = 0;
    std::size_t coordinateStartLine = 0;
    std::string coordinateKey;
    for (const auto& entry : parseTxiEntries(txi)) {
        if (entry.blankOrComment) continue;
        if (entry.coordinateData) {
            if (!parseCoordinateRow(entry.value)) {
                addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key,
                         "Expected a coordinate row containing X, Y, and an integer character index.");
            }
            if (coordinateRowsRemaining != 0) --coordinateRowsRemaining;
            continue;
        }
        auto directive = findTxiDirective(entry.key);
        if (!directive) {
            addIssue(issues, TxiIssueSeverity::Warning, entry.lineNumber, entry.key,
                     "Unknown TXI directive. It will be preserved, but it was not found in the uploaded parser trace catalog.");
            continue;
        }
        if (directive->valueKind == TxiDirectiveValueKind::ValueToken) {
            const auto parent = valueTokenParents().find(entry.key);
            const std::string expected = parent == valueTokenParents().end() ? "another directive" : parent->second;
            addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key,
                     "This token is a value for another directive (" + expected + "), not a standalone TXI key.");
            continue;
        }
        if (entry.value.empty()) {
            addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key, "Directive is missing a value.");
        }
        if (firstSeen.count(entry.key) != 0) {
            addIssue(issues, TxiIssueSeverity::Warning, entry.lineNumber, entry.key,
                     "Duplicate TXI directive; simple parser fields are usually overwritten by the last parsed value.");
        } else {
            firstSeen[entry.key] = entry.lineNumber;
        }
        switch (directive->valueKind) {
        case TxiDirectiveValueKind::Boolean:
            if (!parseBoolLike(entry.value)) addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key, "Expected a boolean value such as 0/1, true/false, yes/no, or on/off.");
            break;
        case TxiDirectiveValueKind::BooleanOrInteger:
            if (!parseBoolOrSmallInteger(entry.value)) addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key, "Expected 0/1 or a small integer. isbumpmap 2 is accepted for authored normal maps.");
            break;
        case TxiDirectiveValueKind::Integer:
            if (!parseIntStrict(entry.value)) addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key, "Expected an integer value.");
            break;
        case TxiDirectiveValueKind::Float:
            if (!parseFloatStrict(entry.value)) addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key, "Expected a numeric/float value.");
            break;
        case TxiDirectiveValueKind::ResRef:
            if (!validResRef(entry.value)) addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key, "Expected a non-empty Odyssey ResRef-style texture name of at most 16 ASCII characters.");
            break;
        case TxiDirectiveValueKind::Enum:
            if (!containsAllowed(directive->allowedValues, entry.value)) {
                std::ostringstream msg;
                msg << "Expected one of:";
                for (const auto& value : directive->allowedValues) msg << ' ' << value;
                addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key, msg.str());
            }
            break;
        case TxiDirectiveValueKind::NumericList:
            if (!parseNumericList(entry.value)) addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key, "Expected one or more numeric values separated by spaces, commas, or coordinate punctuation.");
            break;
        case TxiDirectiveValueKind::CoordinateBlockCount: {
            const auto count = parseCoordinateCount(entry.value);
            if (!count) {
                addIssue(issues, TxiIssueSeverity::Error, entry.lineNumber, entry.key,
                         "Expected a non-negative coordinate-record count.");
            } else {
                coordinateRowsRemaining = *count;
                coordinateStartLine = entry.lineNumber;
                coordinateKey = entry.key;
            }
            break;
        }
        case TxiDirectiveValueKind::FreeText:
        case TxiDirectiveValueKind::ValueToken:
            break;
        }

        if (entry.key == "proceduretype") {
            currentProcedure = asciiLower(trimTxi(entry.value));
        } else if ((isControllerKey(entry.key) || isWaterControllerKey(entry.key) || isArturoControllerKey(entry.key)) && currentProcedure.empty()) {
            addIssue(issues, TxiIssueSeverity::Warning, entry.lineNumber, entry.key,
                     "Controller key appears before proceduretype; the traced parser only forwards these after a controller exists.");
        } else if (isWaterControllerKey(entry.key) && currentProcedure != "water") {
            addIssue(issues, TxiIssueSeverity::Warning, entry.lineNumber, entry.key,
                     "Water controller key is only active after proceduretype water.");
        } else if (isArturoControllerKey(entry.key) && currentProcedure != "arturo") {
            addIssue(issues, TxiIssueSeverity::Warning, entry.lineNumber, entry.key,
                     "Arturo controller key is only active after proceduretype arturo.");
        } else if (entry.key == "fps" && currentProcedure != "cycle") {
            addIssue(issues, TxiIssueSeverity::Warning, entry.lineNumber, entry.key,
                     "fps only controls animated TXI cycles after proceduretype cycle.");
        }

        if (entry.key == "bumpmaptexture" || entry.key == "envmaptexture") {
            addIssue(issues, TxiIssueSeverity::Info, entry.lineNumber, entry.key,
                     "Remember to provide the referenced texture and its own TXI where required.");
        }
    }
    if (coordinateRowsRemaining != 0) {
        addIssue(issues, TxiIssueSeverity::Error, coordinateStartLine, coordinateKey,
                 "Coordinate block ended early; " + std::to_string(coordinateRowsRemaining) + " row(s) are missing.");
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
        if (!entry.blankOrComment && !entry.coordinateData) ++directives;
    }
    std::ostringstream out;
    out << "TXI validation report\n";
    out << "status: " << errors << " error(s), " << warnings << " warning(s), " << infos
        << " info note(s) across " << directives << " directive(s).\n";
    if (issues.empty()) {
        out << "OK: all directives matched the typed TXI catalog.\n";
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
        if (!directive) return "Unknown TXI key/value: " + *key + "\n";
        out << directive->name << '\n'
            << "kind: " << directive->kind << '\n'
            << "parser: " << directive->parser << '\n'
            << "state: " << directive->state << '\n'
            << "value type: " << txiDirectiveValueKindToString(directive->valueKind) << '\n'
            << "default: " << directive->defaultValue << '\n'
            << "put it on: " << directive->where << '\n'
            << "meaning: " << directive->layman << '\n';
        if (!directive->allowedValues.empty()) {
            out << "allowed values:";
            for (const auto& value : directive->allowedValues) out << ' ' << value;
            out << '\n';
        }
        if (!directive->interactions.empty()) out << "interactions: " << directive->interactions << '\n';
        return out.str();
    }
    out << "name\tkind\tvalue-type\tstate\twhere\tmeaning\n";
    for (const auto& directive : txiDirectiveCatalog()) {
        if (directive.valueKind == TxiDirectiveValueKind::ValueToken) continue;
        out << directive.name << '\t' << directive.kind << '\t' << txiDirectiveValueKindToString(directive.valueKind)
            << '\t' << directive.state << '\t' << directive.where << '\t' << directive.layman << '\n';
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
        const std::string hay = asciiLower(directive.name + " " + directive.kind + " " + directive.where + " " + directive.layman + " " + directive.interactions);
        if (hay.find(needle) != std::string::npos) {
            out << directive.name << '\t' << directive.kind << '\t' << txiDirectiveValueKindToString(directive.valueKind)
                << '\t' << directive.where << '\t' << directive.layman << '\n';
        }
    }
    const std::string result = out.str();
    return result.empty() ? ("No TXI catalog matches for: " + filter + "\n") : result;
}

} // namespace neotpc::texture
