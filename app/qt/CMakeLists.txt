set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Multimedia OpenGLWidgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Multimedia OpenGLWidgets)

if(UNIX AND NOT APPLE)
    find_library(ALSA_LIBRARY asound)
    find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
    if(ALSA_LIBRARY AND ALSA_INCLUDE_DIR)
        add_compile_definitions(
            EDGELIVE_HAVE_ALSA_AUDIO_CLOCK=1
            EDGELIVE_HAVE_ALSA_NATIVE_OUTPUT=1
        )
    endif()
endif()

set(APP_SOURCES
    alsa_audio_output.cpp
    alsa_audio_output.h
    audio_clock/audio_clock_provider.h
    audio_clock/audio_clock_provider_factory.cpp
    audio_clock/qt_estimated_audio_clock_provider.cpp
    audio_clock/qt_estimated_audio_clock_provider.h
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    videowidget.cpp
    videowidget.h
    resources.qrc
)

if(WIN32)
    list(APPEND APP_SOURCES
        audio_clock/wasapi_audio_clock_provider.cpp
        audio_clock/wasapi_audio_clock_provider.h
    )
elseif(APPLE)
    list(APPEND APP_SOURCES
        audio_clock/coreaudio_audio_clock_provider.cpp
        audio_clock/coreaudio_audio_clock_provider.h
    )
elseif(UNIX)
    if(ALSA_LIBRARY)
        list(APPEND APP_SOURCES
            audio_clock/alsa_audio_clock_provider.cpp
            audio_clock/alsa_audio_clock_provider.h
        )
    endif()
endif()

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(edgelive_demo_qt MANUAL_FINALIZATION ${APP_SOURCES})
else()
    add_executable(edgelive_demo_qt ${APP_SOURCES})
endif()

target_include_directories(edgelive_demo_qt PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
if(UNIX AND NOT APPLE)
    if(ALSA_INCLUDE_DIR)
        target_include_directories(edgelive_demo_qt PRIVATE ${ALSA_INCLUDE_DIR})
    endif()
endif()

target_link_libraries(edgelive_demo_qt
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Multimedia
        Qt${QT_VERSION_MAJOR}::OpenGLWidgets
        edgelive_core
)

if(WIN32)
    target_link_libraries(edgelive_demo_qt PRIVATE mmdevapi audioclient)
elseif(APPLE)
    target_link_libraries(edgelive_demo_qt PRIVATE "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
    if(ALSA_LIBRARY)
        target_link_libraries(edgelive_demo_qt PRIVATE ${ALSA_LIBRARY})
    endif()
endif()

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(edgelive_demo_qt)
endif()

