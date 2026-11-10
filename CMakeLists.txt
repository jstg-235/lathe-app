cmake_minimum_required(VERSION 3.22)
# Must come before project(): CMake fixes the deployment target when the project starts.
# 10.15 is the floor because the DDP code uses std::filesystem.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS")
project(Lathe VERSION 0.11.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LATHE_BUILD_APP "Build the Lathe application (needs JUCE)" ON)
option(LATHE_BUILD_TESTS "Build the core unit tests" ON)

# ---- Core: Red Book model, DDP writer/verifier, meters, dither. No framework deps.
add_library(lathe_core STATIC
    core/cd/Disc.cpp
    core/cd/CdTextEncoder.cpp
    core/ddp/Ddp.cpp
    core/dsp/Meters.cpp
    core/dsp/Analyzer.cpp
    core/dsp/Scope.cpp
    core/dsp/Restoration.cpp
    core/util/Hash.cpp
    core/util/FlacTags.cpp
    core/doc/Pdf.cpp)
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
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Lathe records from your audio interface's inputs."
    PLUGINHOST_AU TRUE)

target_sources(Lathe PRIVATE
    app/Source/Main.cpp
    app/Source/MainComponent.cpp
    app/Source/Session.cpp
    app/Source/ProgramRenderer.cpp
    app/Source/AudioEngine.cpp
    app/Source/PluginChain.cpp
    app/Source/PluginScan.cpp
    app/Source/PluginManager.cpp
    app/Source/Exporter.cpp
    app/Source/Timeline.cpp
    app/Source/EditPanels.cpp
    app/Source/SourceView.cpp
    app/Source/MarkInfo.cpp
    app/Source/Metering.cpp
    app/Source/AutoMarks.cpp
    app/Source/SegmentList.cpp
    app/Source/NullTest.cpp
    app/Source/Commands.cpp
    app/Source/RegionFx.cpp
    app/Source/ChainPanel.cpp
    app/Source/DdpFormat.cpp
    app/Source/DdpPlayer.cpp
    app/Source/ControlSurface.cpp
    app/Source/Restore.cpp
    app/Source/Products.cpp
    app/Source/Paperwork.cpp
    app/Source/DeliveryPanels.cpp)

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
    target_sources(lathe_render_tests PRIVATE tests/render_tests.cpp app/Source/AutoMarks.cpp app/Source/ProgramRenderer.cpp app/Source/Session.cpp app/Source/Products.cpp app/Source/NullTest.cpp app/Source/Commands.cpp app/Source/RegionFx.cpp app/Source/PluginChain.cpp app/Source/PluginScan.cpp)
    target_include_directories(lathe_render_tests PRIVATE ${r8b_SOURCE_DIR})
    target_compile_definitions(lathe_render_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1 JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_AU=0)
    target_link_libraries(lathe_render_tests PRIVATE lathe_core juce::juce_audio_formats juce::juce_gui_basics juce::juce_audio_processors
        juce::juce_recommended_config_flags)
    add_test(NAME render COMMAND lathe_render_tests)
endif()

if(LATHE_BUILD_TESTS)
    juce_add_console_app(lathe_batch_tests PRODUCT_NAME "lathe_batch_tests")
    target_sources(lathe_batch_tests PRIVATE tests/batch_tests.cpp app/Source/Exporter.cpp app/Source/AudioEngine.cpp
        app/Source/PluginChain.cpp app/Source/PluginScan.cpp app/Source/RegionFx.cpp app/Source/ProgramRenderer.cpp app/Source/Session.cpp
        app/Source/DdpFormat.cpp app/Source/ControlSurface.cpp app/Source/Commands.cpp app/Source/Restore.cpp
        app/Source/Products.cpp app/Source/Paperwork.cpp)
    target_include_directories(lathe_batch_tests PRIVATE ${r8b_SOURCE_DIR})
    target_compile_definitions(lathe_batch_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_FLAC=1
        JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_AU=0)
    target_link_libraries(lathe_batch_tests PRIVATE lathe_core juce::juce_audio_utils juce::juce_audio_processors
        juce::juce_audio_formats juce::juce_audio_devices juce::juce_gui_extra juce::juce_recommended_config_flags)
    add_test(NAME batch COMMAND lathe_batch_tests)

    # Crash-proof plugin scanning: the test binary is also its own scan helper.
    juce_add_console_app(lathe_scan_tests PRODUCT_NAME "lathe_scan_tests")
    target_sources(lathe_scan_tests PRIVATE tests/scan_tests.cpp app/Source/PluginScan.cpp)
    target_compile_definitions(lathe_scan_tests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_AU=0)
    target_link_libraries(lathe_scan_tests PRIVATE juce::juce_audio_processors juce::juce_gui_basics juce::juce_recommended_config_flags)
    add_test(NAME scan COMMAND lathe_scan_tests)
endif()
