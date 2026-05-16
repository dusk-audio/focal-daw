#include "MidiBindings.h"
#include "Session.h"
#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include <algorithm>

namespace focal
{
juce::String describeBindingSource (const MidiBinding& b)
{
    const auto chStr = b.channel == 0 ? juce::String ("Ch -")
                                       : "Ch " + juce::String (b.channel);
    const auto kindStr = b.trigger == MidiBindingTrigger::CC
                            ? juce::String ("CC ") : juce::String ("Note ");
    return chStr + " " + kindStr + juce::String (b.dataNumber);
}

juce::String describeBindingTarget (const MidiBinding& b,
                                     const AudioEngine* engine)
{
    // Centralised so the menu, the readout, and the bindings panel
    // all render the same label for any binding.
    auto trk = [&] { return juce::String (b.targetIndex + 1); };
    switch (b.target)
    {
        case MidiBindingTarget::None:            return "(unbound)";
        case MidiBindingTarget::TransportPlay:   return "Transport: Play";
        case MidiBindingTarget::TransportStop:   return "Transport: Stop";
        case MidiBindingTarget::TransportRecord: return "Transport: Record";
        case MidiBindingTarget::TransportToggle: return "Transport: Play/Stop";
        case MidiBindingTarget::TrackFader:      return "Track " + trk() + " fader";
        case MidiBindingTarget::TrackPan:        return "Track " + trk() + " pan";
        case MidiBindingTarget::TrackMute:       return "Track " + trk() + " mute";
        case MidiBindingTarget::TrackSolo:       return "Track " + trk() + " solo";
        case MidiBindingTarget::TrackArm:        return "Track " + trk() + " arm";
        case MidiBindingTarget::TrackAuxSend:
        {
            const int track = unpackTrackAuxTrack (b.targetIndex);
            const int aux   = unpackTrackAuxLane  (b.targetIndex);
            return "Track " + juce::String (track + 1)
                 + " AUX " + juce::String (aux + 1) + " send";
        }
        case MidiBindingTarget::TrackHpfFreq:    return "Track " + trk() + " HPF";
        case MidiBindingTarget::TrackEqGain:
        {
            const int track = unpackTrackEqTrack (b.targetIndex);
            const int band  = unpackTrackEqBand  (b.targetIndex);
            static const char* kBandNames[] = { "LF", "LM", "HM", "HF" };
            const char* bandName = (band >= 0 && band < 4) ? kBandNames[band] : "?";
            return "Track " + juce::String (track + 1)
                 + " EQ " + juce::String (bandName) + " gain";
        }
        case MidiBindingTarget::TrackCompThresh: return "Track " + trk() + " comp threshold";
        case MidiBindingTarget::TrackCompMakeup: return "Track " + trk() + " comp makeup";
        case MidiBindingTarget::TrackPluginParam:
        {
            // Try to resolve the parameter's name via the engine. If
            // no plugin is loaded or paramIndex is out of range, fall
            // back to the index so the user still sees something
            // identifiable (and can right-click → Remove it).
            juce::String paramName;
            if (engine != nullptr
                && b.targetIndex >= 0
                && b.targetIndex < Session::kNumTracks)
            {
                const auto& slot = engine->getChannelStrip (b.targetIndex)
                                          .getPluginSlot();
                if (auto* instance = slot.getInstance())
                {
                    const auto& params = instance->getParameters();
                    if (b.paramIndex >= 0 && b.paramIndex < params.size())
                        if (auto* p = params[b.paramIndex])
                            paramName = p->getName (32);
                }
            }
            if (paramName.isEmpty())
                paramName = "param " + juce::String (b.paramIndex);
            return "Track " + trk() + " " + paramName;
        }
        case MidiBindingTarget::BusFader:        return "Bus " + trk() + " fader";
        case MidiBindingTarget::BusPan:          return "Bus " + trk() + " pan";
        case MidiBindingTarget::BusMute:         return "Bus " + trk() + " mute";
        case MidiBindingTarget::BusSolo:         return "Bus " + trk() + " solo";
        case MidiBindingTarget::AuxLaneFader:    return "AUX " + trk() + " return";
        case MidiBindingTarget::AuxLaneMute:     return "AUX " + trk() + " mute";
        case MidiBindingTarget::MasterFader:     return "Master fader";
    }
    return "(unknown target)";
}

const char* nameForTarget (MidiBindingTarget t) noexcept
{
    switch (t)
    {
        case MidiBindingTarget::None:            return "(none)";
        case MidiBindingTarget::TransportPlay:   return "Play";
        case MidiBindingTarget::TransportStop:   return "Stop";
        case MidiBindingTarget::TransportRecord: return "Record";
        case MidiBindingTarget::TransportToggle: return "Play/Stop toggle";
        case MidiBindingTarget::TrackFader:      return "Track fader";
        case MidiBindingTarget::TrackPan:        return "Track pan";
        case MidiBindingTarget::TrackMute:       return "Track mute";
        case MidiBindingTarget::TrackSolo:       return "Track solo";
        case MidiBindingTarget::TrackArm:        return "Track arm";
        case MidiBindingTarget::TrackAuxSend:    return "AUX send";
        case MidiBindingTarget::TrackHpfFreq:    return "HPF cutoff";
        case MidiBindingTarget::TrackEqGain:     return "EQ band gain";
        case MidiBindingTarget::TrackCompThresh: return "Comp threshold";
        case MidiBindingTarget::TrackCompMakeup: return "Comp makeup";
        case MidiBindingTarget::TrackPluginParam: return "Plugin parameter";
        case MidiBindingTarget::BusFader:        return "Bus fader";
        case MidiBindingTarget::BusPan:          return "Bus pan";
        case MidiBindingTarget::BusMute:         return "Bus mute";
        case MidiBindingTarget::BusSolo:         return "Bus solo";
        case MidiBindingTarget::AuxLaneFader:    return "AUX return";
        case MidiBindingTarget::AuxLaneMute:     return "AUX mute";
        case MidiBindingTarget::MasterFader:     return "Master fader";
    }
    return "?";
}

namespace midilearn
{
namespace
{
juce::String describeBinding (const MidiBinding& b)
{
    auto chStr = b.channel == 0 ? juce::String ("Ch -")
                                : "Ch " + juce::String (b.channel);
    auto kindStr = b.trigger == MidiBindingTrigger::CC ? "CC " : "Note ";
    return chStr + " " + kindStr + juce::String (b.dataNumber);
}
} // namespace

void showLearnMenu (juce::Component& target,
                    Session& session,
                    MidiBindingTarget kind,
                    int index)
{
    // Find an existing binding for this (kind, index) pair so the menu
    // can show its source and offer "Forget".
    const MidiBinding* existing = nullptr;
    for (const auto& b : session.midiBindings.current())
    {
        if (b.target == kind && b.targetIndex == index) { existing = &b; break; }
    }

    juce::PopupMenu m;
    m.addSectionHeader (nameForTarget (kind));
    m.addItem ("MIDI Learn...", true, false,
        [&session, kind, index]
        {
            session.midiLearnPending.store (packLearnTarget (kind, index),
                                              std::memory_order_relaxed);
            session.midiLearnCapture.store (0, std::memory_order_relaxed);
        });
    if (existing != nullptr)
    {
        const auto label = "Bound: " + describeBinding (*existing);
        m.addItem (label, false, false, []{});
        m.addItem ("Forget binding", true, false,
            [&session, kind, index]
            {
                session.midiBindings.mutate ([kind, index] (std::vector<MidiBinding>& binds)
                {
                    binds.erase (std::remove_if (binds.begin(), binds.end(),
                        [kind, index] (const MidiBinding& x)
                        {
                            return x.target == kind && x.targetIndex == index;
                        }), binds.end());
                });
            });
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}
} // namespace midilearn
} // namespace focal
