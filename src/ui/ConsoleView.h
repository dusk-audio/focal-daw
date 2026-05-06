#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "AuxBusComponent.h"
#include "ChannelStripComponent.h"
#include "MasterStripComponent.h"
#include "../session/Session.h"
#include "../engine/AudioEngine.h"

namespace focal
{
class ConsoleView final : public juce::Component
{
public:
    // Takes both Session (data model) and AudioEngine (live DSP). The
    // engine is needed so each ChannelStripComponent can be handed a
    // reference to its PluginSlot - the UI calls slot.loadFromFile etc.
    // on the message thread.
    ConsoleView (Session& session, AudioEngine& engine);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kMinChannelWidth = 120;
    static constexpr int kMinAuxWidth     = 130;   // 3 × 40 px knob blocks + 10 px padding
    static constexpr int kMinMasterWidth  = 210;   // 5 × 40 px knob blocks + 10 px padding

    static constexpr int kRefChannelWidth = 150;
    static constexpr int kRefAuxWidth     = 150;   // comfortable padding around the 3-knob row
    static constexpr int kRefMasterWidth  = 230;   // 5 EQ knobs + 5 comp knobs at 40 px each

    static constexpr int kStripGap     = 4;
    static constexpr int kSectionGap   = 12;

    static int minimumContentWidth();

    // Width threshold above which we drop banking and show all 16 strips.
    // Public so MainComponent (which now owns the BANK A/B row) can decide
    // whether to render the bank-row above the transport.
    int fixedWidthFor16Tracks() const;

    void setBank (int bankIndex);
    int  getBank() const noexcept { return currentBank; }

private:
    Session& sessionRef;

    std::array<std::unique_ptr<ChannelStripComponent>, Session::kNumTracks> strips;

public:
    // Forwarded by MainComponent when the SUMMARY view toggles. Each track
    // strip collapses its EQ + COMP into popup-launch buttons so the fader,
    // bus assigns, and meters stay visible while the tape strip is up.
    void setStripsCompactMode (bool compact);

    // Forwarded by MainComponent when the stage selector changes. In Mixing
    // each strip swaps its input/IN/ARM/PRINT block for a row of 4 AUX send
    // knobs (the tracking controls only matter while recording).
    void setStripsMixingMode (bool mixing);

private:
    std::array<std::unique_ptr<AuxBusComponent>,       Session::kNumAuxBuses> auxStrips;
    std::unique_ptr<MasterStripComponent> masterStrip;

    int currentBank = 0;
    bool showingAllTracks = false;

    void updateBankVisibility();
};
} // namespace focal
