#---------------------------------------------------------------------------
# krt_release — 配布用の実行可能ファイルを作る (Windows の吉里吉里Z の exe が対象)
#
# リソースの書き換えに Windows API (BeginUpdateResource など) を使うので Windows 専用
#---------------------------------------------------------------------------
add_library(krt_release STATIC
    src/Release.cpp
    src/PeResources.cpp
)
target_include_directories(krt_release PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(krt_release PUBLIC krt_app PRIVATE krt_sig krt_xp3)
# RT_ICON / RT_VERSION などをワイド文字版で使う
target_compile_definitions(krt_release PRIVATE UNICODE _UNICODE)
