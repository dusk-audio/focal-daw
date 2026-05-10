cmake_minimum_required(VERSION 3.22)
project(Focal VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# JUCE path resolution: -DJUCE_PATH > sibling ../JUCE > external/JUCE submodule.
# Same priority order as the Dusk Audio plugins repo so a developer with both
# repos checked out only needs JUCE in one place.
if(DEFINED JUCE_PATH)
    set(FOCAL_JUCE_SOURCE_DIR "${JUCE_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    set(FOCAL_JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    set(FOCAL_JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE")
else()
    message(FATAL_ERROR
        "JUCE not found. Options:\n"
        "  1. Set -DJUCE_PATH=/path/to/JUCE\n"
        "  2. Clone JUCE to ../JUCE (sibling directory)\n"
        "  3. Add as submodule: git submodule add https://github.com/juce-framework/JUCE.git external/JUCE")
endif()

message(STATUS "Using JUCE from: ${FOCAL_JUCE_SOURCE_DIR}")

# Linux-only: verify our local XEmbed mapping fix is present in the JUCE checkout.
# Stock JUCE 8.0.10 ties the host container's X11 mapping to the embedded
# plugin's _XEMBED_INFO flag, which leaves pure-xcb VST3s (e.g. MininnDrum) as
# blank rectangles. The fix splits WindowMapper by Role; the marker below is in
# our local commit. Detect drift if JUCE is ever updated without re-applying.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_focal_xembed_file
        "${FOCAL_JUCE_SOURCE_DIR}/modules/juce_gui_extra/native/juce_XEmbedComponent_linux.cpp")
    if(NOT EXISTS "${_focal_xembed_file}")
        message(FATAL_ERROR "Expected JUCE source not found: ${_focal_xembed_file}")
    endif()
    file(READ "${_focal_xembed_file}" _focal_xembed_src)
    if(NOT _focal_xembed_src MATCHES "enum class Role { host, client }")
        message(FATAL_ERROR
            "Focal requires a patched JUCE: the XEmbed host-mapping fix is missing in\n"
            "  ${_focal_xembed_file}\n"
            "Without it, pure-xcb VST3 plugins (e.g. MininnDrum) render as a blank\n"
            "rectangle. Re-apply the local commit titled\n"
            "  \"XEmbed: stop tying host-window mapping to client _XEMBED_INFO\"\n"
            "in your JUCE checkout, or pull a JUCE branch that contains it.")
    endif()
    unset(_focal_xembed_file)
    unset(_focal_xembed_src)
endif()

add_subdirectory(${FOCAL_JUCE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

# App icon. JUCE bakes the PNG into platform-specific resources at
# build time (.ico on Windows, .icns on macOS, embedded data block on
# Linux). Same file is used for both BIG and SMALL - JUCE downscales
# internally as needed. The conditional keeps a fresh checkout
# building when assets/icon.png hasn't been added yet.
set(FOCAL_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png")
if(EXISTS "${FOCAL_ICON_PATH}")
    set(FOCAL_ICON_ARGS
        ICON_BIG   "${FOCAL_ICON_PATH}"
        ICON_SMALL "${FOCAL_ICON_PATH}")
    message(STATUS "Focal app icon: ${FOCAL_ICON_PATH}")
else()
    set(FOCAL_ICON_ARGS "")
    message(STATUS "Focal app icon not found at ${FOCAL_ICON_PATH} - building without one")
endif()

juce_add_gui_app(Focal
    PRODUCT_NAME       "Focal"
    COMPANY_NAME       "Marc Korte"
    BUNDLE_ID          "com.marckorte.focal"
    VERSION            ${PROJECT_VERSION}
    ${FOCAL_ICON_ARGS}
    NEEDS_WAYLAND      TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Focal needs microphone access to record audio from your inputs.")

# Dusk Audio plugins repo — donor for the analog DSP cores (4K EQ filter chain,
# FET/Opto compressor, Pultec EQ, tape saturation, console saturation, plus the
# AnalogEmulation library). Resolved via -DDUSK_PLUGINS_PATH > sibling
# ../plugins. Header-only cores are picked up via the include paths below;
# editing a header in the plugins repo automatically rebuilds the affected
# Focal translation units. No copy / submodule bump required.
if(DEFINED DUSK_PLUGINS_PATH)
    message(STATUS "Using Dusk plugins from: ${DUSK_PLUGINS_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/plugins/shared")
    set(DUSK_PLUGINS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../plugins")
    message(STATUS "Using Dusk plugins from sibling directory: ${DUSK_PLUGINS_PATH}")
else()
    message(WARNING
        "Dusk plugins repo not found. Focal will build but DSP cores will be "
        "unavailable until you set -DDUSK_PLUGINS_PATH=/path/to/plugins or clone "
        "the plugins repo to ../plugins (sibling directory).")
endif()

target_sources(Focal PRIVATE
    src/Main.cpp
    src/FocalApp.cpp
    src/engine/AudioEngine.cpp
    src/engine/BounceEngine.cpp
    src/engine/MasteringPlayer.cpp
    src/engine/PlaybackEngine.cpp
    src/engine/PluginManager.cpp
    src/engine/PluginSlot.cpp
    src/engine/RecordManager.cpp
    src/dsp/AuxLaneStrip.cpp
    src/dsp/BusStrip.cpp
    src/dsp/BrickwallLimiter.cpp
    src/dsp/ChannelStrip.cpp
    src/dsp/LoudnessMeter.cpp
    src/dsp/MasterBus.cpp
    src/dsp/MasteringChain.cpp
    src/dsp/MasteringDigitalEq.cpp
    src/dsp/Metronome.cpp
    src/session/MarkerEditActions.cpp
    src/session/RecentSessions.cpp
    src/session/RegionEditActions.cpp
    src/dsp/ChordAnalyzer.cpp
    src/session/MidiBindings.cpp
    src/session/Session.cpp
    src/session/SessionSerializer.cpp
    src/session/SessionTemplates.cpp
    src/ui/AnalogVuMeter.cpp
    src/ui/AppConfig.cpp
    src/ui/AudioRegionEditor.cpp
    src/ui/AudioSettingsPanel.cpp
    src/ui/AuxLaneComponent.cpp
    src/ui/AuxView.cpp
    src/ui/BusComponent.cpp
    src/ui/BounceDialog.cpp
    src/ui/ChannelCompEditor.cpp
    src/ui/ChannelEqEditor.cpp
    src/ui/ChannelStripComponent.cpp
    src/ui/CompMeterStrip.cpp
    src/ui/SelfTestPanel.cpp
    src/ui/StartupDialog.cpp
    src/engine/AudioPipelineSelfTest.cpp
    src/ui/ConsoleView.cpp
    src/ui/DimOverlay.cpp
    src/ui/MainComponent.cpp
    src/ui/MasteringEqEditor.cpp
    src/ui/MasteringLimiterEditor.cpp
    src/ui/MasteringView.cpp
    src/ui/MasterStripComponent.cpp
    src/ui/PianoRollComponent.cpp
    src/ui/PluginPickerHelpers.cpp
    src/ui/SystemStatusBar.cpp
    src/ui/TapeStrip.cpp
    src/ui/TransportBar.cpp
    src/ui/VirtualKeyboardComponent.cpp
    src/ui/WindowState.cpp)

# Cross-platform window-management primitives. One implementation per
# platform; callsites #include only PlatformWindowing.h and stay
# platform-agnostic. Mac and Windows impls are stubs today (see
# PlatformWindowing.h for the eventual per-platform mechanisms) but
# the files exist so the fix slot is obvious when the equivalent bugs
# surface there.
if(APPLE)
    target_sources(Focal PRIVATE src/ui/PlatformWindowing_Mac.mm)
elseif(WIN32)
    target_sources(Focal PRIVATE src/ui/PlatformWindowing_Windows.cpp)
else()
    target_sources(Focal PRIVATE src/ui/PlatformWindowing_Linux.cpp)
endif()

# Focal-owned ALSA backend - a juce::AudioIODevice + AudioIODeviceType pair
# written from scratch using Ardour's libs/backends/alsa/ as a reference for
# patterns (xrun recovery, sw_params choices, format priority, channel-mask
# handling) but with no copied code. Replaces JUCE's stock ALSA backend,
# which is structurally insufficient for hardware-direct studio use across
# the long tail of audio interfaces.
#
# Linux-only: the implementation depends on <alsa/asoundlib.h>. macOS and
# Windows builds skip it and fall back to the audio backends JUCE provides
# (CoreAudio / WASAPI). The AudioEngine constructor's registration is
# similarly guarded by #if defined(__linux__).
if(UNIX AND NOT APPLE)
    target_sources(Focal PRIVATE
        src/engine/alsa/AlsaAudioIODevice.cpp
        src/engine/alsa/AlsaAudioIODeviceType.cpp
        src/engine/alsa/AlsaPerformanceTest.cpp)
    target_link_libraries(Focal PRIVATE asound X11)
    target_compile_definitions(Focal PRIVATE FOCAL_HAS_FOCAL_ALSA=1)

    # Out-of-process plugin hosting: shared IPC primitives + the
    # parent-side connection class + a self-test driver. The
    # focal-plugin-host child binary lives next to Focal in the build
    # output. Linux-only; the IPC uses memfd_create + futex which are
    # Linux-specific. Mac/Windows builds keep using the in-process path
    # (PluginSlot) until the OOP work matures into a portable design.
    # FOCAL_HAS_OOP_PLUGINS gates every #if branch in PluginSlot that
    # routes through RemotePluginConnection; defined only here so the
    # rest of the codebase can rely on a single feature flag.
    target_sources(Focal PRIVATE
        src/engine/ipc/RemotePluginConnection.cpp
        src/engine/ipc/IpcSelfTest.cpp)
    target_compile_definitions(Focal PRIVATE FOCAL_HAS_OOP_PLUGINS=1)

    # focal-plugin-host is a JUCE console app so it can pull in the
    # AudioPluginFormatManager + AudioPluginInstance machinery and
    # (in Phase 3) the gui_extra modules for plugin-editor embedding.
    # juce_add_console_app gives us juce::JUCEApplicationBase plumbing
    # without forcing a UI; we still set NEEDS_*_GRAPHICS / GUI for the
    # editor module to compile. The host's main() runs JUCE's message
    # loop on the main thread + a worker juce::Thread for the audio path.
    juce_add_console_app(focal-plugin-host
        PRODUCT_NAME "focal-plugin-host"
        COMPANY_NAME "Marc Korte")
    target_sources(focal-plugin-host PRIVATE
        src/engine/ipc/PluginHostMain.cpp)
    target_compile_features (focal-plugin-host PRIVATE cxx_std_17)
    target_compile_definitions (focal-plugin-host PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_LV2=1
        JUCE_PLUGINHOST_AU=0
        # Stop juce_audio_processors from generating JuceHeader.h-style
        # stubs and Projucer-only metadata - we drive everything from
        # CMake.
        JUCE_DONT_DECLARE_PROJECTINFO=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(focal-plugin-host PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_events
        juce::juce_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    target_include_directories(focal-plugin-host PRIVATE src)
    set_target_properties(focal-plugin-host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:Focal>")
    add_dependencies(Focal focal-plugin-host)
endif()

target_include_directories(Focal PRIVATE
    src
    external/projucer-shim)  # JuceHeader.h shim used by vendored plugin DSP headers

if(DEFINED DUSK_PLUGINS_PATH)
    # Per-plugin DSP headers from the Dusk plugins repo's main branch. Each
    # plugin owns its DSP; Focal vendors:
    #   - multi-q    (BritishEQProcessor — per-channel 4-band British EQ)
    #   - multi-comp (UniversalCompressor — per-channel Opto/FET/VCA comp,
    #                 used here as a juce::AudioProcessor; only its DSP path is
    #                 active — the plugin editor is compiled-in but never
    #                 instantiated by Focal's UI)
    #   - tape-echo    (TapeEchoDSP::TapeSaturation — legacy master bus tape sat,
    #                   being replaced by TapeMachine; still vendored while the
    #                   migration completes)
    #   - TapeMachine  (full-featured tape emulation hosted on the master bus;
    #                   editor opened via the gear button next to TAPE)
    target_include_directories(Focal PRIVATE
        "${DUSK_PLUGINS_PATH}/plugins/shared"
        "${DUSK_PLUGINS_PATH}/plugins/shared/AnalogEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/shared/channel-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-q"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/HardwareEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/tape-echo/Source/DSP"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI")

    # Compile multi-comp's processor + editor + shared LEDMeter so
    # UniversalCompressor links cleanly. Editor sources are linked but not
    # invoked from Focal's UI — they exist only because UniversalCompressor's
    # createEditor() references them.
    target_sources(Focal PRIVATE
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/AnalogLookAndFeel.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/EnhancedCompressorEditor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/shared/LEDMeter.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginProcessor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginEditor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/ImprovedTapeEmulation.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/AnalogVUMeter.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/TapeReelComponent.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/TapeMachineLookAndFeel.cpp")

    # Both multi-comp's editor and TapeMachine's editor declare a class named
    # AnalogVUMeter in the global namespace, which collides at link time.
    # Rename the multi-comp one in-translation-unit so the donors stay
    # unmodified. Applied to every multi-comp .cpp that pulls in
    # AnalogLookAndFeel.h (directly or via EnhancedCompressorEditor.h).
    set_source_files_properties(
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/AnalogLookAndFeel.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/EnhancedCompressorEditor.cpp"
        PROPERTIES
        COMPILE_DEFINITIONS "AnalogVUMeter=MultiCompAnalogVUMeter")

    # Both donors define createPluginFilter() as their JUCE plugin entry
    # point. Focal instantiates them directly so neither is called, but the
    # global symbol still collides at link. Rename the TapeMachine one.
    set_source_files_properties(
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginProcessor.cpp"
        PROPERTIES
        COMPILE_DEFINITIONS "createPluginFilter=createTapeMachinePluginFilter")

    target_compile_definitions(Focal PRIVATE FOCAL_HAS_DUSK_DSP=1)
endif()

target_compile_definitions(Focal PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Focal,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Focal,JUCE_VERSION>"
    JUCE_JACK=1
    JUCE_ALSA=1
    # Native Wayland windowing on Linux (plugdata fork). Bypasses XWayland
    # and the meta_window_unmanage assertion that crashes mutter when an
    # X11 toplevel is destroyed while focused.
    $<$<PLATFORM_ID:Linux>:JUCE_WAYLAND=1>
    # Plugin host formats. Default JUCE setting is OFF; turn on the two
    # formats we actually scan + load (VST3 and LV2 - both popular on Linux
    # via yabridge / native ports). Without these, addDefaultFormats()
    # registers nothing and every plugin load fails with "No compatible
    # plug-in format exists for this plug-in".
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    # AU is the dominant native plugin format on macOS; without this
    # define the format is silently absent from addDefaultFormats().
    JUCE_PLUGINHOST_AU=$<IF:$<PLATFORM_ID:Darwin>,1,0>
    # Projucer-style macros that vendored plugin sources expect. multi-comp
    # uses VersionString / Name / Manufacturer; TapeMachine additionally
    # references the IsMidiEffect / IsSynth / WantsMidiInput / ProducesMidiOutput
    # toggles from inside #if blocks, so we provide defaults (all 0) globally
    # rather than per-source.
    JucePlugin_VersionString="${PROJECT_VERSION}"
    JucePlugin_Name="Focal_Comp"
    JucePlugin_Manufacturer="Marc Korte"
    JucePlugin_IsMidiEffect=0
    JucePlugin_IsSynth=0
    JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0)

if((CMAKE_SYSTEM_NAME STREQUAL "Linux") AND TARGET juce::pkgconfig_JUCE_WAYLAND_LINUX_DEPS)
    # Pulls in wayland-client / xkbcommon / libdecor-0 include paths.
    # Linkage is dlopen at runtime (NOLINK in the JUCE pkg-config target),
    # so this is purely for the headers exposed via juce_gui_basics.h
    # under JUCE_WAYLAND.
    target_link_libraries(Focal PRIVATE juce::pkgconfig_JUCE_WAYLAND_LINUX_DEPS)
endif()

target_link_libraries(Focal PRIVATE
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

option(FOCAL_BUILD_TESTS "Build Focal unit tests with Catch2" OFF)
if(FOCAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
