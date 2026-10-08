#---------------------------------------------------------------------------
# krt_tlg — 吉里吉里Z の TLG5 / TLG6 画像コーデック (外部依存なし)
#
# 本体 src/core/common/visual/{SaveTLG5,SaveTLG6,LoadTLG}.cpp と tvpgl.c (C 版)
# からの移植。エンコード結果は本体の Bitmap.save とバイト一致する。
#---------------------------------------------------------------------------
add_library(krt_tlg STATIC
    src/Tlg.cpp
    src/TlgEncode5.cpp
    src/TlgEncode6.cpp
    src/TlgDecode.cpp
    src/SlideCompressor.cpp
)
target_include_directories(krt_tlg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(krt_tlg PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(krt_tlg PRIVATE /utf-8)
endif()
