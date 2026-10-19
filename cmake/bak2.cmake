# ---------------------------------------------------------------------------
# Vendored "SkillEffect Studio" (BAK-2) effect / model renderer, namespace
# se::render (src/bak2/render, copied as-is; src/bak2/skillplay/SkillPlay.h and
# src/bak2/core/Text.h are the headers RPlayHost / PlayerDress include).
# bak2view = the pimpl bridge (src/bak2_view.hpp, std + HWND only) the app
# embeds in its "3B Model" tab. Included from the top-level CMakeLists.txt.
# ---------------------------------------------------------------------------

set(BAK2_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(BAK2_RENDER ${BAK2_ROOT}/src/bak2/render)

set(BAK2_MSVC_FLAGS /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
set(BAK2_DEFS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# core / editor sources define WIN32_LEAN_AND_MEAN themselves (unguarded): not passed again there.
set(BAK2_DEFS_SELF_LEAN UNICODE _UNICODE NOMINMAX)

add_library(bak2render STATIC
  ${BAK2_RENDER}/RImage.cpp
  ${BAK2_RENDER}/RAssets.cpp
  ${BAK2_RENDER}/RFormats.cpp
  ${BAK2_RENDER}/Efp.cpp
  ${BAK2_RENDER}/EfpSim.cpp
  ${BAK2_RENDER}/EfxDraw.cpp
  ${BAK2_RENDER}/EfxSheet.cpp
  ${BAK2_RENDER}/SoftRaster.cpp
  ${BAK2_RENDER}/RModel.cpp
  ${BAK2_RENDER}/RScene.cpp
  ${BAK2_RENDER}/RView.cpp
  ${BAK2_RENDER}/RGif.cpp
  ${BAK2_RENDER}/RPlayHost.cpp
  ${BAK2_RENDER}/PlayerDress.cpp
  ${BAK2_RENDER}/EfxThumb.cpp
)
target_compile_options(bak2render PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2render PRIVATE ${BAK2_DEFS})
target_link_libraries(bak2render PUBLIC opengl32 glu32 gdi32 user32)

add_library(bak2view STATIC ${BAK2_ROOT}/src/bak2_view.cpp)
target_include_directories(bak2view PUBLIC ${BAK2_ROOT}/src)
target_link_libraries(bak2view PUBLIC bak2render pk2core)
target_compile_options(bak2view PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2view PRIVATE ${BAK2_DEFS})

# Console checks against the real archives (read-only, through bakbridge's reader): -DSROPK2_BUILD_TESTS=ON.
if(SROPK2_BUILD_TESTS AND TARGET bakbridge)
  add_executable(bak2view_test ${BAK2_ROOT}/tests/bak2_view_test.cpp)
  target_link_libraries(bak2view_test PRIVATE bak2view bakbridge)
  target_compile_options(bak2view_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
  target_compile_definitions(bak2view_test PRIVATE ${BAK2_DEFS})
endif()

# ---------------------------------------------------------------------------
# BAK-2 tool modules, copied as-is: core (namespace se: Text, Table, TextDiff,
# SkillCrypt + SkillKeyData, and the rest of core TextDiff's Studio prova pulls
# in), ddj (se::ddj: DXT / DDJ writer, picture readers, icon import, portraits)
# and the GUI-less editor (se::ed: EdDoc, EdSchema, EdValidate, EdCompare,
# EdHealth + EdAssets / EdVorbis it uses, EdHistory, EdLang, EdTemplates).
# bak2tools = the std-only bridge (src/bak2_tools.hpp).
# ---------------------------------------------------------------------------
set(BAK2_SRC ${BAK2_ROOT}/src/bak2)

add_library(bak2core STATIC
  ${BAK2_SRC}/core/Client.cpp
  ${BAK2_SRC}/core/Engine.cpp
  ${BAK2_SRC}/core/List.cpp
  ${BAK2_SRC}/core/Ops.cpp
  ${BAK2_SRC}/core/Package.cpp
  ${BAK2_SRC}/core/Pk2.cpp
  ${BAK2_SRC}/core/SeModel.cpp
  ${BAK2_SRC}/core/SkillCrypt.cpp
  ${BAK2_SRC}/core/Source.cpp
  ${BAK2_SRC}/core/Studio.cpp
  ${BAK2_SRC}/core/Table.cpp
  ${BAK2_SRC}/core/Text.cpp
  ${BAK2_SRC}/core/TextDiff.cpp
  ${BAK2_SRC}/core/Validate.cpp
)
target_compile_options(bak2core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2core PRIVATE ${BAK2_DEFS_SELF_LEAN})
target_link_libraries(bak2core PUBLIC user32 advapi32 shell32)

add_library(bak2ddj STATIC
  ${BAK2_SRC}/ddj/Dxt.cpp
  ${BAK2_SRC}/ddj/ImageIo.cpp
  ${BAK2_SRC}/ddj/DdjCodec.cpp
  ${BAK2_SRC}/ddj/IconImport.cpp
  ${BAK2_SRC}/ddj/Portrait.cpp
)
target_compile_options(bak2ddj PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2ddj PRIVATE ${BAK2_DEFS})
target_link_libraries(bak2ddj PUBLIC bak2render windowscodecs ole32)

add_library(bak2editor STATIC
  ${BAK2_SRC}/editor/EdSchema.cpp
  ${BAK2_SRC}/editor/EdDoc.cpp
  ${BAK2_SRC}/editor/EdValidate.cpp
  ${BAK2_SRC}/editor/EdTemplates.cpp
  ${BAK2_SRC}/editor/EdCompare.cpp
  ${BAK2_SRC}/editor/EdHealth.cpp
  ${BAK2_SRC}/editor/EdAssets.cpp
  ${BAK2_SRC}/editor/EdVorbis.cpp
  ${BAK2_SRC}/editor/EdHistory.cpp
  ${BAK2_SRC}/editor/EdLang.cpp
)
target_compile_options(bak2editor PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2editor PRIVATE ${BAK2_DEFS_SELF_LEAN})
target_link_libraries(bak2editor PUBLIC bak2core)

add_library(bak2tools STATIC ${BAK2_ROOT}/src/bak2_tools.cpp)
target_include_directories(bak2tools PUBLIC ${BAK2_ROOT}/src)
target_link_libraries(bak2tools PUBLIC bak2ddj bak2editor bak2core pk2core)
target_compile_options(bak2tools PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2tools PRIVATE ${BAK2_DEFS})

if(SROPK2_BUILD_TESTS AND TARGET bakbridge)
  add_executable(bak2tools_test ${BAK2_ROOT}/tests/bak2_tools_test.cpp)
  target_link_libraries(bak2tools_test PRIVATE bak2tools bakbridge)
  target_compile_options(bak2tools_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
  target_compile_definitions(bak2tools_test PRIVATE ${BAK2_DEFS})
endif()

# ---------------------------------------------------------------------------
# BAK-2 modules, copied as-is:
#   efpwrite  (se::efpw: lossless .efp Parse / Serialize, recolour / scale / speed /
#             duration / texture + mesh swap, variant pipeline; EfpwCli left out)
#   skillplay (se::sp: FxDb, BsrClips, Timeline, Player, Describe, Diagnose,
#             Golden, SideFx; drives render::ScenePlayHost)
#   client    (se::client: Catalog over a Vfs - characterdata / itemdata / skilldata
#             ENC / textdata names; Pk2Archive / Pk2Crypto / KeyFinder are linked
#             because Vfs.cpp references them, the bridge never opens an archive)
# Bridges: bak2edit (src/bak2_edit.hpp) and bak2skill (src/bak2_skill.hpp).
# ---------------------------------------------------------------------------
add_library(bak2efpw STATIC
  ${BAK2_SRC}/efpwrite/EfpDoc.cpp
  ${BAK2_SRC}/efpwrite/EfpEdit.cpp
  ${BAK2_SRC}/efpwrite/EfpTexture.cpp
  ${BAK2_SRC}/efpwrite/EfpVariant.cpp
)
target_compile_options(bak2efpw PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2efpw PRIVATE ${BAK2_DEFS})
target_link_libraries(bak2efpw PUBLIC bak2render)

add_library(bak2skillplay STATIC
  ${BAK2_SRC}/skillplay/FxDb.cpp
  ${BAK2_SRC}/skillplay/BsrClips.cpp
  ${BAK2_SRC}/skillplay/Timeline.cpp
  ${BAK2_SRC}/skillplay/Player.cpp
  ${BAK2_SRC}/skillplay/Describe.cpp
  ${BAK2_SRC}/skillplay/Diagnose.cpp
  ${BAK2_SRC}/skillplay/Golden.cpp
  ${BAK2_SRC}/skillplay/SideFx.cpp
)
target_compile_options(bak2skillplay PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS} /bigobj>)
target_compile_definitions(bak2skillplay PRIVATE ${BAK2_DEFS})
target_link_libraries(bak2skillplay PUBLIC bak2core)

add_library(bak2client STATIC
  ${BAK2_SRC}/client/Pk2Crypto.cpp
  ${BAK2_SRC}/client/Pk2Archive.cpp
  ${BAK2_SRC}/client/KeyFinder.cpp
  ${BAK2_SRC}/client/Vfs.cpp
  ${BAK2_SRC}/client/Catalog.cpp
  ${BAK2_SRC}/client/ClientInfo.cpp
)
target_compile_options(bak2client PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2client PRIVATE ${BAK2_DEFS_SELF_LEAN})
target_link_libraries(bak2client PUBLIC bak2core)

add_library(bak2edit STATIC ${BAK2_ROOT}/src/bak2_edit.cpp)
target_include_directories(bak2edit PUBLIC ${BAK2_ROOT}/src)
target_link_libraries(bak2edit PUBLIC bak2efpw pk2core)
target_compile_options(bak2edit PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2edit PRIVATE ${BAK2_DEFS})

add_library(bak2skill STATIC ${BAK2_ROOT}/src/bak2_skill.cpp)
target_include_directories(bak2skill PUBLIC ${BAK2_ROOT}/src)
target_link_libraries(bak2skill PUBLIC bak2client bak2skillplay bak2render pk2core)
target_compile_options(bak2skill PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
target_compile_definitions(bak2skill PRIVATE ${BAK2_DEFS})
# View::play_skill builds its scene through the skill bridge's session code.
target_link_libraries(bak2view PUBLIC bak2skill)

if(SROPK2_BUILD_TESTS AND TARGET bakbridge)
  add_executable(bak2more_test ${BAK2_ROOT}/tests/bak2_more_test.cpp)
  target_link_libraries(bak2more_test PRIVATE bak2edit bak2view bakbridge)
  target_compile_options(bak2more_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:${BAK2_MSVC_FLAGS}>)
  target_compile_definitions(bak2more_test PRIVATE ${BAK2_DEFS})
endif()
