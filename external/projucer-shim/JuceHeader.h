// Projucer-style umbrella header shim for ADHDaw's CMake build.
//
// The Dusk plugins repo (Projucer-built) auto-generates JuceHeader.h with all
// the JUCE module includes. ADHDaw uses CMake/juce_add_gui_app, which doesn't
// produce that file. When we vendor plugin DSP headers (BritishEQProcessor,
// UniversalCompressor, TapeSaturation, etc.) we include this shim on the
// compiler include path so those headers find their `<JuceHeader.h>` include.
//
// We intentionally do NOT inject `using namespace juce;` here even though
// the Projucer template does — ADHDaw code is namespace-qualified and we
// don't want global `juce::` to spill into our translation units.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
