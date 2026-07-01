@echo off
REM Proxy build script: loads MSVC dev env, then configures + builds.
REM Callable from WSL via cmd.exe. PATH is reset first so WSL-interop
REM leakage (e.g. msys64 ld.exe) does not hijack the MSVC toolchain.
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Windows\System32\WindowsPowerShell\v1.0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d D:\Repos\MediaDockCompanion
cmake --preset x64-debug || exit /b 1
cmake --build out\build\x64-debug --target MediaDockCompanion
