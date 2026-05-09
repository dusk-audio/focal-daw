#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace focal
{
class PluginManager;
class PluginSlot;

// Shared plugin-picker UX used by per-channel insert slots and per-aux send-FX
// slots. The picker, scan, and file-chooser flows are identical between the
// two surfaces; the only differences are which PluginSlot they target and
// what to refresh on success. Keeping the implementation here avoids two
// slowly-diverging copies of the same menu-building / load-from-description
// code.
namespace pluginpicker
{
// Filter applied to the installed-plugin list before the picker is
// shown. Effect slots (channel strip in Mono/Stereo mode, aux send-FX
// slots) want Effects only - instrument plugins need MIDI input to
// produce sound and render as no-op or unstable when loaded as audio
// inserts. Instrument slots (channel strip in MIDI mode) want
// Instruments only.
enum class PluginKind { Effects, Instruments };

// Open a popup menu of installed plugins anchored on `target`. On selection:
//   • 1..N → resolves to KnownPluginList types and loadFromDescription.
//   • "Scan plugins" → runs the synchronous scan + reopens the picker.
//   • "Browse for file..." → launches a juce::FileChooser owned by
//     `chooserOwner` (kept alive across the async callback).
// `onChange` runs on every successful change to the slot.
//
// `kind` filters the visible list: Effects hides instruments,
// Instruments hides effects. The Browse-for-file path is unfiltered -
// if the user explicitly browses to a file we trust their choice.
//
// `screenPosition` overrides the menu anchor. Pass the cursor's screen
// position when `target` is a large click-target (full-slot placeholder)
// so the menu appears at the click rather than at the component's
// top-left. Default {-1,-1} means "use target component bounds".
void openPickerMenu (PluginSlot& slot,
                      juce::Component& target,
                      std::unique_ptr<juce::FileChooser>& chooserOwner,
                      std::function<void()> onChange,
                      PluginKind kind,
                      juce::Point<int> screenPosition = { -1, -1 });

// Synchronous scan with a tiny modal banner so the user sees progress.
// Same UX as the per-channel scan dialog. Safe to call from message thread.
void runScanModal (PluginManager& manager);

// Run a juce::FileChooser to pick a plugin file (.vst3 / .so / .lv2),
// loading on selection. `chooserOwner` keeps the chooser alive across the
// async callback. `onChange` runs on successful load.
void openFileChooser (PluginSlot& slot,
                       std::unique_ptr<juce::FileChooser>& chooserOwner,
                       std::function<void()> onChange);
}
} // namespace focal
