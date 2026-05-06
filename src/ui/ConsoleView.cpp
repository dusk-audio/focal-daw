#include "ConsoleView.h"

namespace adhdaw
{
ConsoleView::ConsoleView (Session& session, AudioEngine& engine) : sessionRef (session)
{
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        strips[(size_t) i] = std::make_unique<ChannelStripComponent> (
            i, session.track (i), session, engine.getStrip (i).getPluginSlot());
        addAndMakeVisible (strips[(size_t) i].get());
    }
    for (int i = 0; i < Session::kNumAuxBuses; ++i)
    {
        auxStrips[(size_t) i] = std::make_unique<AuxBusComponent> (
            session.aux (i), session, i);
        addAndMakeVisible (auxStrips[(size_t) i].get());
    }
    masterStrip = std::make_unique<MasterStripComponent> (session.master());
    addAndMakeVisible (masterStrip.get());

    // BANK A/B controls were previously rendered here as a 28-px row at the
    // top of ConsoleView. They moved up to MainComponent (under the stage
    // selector) so the channel strips get the full vertical body for taller
    // faders. ConsoleView now owns only the bank-state model + visibility.
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
    // *reference* widths - this is the threshold above which we drop the
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

    updateBankVisibility();
    resized();

    // The MainComponent owns the BANK A / BANK B buttons; ask the parent
    // chain to refresh its layout so its toggle states stay in sync.
    if (auto* parent = getParentComponent())
        parent->resized();
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
    // are wide enough - typically fullscreen on a 1080p+ display) or fall
    // back to the banked-8 layout that scales to fit narrower windows.
    showingAllTracks = (area.getWidth() >= fixedWidthFor16Tracks() - 12);
    updateBankVisibility();

    const int visibleChannels = showingAllTracks ? Session::kNumTracks : Session::kBankSize;

    // Channels stay at *reference* width unless even that won't fit - in which
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
        // Window is narrower than the reference - shrink proportionally.
        const float scale = (float) availForStrips / (float) refTotal;
        channelW = juce::jmax (kMinChannelWidth, (int) std::round (kRefChannelWidth * scale));
        auxW     = juce::jmax (kMinAuxWidth,     (int) std::round (kRefAuxWidth     * scale));
        masterW  = juce::jmax (kMinMasterWidth,  (int) std::round (kRefMasterWidth  * scale));

        // Secondary fit-to-budget pass: if any kMin floor pushed the total
        // above availForStrips, shrink everything uniformly until it fits.
        // We accept going under kMin in the very-narrow case - slightly
        // squished knobs are better than the master strip being clipped
        // off the right edge of the window.
        const int totalContent = visibleChannels * channelW
                               + Session::kNumAuxBuses * auxW
                               + masterW;
        if (totalContent > availForStrips && totalContent > 0)
        {
            const float secondary = (float) availForStrips / (float) totalContent;
            channelW = juce::jmax (1, (int) std::floor (channelW * secondary));
            auxW     = juce::jmax (1, (int) std::floor (auxW     * secondary));
            masterW  = juce::jmax (1, (int) std::floor (masterW  * secondary));
        }
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

void ConsoleView::setStripsCompactMode (bool compact)
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->setCompactMode (compact);
}

void ConsoleView::setStripsMixingMode (bool mixing)
{
    for (auto& strip : strips)
        if (strip != nullptr)
            strip->setMixingMode (mixing);
}
} // namespace adhdaw
