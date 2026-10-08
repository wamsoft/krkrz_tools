krt_add_tool(krkrsign
    SOURCES main.cpp
    WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
    LIBS    krt_sig
)
target_compile_definitions(krkrsign PRIVATE KRT_VERSION="${PROJECT_VERSION}")
