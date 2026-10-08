#---------------------------------------------------------------------------
# krt_audio — 音声の読み書き (WAV / Ogg Vorbis / Ogg Opus) と解析 (ラウドネス・音量)
#
# vcpkg: libogg / libvorbis / opus / opusfile / libopusenc / libebur128
# (libopusenc / libebur128 は CMake の設定ファイルを提供しないので自前で探す)
#---------------------------------------------------------------------------
find_package(Ogg CONFIG REQUIRED)
find_package(Vorbis CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(OpusFile CONFIG REQUIRED)
find_library(OPUSENC_LIBRARY opusenc REQUIRED)
find_library(EBUR128_LIBRARY ebur128 REQUIRED)
find_path(EBUR128_INCLUDE_DIR ebur128.h REQUIRED)

add_library(krt_audio STATIC
    src/Audio.cpp
)
target_include_directories(krt_audio
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${EBUR128_INCLUDE_DIR})
target_link_libraries(krt_audio
    PUBLIC  krt_app
    PRIVATE OpusFile::opusfile ${OPUSENC_LIBRARY} Opus::opus
            Vorbis::vorbisenc Vorbis::vorbisfile Vorbis::vorbis Ogg::ogg ${EBUR128_LIBRARY})
