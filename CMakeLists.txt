cmake_minimum_required(VERSION 3.22)
project(ADHDaw VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# JUCE path resolution: -DJUCE_PATH > sibling ../JUCE > external/JUCE submodule.
# Same priority order as the Dusk Audio plugins repo so a developer with both
# repos checked out only needs JUCE in one place.
if(DEFINED JUCE_PATH)
    message(STATUS "Using JUCE from: ${JUCE_PATH}")
    add_subdirectory(${JUCE_PATH} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    message(STATUS "Using JUCE from sibling directory: ${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE ${CMAKE_CURRENT_BINARY_DIR}/JUCE)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    message(STATUS "Using JUCE from submodule: external/JUCE")
    add_subdirectory(external/JUCE)
else()
    message(FATAL_ERROR
        "JUCE not found. Options:\n"
        "  1. Set -DJUCE_PATH=/path/to/JUCE\n"
        "  2. Clone JUCE to ../JUCE (sibling directory)\n"
        "  3. Add as submodule: git submodule add https://github.com/juce-framework/JUCE.git external/JUCE")
endif()

juce_add_gui_app(ADHDaw
    PRODUCT_NAME       "ADH DAW"
    COMPANY_NAME       "Marc Korte"
    BUNDLE_ID          "com.marckorte.adhdaw"
    VERSION            ${PROJECT_VERSION})

target_sources(ADHDaw PRIVATE
    src/Main.cpp
    src/ADHDawApp.cpp
    src/engine/AudioEngine.cpp
    src/ui/MainComponent.cpp)

target_include_directories(ADHDaw PRIVATE src)

target_compile_definitions(ADHDaw PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ADHDaw,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ADHDaw,JUCE_VERSION>"
    JUCE_JACK=1
    JUCE_ALSA=1)

target_link_libraries(ADHDaw PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
