# TM2PSP: host build (Windows/Linux, gcc or clang) and PSP build (psp-cmake sets PSP).
#   cmake -S . -B build/host -G Ninja && cmake --build build/host
#   bash tools/psp_build.sh            (PSP, pspdev in WSL: build/psp/game, EBOOTs in build/psp/<program>/)
cmake_minimum_required(VERSION 3.16)
project(TM2PSP C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(PSP)
  set(TM_PLATFORM psp)
else()
  set(TM_PLATFORM host)
endif()

find_package(ZLIB REQUIRED)

# ---- src/lib: original third-party sources ----
file(GLOB TM_LUA_SOURCES src/lib/lua-5.0.2/src/*.c src/lib/lua-5.0.2/src/lib/*.c)
add_library(tm_lua STATIC ${TM_LUA_SOURCES} src/lib/lua-5.0.2/tm_noparser.c)
target_include_directories(tm_lua PUBLIC src/lib/lua-5.0.2/include PRIVATE src/lib/lua-5.0.2/src)
target_compile_options(tm_lua PUBLIC -include ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/lua-5.0.2/tm_luaconf.h)

# ---- src/shim: NitroSDK replacement ----
add_library(tm_shim STATIC
  src/shim/common/fx.c
  src/shim/common/snd_sdat.cpp
  src/shim/common/snd_driver.cpp
  src/shim/common/snd_stream.cpp
  src/shim/${TM_PLATFORM}/fs_${TM_PLATFORM}.c
  src/shim/${TM_PLATFORM}/os_${TM_PLATFORM}.c)
target_include_directories(tm_shim PUBLIC src/shim/include src/shim/common)
if(NOT WIN32)
  target_link_libraries(tm_shim PUBLIC m)
endif()
if(TM_PLATFORM STREQUAL psp)
  target_link_libraries(tm_shim PUBLIC pspdebug pspdisplay pspge pspctrl psprtc pspaudio)
endif()

add_library(tm_unzip STATIC src/lib/unzip/unzip.c)
target_include_directories(tm_unzip PUBLIC src/lib/unzip)
target_link_libraries(tm_unzip PUBLIC tm_shim ZLIB::ZLIB)

# ---- src/game: decompiled game code ----
file(GLOB_RECURSE TM_GAME_SOURCES CONFIGURE_DEPENDS src/game/*.cpp)
add_library(tm_game STATIC ${TM_GAME_SOURCES})
target_include_directories(tm_game PUBLIC src/game)
target_link_libraries(tm_game PUBLIC tm_shim tm_unzip tm_lua)
target_compile_options(tm_game PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---- check suite: the host test drivers in one process (host and PSP, outputs compared byte for byte) ----
set(TM_SUITE_SOURCES src/checks/suite.cpp src/checks/cfgdump_entry.cpp src/checks/trackdump_entry.cpp
    src/checks/physicstest_entry.cpp)

# ---- PSP programs ----
if(TM_PLATFORM STREQUAL psp)
  add_executable(tm_checks src/psp/checks_psp.cpp ${TM_SUITE_SOURCES})
  target_include_directories(tm_checks PRIVATE src/checks)
  target_link_libraries(tm_checks PRIVATE tm_game)
  create_pbp_file(TARGET tm_checks TITLE "TM2PSP checks" MEMSIZE 1
                  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/tm_checks_pbp)

  # The race (src/psp/race_psp.cpp): G3D stand-in on sceGu, sceCtrl input.
  add_executable(tm_race src/psp/race_psp.cpp src/app/race_session.cpp src/app/race_app.cpp src/app/fe_canvas.cpp src/app/fe_menu.cpp src/app/editor.cpp
                 src/shim/common/g3d_model.cpp src/shim/common/g2d_cells.cpp
                 src/shim/psp/g3d_gu.cpp)
  target_include_directories(tm_race PRIVATE src/app src/shim/common)
  # newlib's int32_t is long on the Allegrex (same 32 bits as int): %d on s32/fx32 is harmless there.
  target_compile_options(tm_race PRIVATE -Wall -Wno-unused-parameter -Wno-format)
  target_link_libraries(tm_race PRIVATE tm_game pspgum pspgu pspdmac psputility psppower)
  create_pbp_file(TARGET tm_race TITLE "TM2PSP - TrackMania Coast" MEMSIZE 1
                  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/tm_race_pbp)
endif()

# ---- host programs ----
if(TM_PLATFORM STREQUAL host)
  add_executable(tm_checksuite src/checks/suite_host.cpp ${TM_SUITE_SOURCES})
  target_include_directories(tm_checksuite PRIVATE src/checks)
  target_link_libraries(tm_checksuite PRIVATE tm_game)
  add_executable(tm_cfgdump src/host/cfgdump.cpp)
  target_link_libraries(tm_cfgdump PRIVATE tm_game)
  add_executable(tm_trackdump src/host/trackdump.cpp)
  target_link_libraries(tm_trackdump PRIVATE tm_game)
  add_executable(tm_physics src/host/physicstest.cpp)
  target_link_libraries(tm_physics PRIVATE tm_game)
  add_executable(tm_sdat src/host/sdat.cpp)
  target_link_libraries(tm_sdat PRIVATE tm_shim)
  if(WIN32)
    target_link_options(tm_cfgdump PRIVATE -static)
    target_link_options(tm_trackdump PRIVATE -static)
    target_link_options(tm_physics PRIVATE -static)
    target_link_options(tm_sdat PRIVATE -static)
    target_link_options(tm_checksuite PRIVATE -static)
  endif()

  # Windowed race (SDL2 + OpenGL 1.x). SDL2 is looked up in build/ext/sdl2 (built from the signed 2.30.12
  # source release, see docs/PORT.md) or the system; the program is skipped without it.
  find_package(SDL2 CONFIG QUIET PATHS ${CMAKE_CURRENT_SOURCE_DIR}/build/ext/sdl2 NO_DEFAULT_PATH)
  if(NOT SDL2_FOUND)
    find_package(SDL2 CONFIG QUIET)
  endif()
  find_package(OpenGL QUIET)
  if(SDL2_FOUND AND OPENGL_FOUND)
    add_library(tm_g3d STATIC src/shim/common/g3d_model.cpp src/shim/common/g2d_cells.cpp src/shim/host/g3d_gl.cpp)
    target_include_directories(tm_g3d PUBLIC src/shim/common src/shim/host)
    target_link_libraries(tm_g3d PUBLIC tm_shim OpenGL::GL)
    add_executable(tm_race src/host/race.cpp src/app/race_session.cpp src/app/race_app.cpp src/app/fe_canvas.cpp src/app/fe_menu.cpp src/app/editor.cpp)
    target_include_directories(tm_race PRIVATE src/app)
    target_link_libraries(tm_race PRIVATE tm_game tm_g3d SDL2::SDL2main SDL2::SDL2-static)
    if(WIN32)
      target_link_options(tm_race PRIVATE -static)
    endif()
  else()
    message(STATUS "SDL2 or OpenGL not found: tm_race is not built")
  endif()
endif()
