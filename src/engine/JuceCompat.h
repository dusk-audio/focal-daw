#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace focal::juce_compat
{
// One-call shim hiding the AudioPluginFormatManager::addDefaultFormats vs
// the wayland fork's free juce::addDefaultFormatsToManager() split. Call
// this from anywhere - no #if defined(__linux__) at the call site.
//
// Why the fork diverges: plugdata-team's JUCE-wayland (pinned for Focal's
// Linux build) made the member `= delete` and provided a free function so
// the same surface area can be shared across plugin-host targets without
// pulling the host module's full deps into every consumer. Upstream JUCE
// on macOS / Windows still has the member.
inline void addDefaultFormats (juce::AudioPluginFormatManager& fm)
{
   #if defined(__linux__)
    juce::addDefaultFormatsToManager (fm);
   #else
    fm.addDefaultFormats();
   #endif
}
} // namespace focal::juce_compat
