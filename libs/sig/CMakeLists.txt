#---------------------------------------------------------------------------
# krt_sig — 電子署名 (.sig / exe 埋め込み署名) の検証
#
# vcpkg の libtomcrypt / libtommath は CMake の設定ファイルを提供しないので
# 自前で探す (krkrz_dev の sigcheck プラグインと同じ)。
#---------------------------------------------------------------------------
find_path(LIBTOMCRYPT_INCLUDE_DIR tomcrypt.h REQUIRED)
find_path(LIBTOMMATH_INCLUDE_DIR tommath.h REQUIRED)
find_library(LIBTOMCRYPT_LIBRARY tomcrypt REQUIRED)
find_library(LIBTOMMATH_LIBRARY tommath REQUIRED)

add_library(krt_sig STATIC
    src/Signature.cpp
)
target_include_directories(krt_sig
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${LIBTOMCRYPT_INCLUDE_DIR} ${LIBTOMMATH_INCLUDE_DIR})
target_compile_definitions(krt_sig PRIVATE LTM_DESC USE_LTM)
target_link_libraries(krt_sig
    PUBLIC  krt_app
    PRIVATE ${LIBTOMCRYPT_LIBRARY} ${LIBTOMMATH_LIBRARY})
