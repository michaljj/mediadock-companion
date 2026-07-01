// Logger.h : Thread-safe append-only file logger.
#pragma once

#include <string>

// Appends "[HH:MM:SS][level] msg\n" to %LOCALAPPDATA%\MediaDockCompanion\log.txt.
// Creates the directory on first use. Safe to call from any thread.
void Log(const char* level, const std::string& msg);
