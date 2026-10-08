#---------------------------------------------------------------------------
# krt_xp3 — xp3 アーカイブの読み書き
#---------------------------------------------------------------------------
find_package(ZLIB REQUIRED)

add_library(krt_xp3 STATIC
    src/Xp3.cpp
)
target_include_directories(krt_xp3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(krt_xp3 PUBLIC krt_app PRIVATE ZLIB::ZLIB)
