krt_add_tool(krkrloop
    SOURCES main.cpp
    WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
    LIBS    krt_audio krt_loop
)
target_compile_definitions(krkrloop PRIVATE KRT_VERSION="${PROJECT_VERSION}")
