// Serializer.cpp
#include "Serializer.h"
#include "StateStore.h"
#include "Logger.h"

#include <string>

namespace {
    // Escapes a UTF-8 string for embedding in a JSON string literal.
    void AppendEscaped(std::string& out, const std::string& s) {
        out += '"';
        for (char c : s) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    wsprintfA(buf, "\\u%04x", c & 0xff);
                    out += buf;
                } else {
                    out += c;
                }
            }
        }
        out += '"';
    }
}

std::string SerializeState(const AppState& s) {
    std::string out = "{\"vol\":";
    out += std::to_string(s.volume);
    out += ",\"mute\":";
    out += s.muted ? "true" : "false";
    out += ",\"dev\":";  AppendEscaped(out, s.deviceName);
    out += ",\"title\":"; AppendEscaped(out, s.title);
    out += ",\"artist\":"; AppendEscaped(out, s.artist);
    out += ",\"play\":";  AppendEscaped(out, s.playback);
    out += '}';
    return out;
}

bool WriteToDevice(HANDLE h, const std::string& payload) {
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string line = payload + "\n";
    DWORD written = 0;
    if (!WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)
        || written != line.size()) {
        Log("WARN", "WriteToDevice failed");
        return false;
    }
    return true;
}
