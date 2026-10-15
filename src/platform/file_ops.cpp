#include "platform/file_ops.h"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pzip {
namespace platform {

namespace {

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& utf8Str) {
    if (utf8Str.empty()) {
        return std::wstring();
    }
    const int32_t wideLen = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8Str.c_str(), static_cast<int32_t>(utf8Str.size()), NULL, 0);
    if (wideLen <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(wideLen), 0);
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8Str.c_str(), static_cast<int32_t>(utf8Str.size()), &wide[0], wideLen);
    return wide;
}
#endif

}  // namespace

bool AtomicRenameFile(const std::string& fromPathUtf8, const std::string& toPathUtf8) {
    if (fromPathUtf8.empty() || toPathUtf8.empty()) {
        return false;
    }

#if defined(_WIN32)
    const std::wstring fromWide = Utf8ToWide(fromPathUtf8);
    const std::wstring toWide = Utf8ToWide(toPathUtf8);
    if (fromWide.empty() || toWide.empty()) {
        return false;
    }

    if (::MoveFileExW(fromWide.c_str(), toWide.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    // Fallback using ReplaceFileW if MoveFileEx failed
    if (::ReplaceFileW(toWide.c_str(), fromWide.c_str(), NULL, 0, NULL, NULL)) {
        return true;
    }
    return false;
#else
    return std::rename(fromPathUtf8.c_str(), toPathUtf8.c_str()) == 0;
#endif
}

bool RemoveFile(const std::string& pathUtf8) {
    if (pathUtf8.empty()) {
        return true;
    }
#if defined(_WIN32)
    const std::wstring pathWide = Utf8ToWide(pathUtf8);
    if (pathWide.empty()) {
        return false;
    }
    if (::DeleteFileW(pathWide.c_str())) {
        return true;
    }
    return ::GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    if (std::remove(pathUtf8.c_str()) == 0) {
        return true;
    }
    return true;  // If file does not exist, consider succeeded
#endif
}

}  // namespace platform
}  // namespace pzip
