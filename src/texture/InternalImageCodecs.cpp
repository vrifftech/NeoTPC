#include "texture/InternalImageCodecs.hpp"
#include "texture/CodecLimits.hpp"
#include "texture/Operation.hpp"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <spng.h>
namespace neotpc::texture::internal_image {
namespace {
std::string spngError(const char* action, int err) {
    std::ostringstream out;
    out << action << ": " << spng_strerror(err);
    return out.str();
}

struct SpngContext {
    spng_ctx* ctx = nullptr;
    explicit SpngContext(int flags) : ctx(spng_ctx_new(flags)) {
        if (!ctx) throw TextureError("Unable to create PNG codec context");
    }
    ~SpngContext() { spng_ctx_free(ctx); }
    SpngContext(const SpngContext&) = delete;
    SpngContext& operator=(const SpngContext&) = delete;
};

} // namespace

RgbaImage decodePng(const std::vector<std::uint8_t>& bytes) {
    SpngContext png(0);
    int err = spng_set_png_buffer(png.ctx, bytes.data(), bytes.size());
    if (err) throw TextureError(spngError("Unable to attach PNG input buffer", err));

    spng_ihdr ihdr{};
    err = spng_get_ihdr(png.ctx, &ihdr);
    if (err) throw TextureError(spngError("Unable to read PNG header", err));
    const auto expectedSize = ensureDimensions(ihdr.width, ihdr.height, "PNG");

    size_t outSize = 0;
    err = spng_decoded_image_size(png.ctx, SPNG_FMT_RGBA8, &outSize);
    if (err) throw TextureError(spngError("Unable to size decoded PNG image", err));
    if (outSize != expectedSize) throw TextureError("PNG decoded size is not RGBA8888");

    RgbaImage image;
    image.width = ihdr.width;
    image.height = ihdr.height;
    image.rgba.resize(outSize);
    checkOperation();
    err = spng_decode_image(png.ctx, image.rgba.data(), image.rgba.size(), SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (err) throw TextureError(spngError("Unable to decode PNG image", err));
    checkOperation();
    image.encoding = "PNG via libspng";
    return image;
}

std::vector<std::uint8_t> encodePngRgba(std::uint32_t width,
                                        std::uint32_t height,
                                        const std::vector<std::uint8_t>& rgba) {
    const auto expected = ensureDimensions(width, height, "PNG");
    if (rgba.size() != expected) throw TextureError("PNG encoder expected RGBA8888 pixels");

    SpngContext png(SPNG_CTX_ENCODER);
    int err = spng_set_option(png.ctx, SPNG_ENCODE_TO_BUFFER, 1);
    if (err) throw TextureError(spngError("Unable to enable PNG memory output", err));

    spng_ihdr ihdr{};
    ihdr.width = width;
    ihdr.height = height;
    ihdr.bit_depth = 8;
    ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
    ihdr.compression_method = 0;
    ihdr.filter_method = 0;
    ihdr.interlace_method = SPNG_INTERLACE_NONE;
    err = spng_set_ihdr(png.ctx, &ihdr);
    if (err) throw TextureError(spngError("Unable to set PNG header", err));

    checkOperation();
    err = spng_encode_image(png.ctx, rgba.data(), rgba.size(), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
    if (err) throw TextureError(spngError("Unable to encode PNG image", err));

    checkOperation();
    size_t encodedSize = 0;
    int bufferError = 0;
    void* raw = spng_get_png_buffer(png.ctx, &encodedSize, &bufferError);
    if (!raw || bufferError) {
        if (raw) std::free(raw);
        throw TextureError(spngError("Unable to retrieve encoded PNG buffer", bufferError ? bufferError : SPNG_EINVAL));
    }
    const auto release=[](void* p){std::free(p);};
    std::unique_ptr<void,decltype(release)> owned(raw,release);
    std::vector<std::uint8_t> out(encodedSize);
    std::memcpy(out.data(), raw, encodedSize);
    return out;
}

} // namespace neotpc::texture::internal_image
