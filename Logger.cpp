// Logger.cpp
#include "Logger.h"
#include "Config.h"

#include <windows.h>
#include <shlobj.h>
#include <mutex>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace {
    std::mutex g_logMutex;
    bool g_pathResolved = false;
    std::wstring g_logPath;

    // Resolves %LOCALAPPDATA%\MediaDockCompanion\log.txt, creating the dir.
    // Caller must hold g_logMutex.
    const std::wstring& ResolvePath() {
        if (g_pathResolved) return g_logPath;
        g_pathResolved = true;

        PWSTR base = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
            std::wstring dir = base;
            CoTaskMemFree(base);
            dir += L'\\';
            dir += Config::LOG_DIR_NAME;
            CreateDirectoryW(dir.c_str(), nullptr); // ok if already exists
            g_logPath = dir + L'\\' + Config::LOG_FILE_NAME;
        }
        return g_logPath;
    }

    // Cap the log at ~1 MB: rename the current file aside so it starts fresh,
    // keeping one previous generation (log.txt.old). Holds g_logMutex already.
    constexpr ULONGLONG kMaxLogBytes = 1ull << 20;
    void RotateIfLarge(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
        ULONGLONG size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size < kMaxLogBytes) return;
        std::wstring old = path + L".old";
        DeleteFileW(old.c_str());
        MoveFileW(path.c_str(), old.c_str());
    }

    std::string Timestamp() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[24];
        wsprintfA(buf, "%04d-%02d-%02d %02d:%02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
    }
}

void Log(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    const std::wstring& path = ResolvePath();
    if (path.empty()) return;
    RotateIfLarge(path);

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    std::string line = "[" + Timestamp() + "][" + level + "] " + msg + "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
}
