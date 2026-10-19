cmake_minimum_required(VERSION 4.0)
project(SroPk2Studio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)              # MSVC: maps to /std:c++latest (C++26 features)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)     # no C++ modules here; avoids spurious CXXInterface rules on Ninja
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Portable single-exe: static runtime, no third-party DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Output layout, the same as build.bat / Visual Studio: <source>\bin holds only SroPk2Studio.exe
# (Debug: bin\Debug); static libraries and test programs stay in the build tree (obj\cmake\...).
# The generator expression keeps multi-config generators from adding their own config folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<$<CONFIG:Debug>:Debug>)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Console test programs in tests/ (they read real game archives). Off for a normal build.
option(SROPK2_BUILD_TESTS "Build the console test programs in tests/" OFF)

add_library(pk2core STATIC
  src/binary_info.cpp
  src/blowfish.cpp
  src/pk2_archive.cpp
  src/text.cpp
  src/i18n.cpp
  src/file_types.cpp
  src/dds.cpp
  src/jmx_parse.cpp
)
target_include_directories(pk2core PUBLIC src)
target_compile_options(pk2core PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc>)

# Embedded public-domain Ogg Vorbis decoder (in-process .ogg playback, no DLL).
# Compiled with warnings off (/w) so the project's /W4 /WX stays clean.
add_library(stbvorbis STATIC src/thirdparty/stb_vorbis.c)
target_compile_options(stbvorbis PRIVATE $<$<C_COMPILER_ID:MSVC>:/w>)
target_compile_definitions(stbvorbis PRIVATE STB_VORBIS_NO_STDIO _CRT_SECURE_NO_WARNINGS)

# GUI application (Win32 + Direct2D/DirectWrite)
add_executable(SroPk2Studio WIN32
  src/main.cpp
  src/app.cpp
  src/file_browser.cpp
  src/model3d.cpp
  src/store.cpp
  src/app.manifest
)
target_link_libraries(SroPk2Studio PRIVATE
  pk2core bakbridge bak2view bak2tools bak2edit bak2skill stbvorbis d2d1 dwrite dwmapi ole32 shell32 shlwapi user32 gdi32 comdlg32
  d3d11 dxgi d3dcompiler winmm)
target_compile_options(SroPk2Studio PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc>)
target_compile_definitions(SroPk2Studio PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(pk2core PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# ---------------------------------------------------------------------------
# Vendored "SRO Asset Studio" (BAK) non-UI sources: archives (PK2 / PK2v2 / VP /
# folder), asset ciphers, format parsers, effect scene. Namespace `sro`.
# ---------------------------------------------------------------------------

# Third-party zstd amalgamation (used by the PK2v2 directory). Warnings off.
add_library(bakzstd STATIC src/bak/core/zstd.cpp)
target_compile_options(bakzstd PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/w /utf-8 /EHsc>)

add_library(sroasset STATIC
  src/bak/core/AssetCipherAes.cpp
  src/bak/core/AssetCipherGopro.cpp
  src/bak/core/AssetCipherSailor.cpp
  src/bak/core/AssetCipherSbox.cpp
  src/bak/core/AssetCipherVsro.cpp
  src/bak/core/AssetCrypt.cpp
  src/bak/core/Blowfish.cpp
  src/bak/core/Config.cpp
  src/bak/core/FolderArchive.cpp
  src/bak/core/KeyFinder.cpp
  src/bak/core/KeyRing.cpp
  src/bak/core/L33tPayloadKey.cpp
  src/bak/core/Pk2Archive.cpp
  src/bak/core/Pk2v2Archive.cpp
  src/bak/core/Pk2v2Crypto.cpp
  src/bak/core/Pk2v2Zstd.cpp
  src/bak/core/SiblingArchives.cpp
  src/bak/core/VpArchive.cpp
  src/bak/core/VpCrypto.cpp
  src/bak/formats/AssetDeps.cpp
  src/bak/formats/AvatarSet.cpp
  src/bak/formats/Ban.cpp
  src/bak/formats/Bms.cpp
  src/bak/formats/Bmt.cpp
  src/bak/formats/Bsk.cpp
  src/bak/formats/Bsr.cpp
  src/bak/formats/DataTableDeps.cpp
  src/bak/formats/Ddj.cpp
  src/bak/formats/DdsDecode.cpp
  src/bak/formats/Efp.cpp
  src/bak/formats/FormatId.cpp
  src/bak/formats/Formats_stubs.cpp
  src/bak/formats/MapFiles.cpp
  src/bak/formats/Nif2dt.cpp
  src/bak/formats/TextFile.cpp
  src/bak/fx/EffectScene.cpp
  src/bak/preview/PreviewEngine.cpp
  src/bak/render/Image.cpp
  src/bak/render/ImageIO.cpp
  src/bak/render/MeshView.cpp
  src/bak/render/Raster.cpp
  src/bak/render/SroPose.cpp
)
target_compile_options(sroasset PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc /bigobj>)
target_compile_definitions(sroasset PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN SRO_CHECK_SOURCE_ENCODING)
target_link_libraries(sroasset PUBLIC
  bakzstd bcrypt crypt32 advapi32 ole32 oleaut32 windowscodecs user32 gdi32 msimg32)

# Thin pimpl bridge (bak_bridge.hpp has std types only) for the app to call.
add_library(bakbridge STATIC src/bak_bridge.cpp)
target_include_directories(bakbridge PUBLIC src)
target_link_libraries(bakbridge PUBLIC sroasset pk2core)
target_compile_options(bakbridge PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc>)
target_compile_definitions(bakbridge PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# Vendored "SkillEffect Studio" (BAK-2): renderer, skill player, EFP writer, DDJ encoder, tables.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bak2.cmake)

if(SROPK2_BUILD_TESTS)
  # pk2 engine tool (extract / probe) and the BAK bridge check, against real archives.
  add_executable(pk2test tests/pk2test.cpp)
  target_link_libraries(pk2test PRIVATE pk2core)
  add_executable(bakbridge_test tests/bak_bridge_test.cpp)
  target_link_libraries(bakbridge_test PRIVATE bakbridge)
  # Which view every extension of the real archives gets (texture / 3D / text / info / raw).
  add_executable(coverage_test tests/coverage_test.cpp)
  target_link_libraries(coverage_test PRIVATE pk2core bakbridge bak2tools)
  # Translation tables: every row complete in all five languages, placeholders consistent.
  add_executable(i18n_test tests/i18n_test.cpp)
  target_link_libraries(i18n_test PRIVATE pk2core)
  foreach(t pk2test bakbridge_test coverage_test i18n_test)
    target_compile_options(${t} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc>)
    target_compile_definitions(${t} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  endforeach()
  # Keep test programs out of bin/ (bak2view_test, bak2tools_test, bak2more_test come from cmake/bak2.cmake).
  foreach(t pk2test bakbridge_test coverage_test i18n_test bak2view_test bak2tools_test bak2more_test)
    if(TARGET ${t})
      set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    endif()
  endforeach()
endif()
