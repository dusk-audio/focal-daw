include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(focal-tests
    smoke_brickwall_limiter.cpp
    session_tick_conversion.cpp
    atomic_park.cpp
    embedded_modal_close_safety.cpp
    x11_anti_pattern_guard.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/BrickwallLimiter.cpp)

target_include_directories(focal-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/projucer-shim)

target_compile_definitions(focal-tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    FOCAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(focal-tests PRIVATE
    Catch2::Catch2WithMain
    juce::juce_audio_basics
    juce::juce_core
    juce::juce_graphics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(focal-tests)
