krt_add_tool(krkrxp3
    SOURCES main.cpp
    WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
    LIBS    krt_xp3
)
target_compile_definitions(krkrxp3 PRIVATE KRT_VERSION="${PROJECT_VERSION}")
