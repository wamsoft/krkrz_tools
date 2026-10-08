krt_add_tool(krkrimg
    SOURCES main.cpp
    WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
    LIBS    krt_image
)
target_compile_definitions(krkrimg PRIVATE KRT_VERSION="${PROJECT_VERSION}")
