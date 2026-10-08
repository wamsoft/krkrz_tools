krt_add_tool(krkrcheck
    SOURCES main.cpp FileCheck.cpp
    WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
    LIBS    krt_sig
)
target_compile_definitions(krkrcheck PRIVATE KRT_VERSION="${PROJECT_VERSION}")
