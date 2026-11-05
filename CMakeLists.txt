cmake_minimum_required(VERSION 3.22)
project(Lathe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS")

option(LATHE_BUILD_APP "Build the Lathe application (needs JUCE)" ON)
option(LATHE_BUILD_TESTS "Build the core unit tests" ON)

# ---- Core: Red Book model, DDP writer/verifier, meters, dither. No framework deps.
add_library(lathe_core STATIC
    core/cd/Disc.cpp
    core/cd/CdTextEncoder.cpp
    core/ddp/Ddp.cpp
    core/dsp/Meters.cpp
    core/util/Hash.cpp)
target_include_directories(lathe_core PUBLIC core)
set_target_properties(lathe_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LATHE_BUILD_TESTS)
    enable_testing()
    add_executable(lathe_core_tests tests/core_tests.cpp)
    target_link_libraries(lathe_core_tests PRIVATE lathe_core)
    add_test(NAME core COMMAND lathe_core_tests)
endif()

if(NOT LATHE_BUILD_APP)
    return()
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(DEFINED LATHE_JUCE_DIR)
    add_subdirectory(${LATHE_JUCE_DIR} juce)
else()
    FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

# r8brain-free-src: header-only, MIT licence, high-quality sample-rate conversion.
if(DEFINED LATHE_R8B_DIR)
    set(r8b_SOURCE_DIR ${LATHE_R8B_DIR})
else()
    FetchContent_Declare(r8b GIT_REPOSITORY https://github.com/avaneev/r8brain-free-src.git GIT_TAG master GIT_SHALLOW TRUE)
    FetchContent_Populate(r8b)
endif()

juce_add_gui_app(Lathe
    PRODUCT_NAME "Lathe"
    COMPANY_NAME "newconstellation"
    BUNDLE_ID "com.newconstellation.lathe"
    VERSION ${PROJECT_VERSION}
    DOCUMENT_EXTENSIONS lathe
    MICROPHONE_PERMISSION_ENABLED FALSE
    PLUGINHOST_AU TRUE)

target_sources(Lathe PRIVATE
    app/Source/Main.cpp
    app/Source/MainComponent.cpp
    app/Source/Session.cpp
    app/Source/ProgramRenderer.cpp
    app/Source/AudioEngine.cpp
    app/Source/PluginChain.cpp
    app/Source/Exporter.cpp
    app/Source/Timeline.cpp
    app/Source/EditPanels.cpp)

target_include_directories(Lathe PRIVATE ${r8b_SOURCE_DIR})

target_compile_definitions(Lathe PRIVATE
    LATHE_VERSION="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="Lathe"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_FLAC=1
    JUCE_USE_MP3AUDIOFORMAT=0)

target_link_libraries(Lathe PRIVATE
    lathe_core
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(LATHE_BUILD_TESTS)
    juce_add_console_app(lathe_render_tests PRODUCT_NAME "lathe_render_tests")
    target_sources(lathe_render_tests PRIVATE tests/render_tests.cpp app/Source/ProgramRenderer.cpp app/Source/Session.cpp)
    target_include_directories(lathe_render_tests PRIVATE ${r8b_SOURCE_DIR})
    target_compile_definitions(lathe_render_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1)
    target_link_libraries(lathe_render_tests PRIVATE lathe_core juce::juce_audio_formats juce::juce_gui_basics
        juce::juce_recommended_config_flags)
    add_test(NAME render COMMAND lathe_render_tests)
endif()
