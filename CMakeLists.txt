cmake_minimum_required(VERSION 3.24)
project(Rayman2MacSilicon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64")

if(NOT DEFINED RECOMP_LLVM_ROOT)
  set(RECOMP_LLVM_ROOT "/opt/homebrew/opt/llvm" CACHE PATH "Homebrew LLVM root")
endif()
set(RECOMP_ALLOW_MISSING_TEST_TOOLS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/SwARc EXCLUDE_FROM_ALL)

# Lifted static-recompiled modules (Rayman2.exe + GliVd1vf + WAVPWBVR + APMP5BVR)
recomp_add_lifted(rayman2_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated")

add_executable(Rayman2
  src/main.cpp
  src/hle/msvcrt32.cpp
  src/hle/win32_extra.cpp
  src/hle/winmm_waveout.cpp
  src/hle/glide2x_metal.cpp
  src/hooks/macos_input.cpp
  src/hooks/widescreen_and_focus.cpp
)

target_include_directories(Rayman2 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SwARc/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SwARc/platforms/win32/src"
)

target_link_libraries(Rayman2 PRIVATE
  rayman2_lifted
  recomp_win32
  recomp_gfx
)

target_compile_options(Rayman2 PRIVATE
  -O2
  -fno-strict-aliasing
  -fnon-call-exceptions
  -fno-omit-frame-pointer
  -mno-omit-leaf-frame-pointer
)
