add_executable(lifecycle_stress
    main.cpp
)

target_include_directories(lifecycle_stress PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(lifecycle_stress PRIVATE edgelive_core)
