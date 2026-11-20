cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)            # lets us pick the static MSVC runtime
project(HotkeyCloser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: link the C++ runtime statically -> one .exe, no VC++ redistributable needed
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(SOURCES main.cpp)
if(MINGW)
  enable_language(RC)                    # MinGW needs the manifest via a resource file
  list(APPEND SOURCES app.rc)
endif()

add_executable(HotkeyCloser WIN32 ${SOURCES})
target_compile_definitions(HotkeyCloser PRIVATE UNICODE _UNICODE)
target_link_libraries(HotkeyCloser PRIVATE
  user32 gdi32 comctl32 shell32 advapi32 dwmapi uxtheme version)

if(MSVC)
  target_compile_options(HotkeyCloser PRIVATE /W3 /utf-8)
else()
  target_link_options(HotkeyCloser PRIVATE -municode -mwindows -static -static-libgcc -static-libstdc++)
endif()
