#include "texture/InternalImageCodecs.hpp"
#include "texture/CodecLimits.hpp"
#include "texture/Operation.hpp"
#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
extern "C" {
#include <jpeglib.h>
}
namespace neotpc::texture::internal_image {
namespace {
// The libjpeg helpers own C state across setjmp. Never throw through a C
// callback/longjmp frame. Public C++ wrappers rethrow cancellation afterwards.
bool continueJpegOperation() noexcept {
    try { checkOperation(); return true; } catch(...) { return false; }
}
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
        if(!continueJpegOperation()){copyJpegMessage(error,errorCapacity,"JPEG operation interrupted");destroyJpegDecoderState(state);return false;}
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
        if(!continueJpegOperation()){copyJpegMessage(error,errorCapacity,"JPEG operation interrupted");destroyJpegEncoderState(state);return false;}
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

RgbaImage decodeJpeg(const std::vector<std::uint8_t>& bytes) {
    checkOperation();
    if (bytes.empty()) throw TextureError("JPEG input is empty");
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    char error[JMSG_LENGTH_MAX]{};
    if (!inspectJpegDimensions(reinterpret_cast<const unsigned char*>(bytes.data()),
                               bytes.size(), &width, &height, error, sizeof(error))) {
        checkOperation();
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
        checkOperation();
        throw TextureError(error[0] ? error : "Unable to decode JPEG image");
    }
    checkOperation();
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
        checkOperation();
        throw TextureError(error[0] ? error : "Unable to encode JPEG image");
    }
    std::unique_ptr<unsigned char, FreeBuffer> ownedOutput(rawOut);
    checkOperation();
    return std::vector<std::uint8_t>(ownedOutput.get(), ownedOutput.get() + rawSize);
}

} // namespace neotpc::texture::internal_image
