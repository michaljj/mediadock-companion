// Serializer.h : State -> JSON line, and write to the COM handle.
#pragma once

#include <windows.h>
#include <string>

struct AppState;

// Produces a compact JSON object, e.g.
// {"vol":50,"dev":"Speakers","title":"...","artist":"...","play":"Playing"}
std::string SerializeState(const AppState& s);

// Writes payload + "\n" to the COM handle. Returns false on write failure
// (treat as device disconnect).
bool WriteToDevice(HANDLE h, const std::string& payload);
