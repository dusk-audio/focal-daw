#include "MidiBindings.h"
#include "Session.h"
#include <algorithm>

namespace focal
{
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
