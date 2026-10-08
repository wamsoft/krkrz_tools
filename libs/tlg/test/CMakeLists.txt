#---------------------------------------------------------------------------
# krt_tlg の単体テスト (単独プロジェクト)
#   cmake -S libs/tlg/test -B build/tlgtest -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/tlgtest
#   build/tlgtest/tlgtest selftest
#---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(tlgtest CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. tlg)

add_executable(tlgtest tlgtest.cpp)
target_link_libraries(tlgtest PRIVATE krt_tlg)
if(MSVC)
    target_compile_options(tlgtest PRIVATE /utf-8)
endif()
