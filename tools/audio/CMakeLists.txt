krt_add_tool(krkraudio
    SOURCES main.cpp
    WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
    LIBS    krt_audio krt_loop
)
target_compile_definitions(krkraudio PRIVATE KRT_VERSION="${PROJECT_VERSION}")
