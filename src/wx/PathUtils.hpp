#pragma once

#include <wx/string.h>

#include <filesystem>

namespace neotpc::wxpath {

inline std::filesystem::path fromWx(const wxString& value) {
#if defined(_WIN32)
    return std::filesystem::path(value.ToStdWstring());
#else
    const auto utf8 = value.ToUTF8();
    return utf8 ? std::filesystem::u8path(utf8.data()) : std::filesystem::path{};
#endif
}

inline wxString toWx(const std::filesystem::path& value) {
#if defined(_WIN32)
    return wxString(value.wstring());
#else
    const auto utf8 = value.u8string();
    return wxString::FromUTF8(utf8.data(), utf8.size());
#endif
}

} // namespace neotpc::wxpath
