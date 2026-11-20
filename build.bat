@echo off
REM Builds HotkeyCloser.exe with CMake + Visual Studio Build Tools (MSVC).
where cmake >nul 2>nul || (echo CMake not found. Install it from https://cmake.org/download/ & pause & exit /b 1)
cmake -S . -B build -A x64 || (pause & exit /b 1)
cmake --build build --config Release || (pause & exit /b 1)
echo.
echo Done:  build\Release\HotkeyCloser.exe
pause