#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "AuxBusComponent.h"
#include "ChannelStripComponent.h"
#include "MasterStripComponent.h"
#include "../session/Session.h"

namespace adhdaw
{
class ConsoleView final : public juce::Component
{
public:
    explicit ConsoleView (Session& session);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kMinChannelWidth = 120;   // was 100 — give knobs / value labels real room
    static constexpr int kMinAuxWidth     = 70;
    static constexpr int kMinMasterWidth  = 100;

    static constexpr int kRefChannelWidth = 150;   // was 116 — banked-8 default is now spacious
    static constexpr int kRefAuxWidth     = 90;
    static constexpr int kRefMasterWidth  = 120;

    static constexpr int kStripGap     = 4;
    static constexpr int kSectionGap   = 12;
    static constexpr int kBankBarHeight = 28;

    static int minimumContentWidth();

    void setBank (int bankIndex);
    int  getBank() const noexcept { return currentBank; }

private:
    Session& sessionRef;

    std::array<std::unique_ptr<ChannelStripComponent>, Session::kNumTracks> strips;
    std::array<std::unique_ptr<AuxBusComponent>,       Session::kNumAuxBuses> auxStrips;
    std::unique_ptr<MasterStripComponent> masterStrip;

    juce::TextButton bankAButton { "BANK A  (1-8)" };
    juce::TextButton bankBButton { "BANK B  (9-16)" };

    int currentBank = 0;
    bool showingAllTracks = false;

    void updateBankVisibility();
    int  fixedWidthFor16Tracks() const;
};
} // namespace adhdaw
