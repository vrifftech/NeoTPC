#pragma once

#include <wx/string.h>

#include <filesystem>
#include <string>

namespace neotpc::wxpath {

inline std::filesystem::path fromWx(const wxString& value) {
#if defined(_WIN32)
    return std::filesystem::path(value.ToStdWstring());
#else
    const auto utf8 = value.ToUTF8();
    // POSIX filesystem paths are byte strings. NeoTPC treats those bytes as
    // UTF-8 at the wxWidgets boundary; constructing from std::string avoids
    // std::filesystem::u8path(), which is deprecated by newer libstdc++.
    return utf8 ? std::filesystem::path(std::string(utf8.data())) : std::filesystem::path{};
#endif
}

inline wxString toWx(const std::filesystem::path& value) {
#if defined(_WIN32)
    return wxString(value.wstring());
#else
    // path::u8string() may return std::u8string (char8_t) with newer
    // standard libraries even when this project is compiled as C++17.
    // path::native() is std::string on POSIX, which is exactly the byte
    // representation wxString::FromUTF8() expects here.
    const auto& utf8 = value.native();
    return wxString::FromUTF8(utf8.data(), utf8.size());
#endif
}

} // namespace neotpc::wxpath
