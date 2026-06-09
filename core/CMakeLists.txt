add_library(edgelive_core STATIC
    log/logger.cpp
    player/player_engine.cpp
)

target_include_directories(edgelive_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(edgelive_core
    PUBLIC
        ffmpeg::deps
)

