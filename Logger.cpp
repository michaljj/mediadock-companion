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

    std::string Timestamp() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[16];
        wsprintfA(buf, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        return buf;
    }
}

void Log(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    const std::wstring& path = ResolvePath();
    if (path.empty()) return;

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    std::string line = "[" + Timestamp() + "][" + level + "] " + msg + "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
}
