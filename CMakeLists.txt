cmake_minimum_required(VERSION 3.24)
project(FSRScale LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# FidelityFX SDK 1.1.4
# Full source tree:  <SDK>/ffx-api/include/ffx_api/...
# Minimal package:   <SDK>/PrebuiltSignedDLL/amd_fidelityfx_dx12.{dll,lib}
# Headers are always required from ffx-api/include (or a mirrored layout).
# ---------------------------------------------------------------------------
set(FFX_SDK "" CACHE PATH "AMD FidelityFX SDK 1.1.4 root (folder that contains ffx-api and/or PrebuiltSignedDLL)")

if(NOT FFX_SDK)
  message(FATAL_ERROR
    "Set -DFFX_SDK= to your FidelityFX SDK 1.1.4 root.\n"
    "Example:\n"
    "  cmake ... -DFFX_SDK=C:/Users/Demuriel/Documents/my-apps/FidelityFX-SDK-v1.1.4")
endif()

# Normalise: strip trailing slash / accidental junk
get_filename_component(FFX_SDK "${FFX_SDK}" ABSOLUTE)

message(STATUS "FFX_SDK = ${FFX_SDK}")

# --- Headers: look for ffx_api/ffx_api.h under several known layouts ---------
set(_FFX_INCLUDE_CANDIDATES
  "${FFX_SDK}/ffx-api/include"
  "${FFX_SDK}/ffx-api/include/ffx_api/.."
  "${FFX_SDK}/include"
  "${FFX_SDK}/Kits/FidelityFX/api/include"
  "${FFX_SDK}/Kits/FidelityFX/api/include/.."
  "${CMAKE_SOURCE_DIR}/third_party"
  "${CMAKE_SOURCE_DIR}/third_party/ffx/.."
)

find_path(FFX_INCLUDE
  NAMES ffx_api/ffx_api.h
  PATHS ${_FFX_INCLUDE_CANDIDATES}
  NO_DEFAULT_PATH
)

# Also accept a tree where headers live directly as ffx_api.h (no ffx_api/ subdir)
if(NOT FFX_INCLUDE)
  find_path(FFX_INCLUDE_FLAT
    NAMES ffx_api.h
    PATHS
      "${FFX_SDK}/ffx-api/include/ffx_api"
      "${FFX_SDK}/PrebuiltSignedDLL"
      "${CMAKE_SOURCE_DIR}/third_party/ffx"
    NO_DEFAULT_PATH
  )
  if(FFX_INCLUDE_FLAT)
    # Parent so that #include <ffx_api/ffx_api.h> still works if we mirror layout
    get_filename_component(FFX_INCLUDE "${FFX_INCLUDE_FLAT}" DIRECTORY)
  endif()
endif()

if(NOT FFX_INCLUDE)
  message(FATAL_ERROR
    "Could not find ffx_api/ffx_api.h under FFX_SDK.\n"
    "  FFX_SDK was: ${FFX_SDK}\n"
    "Expected one of:\n"
    "  ${FFX_SDK}/ffx-api/include/ffx_api/ffx_api.h\n"
    "  ${FFX_SDK}/include/ffx_api/ffx_api.h\n"
    "Make sure you extracted the FULL SDK 1.1.4 (not only the DLL zip),\n"
    "or that the path does not have trailing garbage (e.g. ...1.1.4clear).")
endif()

message(STATUS "FFX_INCLUDE = ${FFX_INCLUDE}")

# Upscale headers are required by fsr.h
if(NOT EXISTS "${FFX_INCLUDE}/ffx_api/ffx_upscale.h" AND
   NOT EXISTS "${FFX_INCLUDE}/ffx_upscale.h")
  # Some trees nest differently; warn but continue if parent has them
  file(GLOB_RECURSE _UP_H "${FFX_SDK}/*/ffx_upscale.h")
  if(NOT _UP_H)
    message(WARNING
      "ffx_upscale.h not found next to ffx_api.h. "
      "Build may fail if the SDK is incomplete.")
  endif()
endif()

# --- Import library --------------------------------------------------------
set(_FFX_LIB_CANDIDATES
  "${FFX_SDK}/ffx-api/bin"
  "${FFX_SDK}/PrebuiltSignedDLL"
  "${FFX_SDK}/bin"
  "${FFX_SDK}/Kits/FidelityFX/bin"
  "${FFX_SDK}/Kits/FidelityFX/lib"
  "${FFX_SDK}/sdk/bin"
  "${FFX_SDK}/sdk/libs"
)

find_library(FFX_LIB
  NAMES amd_fidelityfx_dx12
  PATHS ${_FFX_LIB_CANDIDATES}
  NO_DEFAULT_PATH
)

if(NOT FFX_LIB)
  message(STATUS "amd_fidelityfx_dx12.lib not required (runtime GetProcAddress)")
  set(FFX_LIB "")
endif()

message(STATUS "FFX_LIB = ${FFX_LIB}")

# --- Runtime DLL (copied next to the EXE) ----------------------------------
set(_FFX_DLL "")
foreach(_dir ${_FFX_LIB_CANDIDATES})
  if(EXISTS "${_dir}/amd_fidelityfx_dx12.dll")
    set(_FFX_DLL "${_dir}/amd_fidelityfx_dx12.dll")
    break()
  endif()
endforeach()
if(NOT _FFX_DLL)
  message(WARNING "amd_fidelityfx_dx12.dll not found; copy it beside FSRScale.exe manually.")
else()
  message(STATUS "FFX_DLL = ${_FFX_DLL}")
endif()

# ---------------------------------------------------------------------------
add_executable(FSRScale WIN32
  src/main.cpp
  src/graphics.cpp
  src/capture.cpp
  src/fsr.cpp
  src/amdof.cpp
  src/ui.cpp
  src/target.cpp
)

target_include_directories(FSRScale PRIVATE
  "${FFX_INCLUDE}"
  "${CMAKE_SOURCE_DIR}/include"
)

target_compile_definitions(FSRScale PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

target_link_libraries(FSRScale PRIVATE
  d3d11 d3d12 dxgi dxguid windowsapp shcore d3dcompiler dwmapi gdi32
)

set_target_properties(FSRScale PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:FSRScale>"
)

if(_FFX_DLL)
  add_custom_command(TARGET FSRScale POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_FFX_DLL}"
      "$<TARGET_FILE_DIR:FSRScale>"
  )
endif()
