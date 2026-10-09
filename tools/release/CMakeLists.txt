krt_add_tool(krkrrelease
    SOURCES main.cpp
    WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
    LIBS    krt_release
)
target_compile_definitions(krkrrelease PRIVATE KRT_VERSION="${PROJECT_VERSION}")
