// Util.h : Small shared helpers.
#pragma once

#include <string>
#include <string_view>

// Converts a UTF-16 Windows string to UTF-8 for serialization/logging.
std::string WideToUtf8(std::wstring_view w);
