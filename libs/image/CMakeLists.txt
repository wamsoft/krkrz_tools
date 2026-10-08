#---------------------------------------------------------------------------
# krt_image — 画像の読み書き (BMP / PNG / JPEG / TLG5 / TLG6 / PSD / CLIP) と前処理
#
# vcpkg: libpng / libjpeg-turbo。TLG は libs/tlg、PSD は external/psdparse、CLIP は external/clipparse
#---------------------------------------------------------------------------
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)

add_library(krt_image STATIC
    src/Image.cpp
    src/Bmp.cpp
    src/Png.cpp
    src/Jpeg.cpp
    src/Psd.cpp
    src/Clip.cpp
    src/Layers.cpp
)
target_include_directories(krt_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(krt_image
    PRIVATE krt_tlg psdparse clipparse PNG::PNG JPEG::JPEG)
