cmake_minimum_required(VERSION 3.10)
project(tm2psp_viewer C)

add_executable(viewer main.c)
target_compile_options(viewer PRIVATE -O2 -Wall)
target_link_libraries(viewer pspgum pspgu pspctrl pspdisplay pspge m)
create_pbp_file(TARGET viewer ICON_PATH NULL BACKGROUND_PATH NULL PREVIEW_PATH NULL TITLE "TM2PSP model viewer")
