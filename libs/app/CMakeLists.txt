#---------------------------------------------------------------------------
# krt_app — ツール共通の枠 (GUI / CLI の振り分け、長い処理の実行と進捗、
# 文字列・パスの変換)
#---------------------------------------------------------------------------
add_library(krt_app STATIC
    src/ToolApp.cpp
    src/JobRunner.cpp
    src/Progress.cpp
    src/Text.cpp
)
target_include_directories(krt_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(krt_app PUBLIC appserve::core)
