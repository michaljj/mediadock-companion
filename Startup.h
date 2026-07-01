// Startup.h : HKCU Run-key login autostart.
#pragma once

// Reflects whether the app is registered to start at login.
bool IsAutostartEnabled();

// Adds (enable) or removes (disable) the HKCU Run value pointing at this exe.
void SetAutostart(bool enable);
