# Cross-compile ReXGlue for Windows with clang-cl and an xwin sysroot.
#
# Generate the sysroot with:
#   xwin --accept-license --arch x86_64 splat \
#     --use-winsysroot-style --preserve-ms-arch-notation --output <path>
#
# Then configure CMake with XWIN_ROOT=<path> and this toolchain file.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED XWIN_ROOT AND DEFINED ENV{XWIN_ROOT})
    set(XWIN_ROOT "$ENV{XWIN_ROOT}")
endif()
if(NOT XWIN_ROOT)
    message(FATAL_ERROR
        "XWIN_ROOT must point to an xwin sysroot generated with "
        "--use-winsysroot-style --preserve-ms-arch-notation")
endif()
file(TO_CMAKE_PATH "${XWIN_ROOT}" XWIN_ROOT)
set(XWIN_ROOT "${XWIN_ROOT}" CACHE PATH "xwin Windows SDK/CRT sysroot" FORCE)

if(NOT EXISTS "${XWIN_ROOT}/VC/Tools/MSVC" OR
   NOT EXISTS "${XWIN_ROOT}/Windows Kits/10")
    message(FATAL_ERROR "XWIN_ROOT is not a complete xwin winsysroot: ${XWIN_ROOT}")
endif()

file(GLOB XWIN_MSVC_VERSIONS LIST_DIRECTORIES TRUE
    "${XWIN_ROOT}/VC/Tools/MSVC/*")
file(GLOB XWIN_SDK_VERSIONS LIST_DIRECTORIES TRUE
    "${XWIN_ROOT}/Windows Kits/10/Lib/*")
list(SORT XWIN_MSVC_VERSIONS COMPARE NATURAL ORDER DESCENDING)
list(SORT XWIN_SDK_VERSIONS COMPARE NATURAL ORDER DESCENDING)
list(GET XWIN_MSVC_VERSIONS 0 XWIN_MSVC_DIR)
list(GET XWIN_SDK_VERSIONS 0 XWIN_SDK_LIB_DIR)

if(NOT EXISTS "${XWIN_MSVC_DIR}/lib/x64/vcruntime.lib" OR
   NOT EXISTS "${XWIN_SDK_LIB_DIR}/ucrt/x64/ucrt.lib" OR
   NOT EXISTS "${XWIN_SDK_LIB_DIR}/um/x64/kernel32.Lib")
    message(FATAL_ERROR "XWIN_ROOT does not contain x86-64 CRT and SDK libraries")
endif()

find_program(XWIN_CLANG_CL NAMES clang-cl REQUIRED)
find_program(XWIN_LLD_LINK NAMES lld-link lld-link-20 lld-link-19 lld-link-18 REQUIRED)
find_program(XWIN_LLVM_LIB NAMES llvm-lib llvm-lib-20 llvm-lib-19 llvm-lib-18 REQUIRED)
find_program(XWIN_LLVM_RC NAMES llvm-rc llvm-rc-20 llvm-rc-19 llvm-rc-18 REQUIRED)
find_program(XWIN_LLVM_MT NAMES llvm-mt llvm-mt-20 llvm-mt-19 llvm-mt-18 REQUIRED)

set(CMAKE_C_COMPILER "${XWIN_CLANG_CL}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${XWIN_CLANG_CL}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${XWIN_LLD_LINK}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${XWIN_LLVM_LIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "${XWIN_LLVM_RC}" CACHE FILEPATH "" FORCE)
set(CMAKE_MT "${XWIN_LLVM_MT}" CACHE FILEPATH "" FORCE)

set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# llvm-rc does not take /winsysroot; give it the SDK headers resource
# scripts include (winresrc.h and friends).
get_filename_component(XWIN_SDK_VERSION "${XWIN_SDK_LIB_DIR}" NAME)
set(XWIN_SDK_INCLUDE_DIR "${XWIN_ROOT}/Windows Kits/10/Include/${XWIN_SDK_VERSION}")
set(CMAKE_RC_FLAGS_INIT
    "/I \"${XWIN_SDK_INCLUDE_DIR}/um\" /I \"${XWIN_SDK_INCLUDE_DIR}/shared\" /I \"${XWIN_SDK_INCLUDE_DIR}/ucrt\"")

set(CMAKE_C_FLAGS_INIT "/winsysroot\"${XWIN_ROOT}\"")
set(CMAKE_CXX_FLAGS_INIT "/winsysroot\"${XWIN_ROOT}\"")

set(_XWIN_LINK_FLAGS
    "/libpath:\"${XWIN_MSVC_DIR}/lib/x64\" /libpath:\"${XWIN_SDK_LIB_DIR}/ucrt/x64\" /libpath:\"${XWIN_SDK_LIB_DIR}/um/x64\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_XWIN_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_XWIN_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_XWIN_LINK_FLAGS}")

list(PREPEND CMAKE_FIND_ROOT_PATH "${XWIN_ROOT}")
list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
