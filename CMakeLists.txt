cmake_minimum_required(VERSION 3.16)
project(EdgeLivePlayer VERSION 0.1 LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg")

add_library(ffmpeg_deps INTERFACE)
add_library(ffmpeg::deps ALIAS ffmpeg_deps)
target_include_directories(ffmpeg_deps INTERFACE "${FFMPEG_DIR}/include")
target_link_directories(ffmpeg_deps INTERFACE "${FFMPEG_DIR}/lib")
target_link_libraries(ffmpeg_deps INTERFACE avcodec avformat avutil swscale swresample)

add_subdirectory(core)
add_subdirectory(app/qt)
add_subdirectory(tools/lifecycle_stress)

add_executable(audio_probe tools/audio_probe.cpp)
target_include_directories(audio_probe PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(audio_probe PRIVATE edgelive_core)

add_executable(rtsp_loop_push tools/rtsp_loop_push.cpp)
target_include_directories(rtsp_loop_push PRIVATE ${CMAKE_SOURCE_DIR})

