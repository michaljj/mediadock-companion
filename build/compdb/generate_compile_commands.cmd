@echo off
call "C:\Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" || exit /b 1
"C:/Program Files/CMake/bin/cmake.exe" -S "D:/Repos/mine/mediadock-firmware/WinApp" -B "D:/Repos/mine/mediadock-firmware/WinApp/build/compdb" -G Ninja -DCMAKE_MAKE_PROGRAM="C:/Users/Jurek/AppData/Local/Microsoft/WinGet/Packages/Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe/ninja.exe" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || exit /b 1
"C:/Program Files/CMake/bin/cmake.exe" -E copy_if_different "D:/Repos/mine/mediadock-firmware/WinApp/build/compdb/compile_commands.json" "D:/Repos/mine/mediadock-firmware/WinApp/compile_commands.json"
