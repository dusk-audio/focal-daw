#include "ConsoleView.h"

namespace adhdaw
{
ConsoleView::ConsoleView (Session& session) : sessionRef (session)
{
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        strips[(size_t) i] = std::make_unique<ChannelStripComponent> (i, session.track (i), session);
        addAndMakeVisible (strips[(size_t) i].get());
    }
    for (int i = 0; i < Session::kNumAuxBuses; ++i)
    {
        auxStrips[(size_t) i] = std::make_unique<AuxBusComponent> (session.aux (i));
        addAndMakeVisible (auxStrips[(size_t) i].get());
    }
    masterStrip = std::make_unique<MasterStripComponent> (session.master());
    addAndMakeVisible (masterStrip.get());

    auto styleBankButton = [] (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (1, juce::dontSendNotification);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd0a060));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colours::lightgrey);
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    };
    styleBankButton (bankAButton);
    styleBankButton (bankBButton);
    bankAButton.setToggleState (true, juce::dontSendNotification);
    bankAButton.onClick = [this] { setBank (0); };
    bankBButton.onClick = [this] { setBank (1); };
    addAndMakeVisible (bankAButton);
    addAndMakeVisible (bankBButton);

    updateBankVisibility();
}

int ConsoleView::minimumContentWidth()
{
    const int gaps = (Session::kBankSize - 1) * kStripGap         // gaps between channels
                   + kSectionGap                                  // channel block -> aux block
                   + (Session::kNumAuxBuses - 1) * kStripGap      // gaps between aux strips
                   + kSectionGap;                                 // aux block -> master
    return Session::kBankSize * kMinChannelWidth
         + Session::kNumAuxBuses * kMinAuxWidth
         + kMinMasterWidth
         + gaps
         + 12;  // outer padding
}

int ConsoleView::fixedWidthFor16Tracks() const
{
    // Width required to lay out all 16 channels + 4 buses + master at their
    // *reference* widths — this is the threshold above which we drop the
    // banking and show every track at once.
    const int gaps = (Session::kNumTracks - 1) * kStripGap
                   + kSectionGap
                   + (Session::kNumAuxBuses - 1) * kStripGap
                   + kSectionGap;
    return Session::kNumTracks * kRefChannelWidth
         + Session::kNumAuxBuses * kRefAuxWidth
         + kRefMasterWidth
         + gaps
         + 12;  // outer padding match
}

void ConsoleView::setBank (int bankIndex)
{
    bankIndex = juce::jlimit (0, Session::kNumBanks - 1, bankIndex);
    if (bankIndex == currentBank) return;
    currentBank = bankIndex;

    bankAButton.setToggleState (currentBank == 0, juce::dontSendNotification);
    bankBButton.setToggleState (currentBank == 1, juce::dontSendNotification);

    updateBankVisibility();
    resized();
}

void ConsoleView::updateBankVisibility()
{
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        const int bank = i / Session::kBankSize;
        strips[(size_t) i]->setVisible (showingAllTracks || bank == currentBank);
    }
}

void ConsoleView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff121214));
}

void ConsoleView::resized()
{
    auto area = getLocalBounds().reduced (6);

    // Decide whether to show all 16 tracks at reference width (windows that
    // are wide enough — typically fullscreen on a 1080p+ display) or fall
    // back to the banked-8 layout that scales to fit narrower windows.
    showingAllTracks = (area.getWidth() >= fixedWidthFor16Tracks() - 12);

    bankAButton.setVisible (! showingAllTracks);
    bankBButton.setVisible (! showingAllTracks);
    updateBankVisibility();

    if (! showingAllTracks)
    {
        auto bankBar = area.removeFromTop (kBankBarHeight);
        bankAButton.setBounds (bankBar.removeFromLeft (140).reduced (1));
        bankBar.removeFromLeft (6);
        bankBButton.setBounds (bankBar.removeFromLeft (140).reduced (1));
        area.removeFromTop (6);
    }

    const int visibleChannels = showingAllTracks ? Session::kNumTracks : Session::kBankSize;

    // Channels stay at *reference* width unless even that won't fit — in which
    // case we scale down, but only as far as the per-strip minimum. We do not
    // scale up: extra horizontal space stays as whitespace on the right.
    const int gaps = (visibleChannels - 1) * kStripGap
                   + kSectionGap
                   + (Session::kNumAuxBuses - 1) * kStripGap
                   + kSectionGap;
    const int availForStrips = juce::jmax (0, area.getWidth() - gaps);
    const int refTotal = visibleChannels * kRefChannelWidth
                       + Session::kNumAuxBuses * kRefAuxWidth
                       + kRefMasterWidth;

    int channelW = kRefChannelWidth;
    int auxW     = kRefAuxWidth;
    int masterW  = kRefMasterWidth;

    if (availForStrips < refTotal)
    {
        // Window is narrower than the reference — shrink proportionally.
        const float scale = (float) availForStrips / (float) refTotal;
        channelW = juce::jmax (kMinChannelWidth, (int) std::round (kRefChannelWidth * scale));
        auxW     = juce::jmax (kMinAuxWidth,     (int) std::round (kRefAuxWidth     * scale));
        masterW  = juce::jmax (kMinMasterWidth,  (int) std::round (kRefMasterWidth  * scale));
    }

    int x = area.getX();
    const int y = area.getY();
    const int h = area.getHeight();

    for (int i = 0; i < visibleChannels; ++i)
    {
        const int trackIdx = showingAllTracks
                              ? i
                              : (currentBank * Session::kBankSize + i);
        if (trackIdx >= Session::kNumTracks) break;
        strips[(size_t) trackIdx]->setBounds (x, y, channelW, h);
        x += channelW + (i + 1 < visibleChannels ? kStripGap : 0);
    }

    x += kSectionGap;
    for (int i = 0; i < Session::kNumAuxBuses; ++i)
    {
        auxStrips[(size_t) i]->setBounds (x, y, auxW, h);
        x += auxW + (i + 1 < Session::kNumAuxBuses ? kStripGap : 0);
    }

    x += kSectionGap;
    masterStrip->setBounds (x, y, masterW, h);
}
} // namespace adhdaw
