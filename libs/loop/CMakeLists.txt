#---------------------------------------------------------------------------
# krt_loop — ループ情報 (.sli) の読み書き
#---------------------------------------------------------------------------
add_library(krt_loop STATIC
    src/Sli.cpp
)
target_include_directories(krt_loop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
