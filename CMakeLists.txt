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

# Dusk Audio plugins repo — donor for the analog DSP cores (4K EQ filter chain,
# FET/Opto compressor, Pultec EQ, tape saturation, console saturation, plus the
# AnalogEmulation library). Resolved via -DDUSK_PLUGINS_PATH > sibling
# ../plugins. Header-only cores are picked up via the include paths below;
# editing a header in the plugins repo automatically rebuilds the affected
# ADH DAW translation units. No copy / submodule bump required.
if(DEFINED DUSK_PLUGINS_PATH)
    message(STATUS "Using Dusk plugins from: ${DUSK_PLUGINS_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/plugins/shared")
    set(DUSK_PLUGINS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../plugins")
    message(STATUS "Using Dusk plugins from sibling directory: ${DUSK_PLUGINS_PATH}")
else()
    message(WARNING
        "Dusk plugins repo not found. ADH DAW will build but DSP cores will be "
        "unavailable until you set -DDUSK_PLUGINS_PATH=/path/to/plugins or clone "
        "the plugins repo to ../plugins (sibling directory).")
endif()

target_sources(ADHDaw PRIVATE
    src/Main.cpp
    src/ADHDawApp.cpp
    src/engine/AudioEngine.cpp
    src/engine/PlaybackEngine.cpp
    src/engine/RecordManager.cpp
    src/dsp/AuxBusStrip.cpp
    src/dsp/ChannelStrip.cpp
    src/dsp/MasterBus.cpp
    src/session/Session.cpp
    src/session/SessionSerializer.cpp
    src/ui/AuxBusComponent.cpp
    src/ui/ChannelStripComponent.cpp
    src/ui/CompMeterStrip.cpp
    src/ui/ConsoleView.cpp
    src/ui/MainComponent.cpp
    src/ui/MasterStripComponent.cpp
    src/ui/SystemStatusBar.cpp
    src/ui/TapeStrip.cpp
    src/ui/TransportBar.cpp)

target_include_directories(ADHDaw PRIVATE
    src
    external/projucer-shim)  # JuceHeader.h shim used by vendored plugin DSP headers

if(DEFINED DUSK_PLUGINS_PATH)
    # Per-plugin DSP headers from the Dusk plugins repo's main branch. Each
    # plugin owns its DSP; ADHDaw vendors:
    #   - multi-q    (BritishEQProcessor — per-channel 4-band British EQ)
    #   - multi-comp (UniversalCompressor — per-channel Opto/FET/VCA comp,
    #                 used here as a juce::AudioProcessor; only its DSP path is
    #                 active — the plugin editor is compiled-in but never
    #                 instantiated by ADHDaw's UI)
    #   - tape-echo  (TapeEchoDSP::TapeSaturation — master bus tape sat)
    target_include_directories(ADHDaw PRIVATE
        "${DUSK_PLUGINS_PATH}/plugins/shared"
        "${DUSK_PLUGINS_PATH}/plugins/shared/AnalogEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/multi-q"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/HardwareEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/tape-echo/Source/DSP")

    # Compile multi-comp's processor + editor + shared LEDMeter so
    # UniversalCompressor links cleanly. Editor sources are linked but not
    # invoked from ADHDaw's UI — they exist only because UniversalCompressor's
    # createEditor() references them.
    target_sources(ADHDaw PRIVATE
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/AnalogLookAndFeel.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/EnhancedCompressorEditor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/shared/LEDMeter.cpp")

    target_compile_definitions(ADHDaw PRIVATE ADHDAW_HAS_DUSK_DSP=1)
endif()

target_compile_definitions(ADHDaw PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ADHDaw,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ADHDaw,JUCE_VERSION>"
    JUCE_JACK=1
    JUCE_ALSA=1
    # Projucer-style macros that vendored multi-comp sources expect.
    JucePlugin_VersionString="${PROJECT_VERSION}"
    JucePlugin_Name="ADHDaw_Comp"
    JucePlugin_Manufacturer="Marc Korte")

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
