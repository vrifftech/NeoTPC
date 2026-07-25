#include "texture/InternalImageCodecs.hpp"

#include "texture/Error.hpp"
#include "texture/ParserLimits.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <sstream>

#include <spng.h>

extern "C" {
#include <jpeglib.h>
}

namespace neotpc::texture::internal_image {
namespace {

constexpr std::uint32_t kMaxImageDimension = 0x8000u;

std::size_t ensureDimensions(std::uint32_t width, std::uint32_t height, const char* codec) {
    if (width == 0 || height == 0 || width > kMaxImageDimension || height > kMaxImageDimension) {
        throw TextureError(std::string(codec) + " dimensions are invalid or too large");
    }
    std::uint64_t pixels = 0;
    std::uint64_t bytes = 0;
    if (!parser::checkedMultiply(width, height, pixels) ||
        !parser::checkedMultiply(pixels, UINT64_C(4), bytes) ||
        bytes > parser::maxDecodedBytes() ||
        bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw TextureError(std::string(codec) + " decoded image exceeds the parser memory limit");
    }
    return static_cast<std::size_t>(bytes);
}

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

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // jmp_buf carries platform alignment; padding is intentional for libjpeg error state.
#endif
struct JpegErrorManager {
    jpeg_error_mgr pub;
    jmp_buf setjmpBuffer;
    char message[JMSG_LENGTH_MAX];
};

struct JpegDecoderState {
    jpeg_decompress_struct cinfo;
    JpegErrorManager error;
};

struct JpegEncoderState {
    jpeg_compress_struct cinfo;
    JpegErrorManager error;
    jpeg_destination_mgr destination;
    unsigned char* output;
    std::size_t outputSize;
    std::size_t outputCapacity;
    std::size_t maxOutputSize;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

void copyJpegMessage(char* destination, std::size_t capacity, const char* message) noexcept {
    if (!destination || capacity == 0) return;
    const char* source = (message && message[0]) ? message : "JPEG codec operation failed";
    std::strncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

[[noreturn]] void jpegFailure(j_common_ptr cinfo, const char* message) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    copyJpegMessage(manager->message, sizeof(manager->message), message);
    longjmp(manager->setjmpBuffer, 1);
}

void jpegErrorExit(j_common_ptr cinfo) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, manager->message);
    longjmp(manager->setjmpBuffer, 1);
}

void destroyJpegDecoderState(JpegDecoderState* state) noexcept {
    if (!state) return;
    jpeg_destroy_decompress(&state->cinfo);
    delete state;
}

void destroyJpegEncoderState(JpegEncoderState* state) noexcept {
    if (!state) return;
    jpeg_destroy_compress(&state->cinfo);
    std::free(state->output);
    delete state;
}

bool inspectJpegDimensions(const unsigned char* bytes,
                           std::size_t byteCount,
                           std::uint32_t* width,
                           std::uint32_t* height,
                           char* error,
                           std::size_t errorCapacity) noexcept {
    if (byteCount > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
        copyJpegMessage(error, errorCapacity, "JPEG input is too large for the linked codec");
        return false;
    }
    auto* state = new (std::nothrow) JpegDecoderState{};
    if (!state) {
        copyJpegMessage(error, errorCapacity, "Unable to allocate JPEG decoder state");
        return false;
    }
    state->cinfo.err = jpeg_std_error(&state->error.pub);
    state->error.pub.error_exit = jpegErrorExit;
#if defined(_MSC_VER)
#pragma warning(suppress: 4611) // libjpeg's C error API requires setjmp/longjmp inside this POD-only helper.
#endif
    if (setjmp(state->error.setjmpBuffer)) {
        copyJpegMessage(error, errorCapacity, state->error.message);
        destroyJpegDecoderState(state);
        return false;
    }

    jpeg_create_decompress(&state->cinfo);
    jpeg_mem_src(&state->cinfo, const_cast<unsigned char*>(bytes), static_cast<unsigned long>(byteCount));
    if (jpeg_read_header(&state->cinfo, TRUE) != JPEG_HEADER_OK) {
        jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "JPEG input does not contain an image header");
    }
    *width = static_cast<std::uint32_t>(state->cinfo.image_width);
    *height = static_cast<std::uint32_t>(state->cinfo.image_height);
    destroyJpegDecoderState(state);
    return true;
}

bool decodeJpegIntoRgba(const unsigned char* bytes,
                        std::size_t byteCount,
                        std::uint32_t expectedWidth,
                        std::uint32_t expectedHeight,
                        unsigned char* rgba,
                        JSAMPLE* row,
                        char* error,
                        std::size_t errorCapacity) noexcept {
    auto* state = new (std::nothrow) JpegDecoderState{};
    if (!state) {
        copyJpegMessage(error, errorCapacity, "Unable to allocate JPEG decoder state");
        return false;
    }
    state->cinfo.err = jpeg_std_error(&state->error.pub);
    state->error.pub.error_exit = jpegErrorExit;
#if defined(_MSC_VER)
#pragma warning(suppress: 4611) // libjpeg's C error API requires setjmp/longjmp inside this POD-only helper.
#endif
    if (setjmp(state->error.setjmpBuffer)) {
        copyJpegMessage(error, errorCapacity, state->error.message);
        destroyJpegDecoderState(state);
        return false;
    }

    jpeg_create_decompress(&state->cinfo);
    jpeg_mem_src(&state->cinfo, const_cast<unsigned char*>(bytes), static_cast<unsigned long>(byteCount));
    if (jpeg_read_header(&state->cinfo, TRUE) != JPEG_HEADER_OK) {
        jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "JPEG input does not contain an image header");
    }
    state->cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&state->cinfo);
    if (state->cinfo.output_width != expectedWidth || state->cinfo.output_height != expectedHeight) {
        jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "JPEG dimensions changed while decoding");
    }
    if (state->cinfo.output_components != 3) {
        jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "JPEG decoder did not produce RGB pixels");
    }

    while (state->cinfo.output_scanline < state->cinfo.output_height) {
        JSAMPROW rowPointer = row;
        const std::uint32_t y = state->cinfo.output_scanline;
        if (jpeg_read_scanlines(&state->cinfo, &rowPointer, 1) != 1) {
            jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "JPEG decoder stopped before the final scanline");
        }
        for (std::uint32_t x = 0; x < expectedWidth; ++x) {
            const std::size_t source = static_cast<std::size_t>(x) * 3u;
            const std::size_t destination =
                (static_cast<std::size_t>(y) * expectedWidth + x) * 4u;
            rgba[destination + 0] = row[source + 0];
            rgba[destination + 1] = row[source + 1];
            rgba[destination + 2] = row[source + 2];
            rgba[destination + 3] = 255;
        }
    }

    jpeg_finish_decompress(&state->cinfo);
    destroyJpegDecoderState(state);
    return true;
}

void jpegDestinationInit(j_compress_ptr cinfo) {
    auto* state = static_cast<JpegEncoderState*>(cinfo->client_data);
    state->outputSize = 0;
    state->destination.next_output_byte = state->output;
    state->destination.free_in_buffer = state->outputCapacity;
}

boolean jpegDestinationGrow(j_compress_ptr cinfo) {
    auto* state = static_cast<JpegEncoderState*>(cinfo->client_data);
    const std::size_t used = state->outputCapacity;
    if (used >= state->maxOutputSize) {
        jpegFailure(reinterpret_cast<j_common_ptr>(cinfo), "Encoded JPEG exceeds the in-memory file limit");
    }
    std::size_t nextCapacity = used <= state->maxOutputSize / 2 ? used * 2 : state->maxOutputSize;
    if (nextCapacity <= used) {
        jpegFailure(reinterpret_cast<j_common_ptr>(cinfo), "Encoded JPEG output size overflows");
    }
    void* resized = std::realloc(state->output, nextCapacity);
    if (!resized) {
        jpegFailure(reinterpret_cast<j_common_ptr>(cinfo), "Unable to grow the JPEG output buffer");
    }
    state->output = static_cast<unsigned char*>(resized);
    state->outputCapacity = nextCapacity;
    state->destination.next_output_byte = state->output + used;
    state->destination.free_in_buffer = nextCapacity - used;
    return TRUE;
}

void jpegDestinationFinish(j_compress_ptr cinfo) {
    auto* state = static_cast<JpegEncoderState*>(cinfo->client_data);
    state->outputSize = state->outputCapacity - state->destination.free_in_buffer;
}

bool encodeJpegToBuffer(std::uint32_t width,
                        std::uint32_t height,
                        const unsigned char* rgba,
                        JSAMPLE* row,
                        int quality,
                        unsigned char** output,
                        std::size_t* outputSize,
                        char* error,
                        std::size_t errorCapacity) noexcept {
    *output = nullptr;
    *outputSize = 0;
    auto* state = new (std::nothrow) JpegEncoderState{};
    if (!state) {
        copyJpegMessage(error, errorCapacity, "Unable to allocate JPEG encoder state");
        return false;
    }
    state->cinfo.err = jpeg_std_error(&state->error.pub);
    state->error.pub.error_exit = jpegErrorExit;
    state->cinfo.client_data = state;
#if defined(_MSC_VER)
#pragma warning(suppress: 4611) // libjpeg's C error API requires setjmp/longjmp inside this POD-only helper.
#endif
    if (setjmp(state->error.setjmpBuffer)) {
        copyJpegMessage(error, errorCapacity, state->error.message);
        destroyJpegEncoderState(state);
        return false;
    }

    jpeg_create_compress(&state->cinfo);
    const auto fileLimit = parser::maxInMemoryFileBytes();
    state->maxOutputSize = fileLimit > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
                               ? std::numeric_limits<std::size_t>::max()
                               : static_cast<std::size_t>(fileLimit);
    state->outputCapacity = std::min<std::size_t>(4096u, state->maxOutputSize);
    if (state->outputCapacity == 0) {
        jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "JPEG output limit is zero");
    }
    state->output = static_cast<unsigned char*>(std::malloc(state->outputCapacity));
    if (!state->output) {
        jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "Unable to allocate the JPEG output buffer");
    }

    state->destination.init_destination = jpegDestinationInit;
    state->destination.empty_output_buffer = jpegDestinationGrow;
    state->destination.term_destination = jpegDestinationFinish;
    state->cinfo.dest = &state->destination;
    state->cinfo.image_width = width;
    state->cinfo.image_height = height;
    state->cinfo.input_components = 3;
    state->cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&state->cinfo);
    jpeg_set_quality(&state->cinfo, quality, TRUE);
    jpeg_start_compress(&state->cinfo, TRUE);

    while (state->cinfo.next_scanline < state->cinfo.image_height) {
        const std::uint32_t y = state->cinfo.next_scanline;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t source = (static_cast<std::size_t>(y) * width + x) * 4u;
            const std::size_t destination = static_cast<std::size_t>(x) * 3u;
            row[destination + 0] = rgba[source + 0];
            row[destination + 1] = rgba[source + 1];
            row[destination + 2] = rgba[source + 2];
        }
        JSAMPROW rowPointer = row;
        if (jpeg_write_scanlines(&state->cinfo, &rowPointer, 1) != 1) {
            jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "JPEG encoder stopped before the final scanline");
        }
    }
    jpeg_finish_compress(&state->cinfo);
    if (state->outputSize == 0) {
        jpegFailure(reinterpret_cast<j_common_ptr>(&state->cinfo), "JPEG encoder produced an empty image");
    }

    jpeg_destroy_compress(&state->cinfo);
    *output = state->output;
    *outputSize = state->outputSize;
    state->output = nullptr;
    delete state;
    return true;
}

struct FreeBuffer {
    void operator()(unsigned char* buffer) const noexcept { std::free(buffer); }
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
    err = spng_decode_image(png.ctx, image.rgba.data(), image.rgba.size(), SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (err) throw TextureError(spngError("Unable to decode PNG image", err));
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

    err = spng_encode_image(png.ctx, rgba.data(), rgba.size(), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
    if (err) throw TextureError(spngError("Unable to encode PNG image", err));

    size_t encodedSize = 0;
    int bufferError = 0;
    void* raw = spng_get_png_buffer(png.ctx, &encodedSize, &bufferError);
    if (!raw || bufferError) {
        if (raw) std::free(raw);
        throw TextureError(spngError("Unable to retrieve encoded PNG buffer", bufferError ? bufferError : SPNG_EINVAL));
    }
    std::vector<std::uint8_t> out(encodedSize);
    std::memcpy(out.data(), raw, encodedSize);
    std::free(raw);
    return out;
}

RgbaImage decodeJpeg(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) throw TextureError("JPEG input is empty");
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    char error[JMSG_LENGTH_MAX]{};
    if (!inspectJpegDimensions(reinterpret_cast<const unsigned char*>(bytes.data()),
                               bytes.size(), &width, &height, error, sizeof(error))) {
        throw TextureError(error[0] ? error : "Unable to read JPEG dimensions");
    }
    const auto decodedBytes = ensureDimensions(width, height, "JPEG");

    RgbaImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(decodedBytes);
    std::vector<JSAMPLE> row(static_cast<std::size_t>(width) * 3u);
    if (!decodeJpegIntoRgba(reinterpret_cast<const unsigned char*>(bytes.data()),
                            bytes.size(), width, height, image.rgba.data(), row.data(),
                            error, sizeof(error))) {
        throw TextureError(error[0] ? error : "Unable to decode JPEG image");
    }
    image.encoding = "JPEG via libjpeg API";
    return image;
}

std::vector<std::uint8_t> encodeJpegRgb(std::uint32_t width,
                                        std::uint32_t height,
                                        const std::vector<std::uint8_t>& rgba,
                                        std::uint8_t quality) {
    const auto expected = ensureDimensions(width, height, "JPEG");
    if (rgba.size() != expected) throw TextureError("JPEG encoder expected RGBA8888 pixels");
    std::vector<JSAMPLE> row(static_cast<std::size_t>(width) * 3u);
    unsigned char* rawOut = nullptr;
    std::size_t rawSize = 0;
    char error[JMSG_LENGTH_MAX]{};
    const int jpegQuality = std::max<int>(1, std::min<int>(100, quality));
    if (!encodeJpegToBuffer(width, height, rgba.data(), row.data(), jpegQuality,
                            &rawOut, &rawSize, error, sizeof(error))) {
        throw TextureError(error[0] ? error : "Unable to encode JPEG image");
    }
    std::unique_ptr<unsigned char, FreeBuffer> ownedOutput(rawOut);
    return std::vector<std::uint8_t>(ownedOutput.get(), ownedOutput.get() + rawSize);
}

} // namespace neotpc::texture::internal_image
