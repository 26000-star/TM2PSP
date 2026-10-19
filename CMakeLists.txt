# TM2PSP: host build (Windows/Linux, gcc or clang) and, later, PSP build (psp-cmake sets PSP).
#   cmake -S . -B build/host -G Ninja && cmake --build build/host
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
  src/shim/${TM_PLATFORM}/fs_${TM_PLATFORM}.c
  src/shim/${TM_PLATFORM}/os_${TM_PLATFORM}.c)
target_include_directories(tm_shim PUBLIC src/shim/include)
if(NOT WIN32)
  target_link_libraries(tm_shim PUBLIC m)
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

# ---- host programs ----
if(TM_PLATFORM STREQUAL host)
  add_executable(tm_cfgdump src/host/cfgdump.cpp)
  target_link_libraries(tm_cfgdump PRIVATE tm_game)
  add_executable(tm_trackdump src/host/trackdump.cpp)
  target_link_libraries(tm_trackdump PRIVATE tm_game)
  add_executable(tm_physics src/host/physicstest.cpp)
  target_link_libraries(tm_physics PRIVATE tm_game)
  if(WIN32)
    target_link_options(tm_cfgdump PRIVATE -static)
    target_link_options(tm_trackdump PRIVATE -static)
    target_link_options(tm_physics PRIVATE -static)
  endif()
endif()
