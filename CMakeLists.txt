#---------------------------------------------------------------------------
# krkrz_tools — 吉里吉里Z の標準ツール群
#
# 各ツールは «ブラウザを画面にするネイティブアプリ» (appserve) で、同じ処理を
# コマンドラインからも呼べる。処理の本体は libs/ の共通ライブラリに置き、
# GUI と CLI はその薄い入口にする。
#
#   cmake --preset windows
#   cmake --build --preset windows-rel
#
# 吉里吉里Z 本体のソースは KRKRZ_BASE (krkrz_dev の親フォルダ) から参照する
# (外枠リポジトリ krkrz_android / krkrz_linux と同じ作法)。
#---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)

project(krkrz_tools VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 配布物を exe 1 本にするため、MSVC のランタイムは静的リンクにする
# (vcpkg の triplet も *-windows-static に揃える。CMakePresets.json)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/utf-8 /W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

#---------------------------------------------------------------------------
# 吉里吉里Z 本体のソース (今は任意。TLG / XP3 / .sli などを共通ライブラリへ
# 取り込む段階で必須になる)
#---------------------------------------------------------------------------
if(DEFINED ENV{KRKRZ_BASE})
    cmake_path(SET KRKRZ_BASE "$ENV{KRKRZ_BASE}")
endif()
set(KRKRZ_BASE "${KRKRZ_BASE}" CACHE PATH "krkrz_dev を置いている親フォルダ")
set(KRKRZ_DEV_DIR "${KRKRZ_BASE}/krkrz_dev")
if(KRKRZ_BASE AND EXISTS "${KRKRZ_DEV_DIR}/src/core")
    message(STATUS "krkrz_dev: ${KRKRZ_DEV_DIR}")
else()
    message(STATUS "krkrz_dev: not found (KRKRZ_BASE が未設定。本体ソースを使うツールはまだ無いので続行)")
endif()

#---------------------------------------------------------------------------
# appserve (ブラウザ UI のフレームワーク)
#---------------------------------------------------------------------------
set(APPSERVE_BUILD_APP OFF CACHE BOOL "" FORCE)
add_subdirectory(external/appserve)

# psdparse / clipparse (PSD・CLIP STUDIO の読み込みと合成)
add_subdirectory(external/psdparse EXCLUDE_FROM_ALL)
set(CLIPPARSE_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(external/clipparse EXCLUDE_FROM_ALL)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(KrtTool)

#---------------------------------------------------------------------------
# 共通ライブラリとツール
#---------------------------------------------------------------------------
add_subdirectory(libs/app)
add_subdirectory(libs/sig)
add_subdirectory(libs/xp3)
add_subdirectory(libs/loop)
add_subdirectory(libs/audio)
add_subdirectory(libs/tlg)
add_subdirectory(libs/image)

add_subdirectory(tools/filecheck)
add_subdirectory(tools/sign)
add_subdirectory(tools/xp3)
add_subdirectory(tools/audio)
add_subdirectory(tools/image)
add_subdirectory(tools/loop)
