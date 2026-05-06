#include "MasterStripComponent.h"
#include "FocalLookAndFeel.h"

namespace focal
{
namespace
{
void styleSmallKnob (juce::Slider& s, double minV, double maxV, double midPt,
                      double initialV, juce::Colour col, const juce::String& suffix,
                      int decimals)
{
    s.setRange (minV, maxV, 0.01);
    if (midPt > minV && midPt < maxV)
        s.setSkewFactorFromMidPoint (midPt);
    s.setValue (initialV, juce::dontSendNotification);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 48, 14);
    s.setColour (juce::Slider::rotarySliderFillColourId, col);
    s.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff2a2a2e));
    s.setColour (juce::Slider::thumbColourId, col.brighter (0.3f));
    s.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffd0d0d0));
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff181820));
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    s.setNumDecimalPlacesToDisplay (decimals);
    s.setTextValueSuffix (suffix);
}

void styleSmallLabel (juce::Label& lbl, const juce::String& text, juce::Colour col)
{
    lbl.setText (text, juce::dontSendNotification);
    lbl.setJustificationType (juce::Justification::centred);
    lbl.setColour (juce::Label::textColourId, col);
    lbl.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
}
} // namespace

MasterStripComponent::MasterStripComponent (MasterBusParams& p) : params (p)
{
    nameLabel.setText ("MASTER", juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    addAndMakeVisible (nameLabel);

    auto styleToggle = [] (juce::TextButton& b, juce::Colour onColour)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, onColour);
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffb0a080));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    };

    // EQ + comp section toggles + their parameter knobs.
    const auto pultecGold = juce::Colour (0xffd09050);
    const auto compGold   = juce::Colour (0xffe0c050);

    styleToggle (eqButton, pultecGold);
    eqButton.setToggleState (params.eqEnabled.load (std::memory_order_relaxed),
                              juce::dontSendNotification);
    eqButton.setTooltip ("Pultec-style Tube EQ on/off");
    eqButton.onClick = [this]
    {
        params.eqEnabled.store (eqButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (eqButton);

    // Suffixes intentionally empty - section labels (LF / HF+ / DRIVE / OUT)
    // already convey the unit, and a 6-char " dB"/" ms" suffix won't fit the
    // 38-px text box without truncating to "...".
    styleSmallKnob (eqLfBoost,   0.0,  10.0,   3.0, params.eqLfBoost.load(),       pultecGold, "", 1);
    styleSmallKnob (eqHfBoost,   0.0,  10.0,   3.0, params.eqHfBoost.load(),       pultecGold, "", 1);
    styleSmallKnob (eqHfAtten,   0.0,  10.0,   3.0, params.eqHfAtten.load(),       pultecGold, "", 1);
    styleSmallKnob (eqTubeDrive, 0.0,   1.0,   0.3, params.eqTubeDrive.load(),     pultecGold, "", 2);
    styleSmallKnob (eqOutputGain,-12.0, 12.0,  0.0, params.eqOutputGainDb.load(),  pultecGold, "", 1);

    eqLfBoost   .onValueChange = [this] { params.eqLfBoost     .store ((float) eqLfBoost   .getValue(), std::memory_order_relaxed); };
    eqHfBoost   .onValueChange = [this] { params.eqHfBoost     .store ((float) eqHfBoost   .getValue(), std::memory_order_relaxed); };
    eqHfAtten   .onValueChange = [this] { params.eqHfAtten     .store ((float) eqHfAtten   .getValue(), std::memory_order_relaxed); };
    eqTubeDrive .onValueChange = [this] { params.eqTubeDrive   .store ((float) eqTubeDrive .getValue(), std::memory_order_relaxed); };
    eqOutputGain.onValueChange = [this] { params.eqOutputGainDb.store ((float) eqOutputGain.getValue(), std::memory_order_relaxed); };

    addAndMakeVisible (eqLfBoost);  addAndMakeVisible (eqHfBoost);
    addAndMakeVisible (eqHfAtten); addAndMakeVisible (eqTubeDrive);
    addAndMakeVisible (eqOutputGain);

    styleSmallLabel (eqLfLabel,      "LF",     pultecGold);
    styleSmallLabel (eqHfBoostLabel, "HF+",    pultecGold);
    styleSmallLabel (eqHfAttenLabel, "HF−", pultecGold);  // HF−
    styleSmallLabel (eqDriveLabel,   "DRIVE",  pultecGold);
    styleSmallLabel (eqOutLabel,     "OUT",    pultecGold);
    addAndMakeVisible (eqLfLabel);   addAndMakeVisible (eqHfBoostLabel);
    addAndMakeVisible (eqHfAttenLabel); addAndMakeVisible (eqDriveLabel);
    addAndMakeVisible (eqOutLabel);

    styleToggle (compButton, compGold);
    compButton.setToggleState (params.compEnabled.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    compButton.setTooltip ("Bus compressor on/off (UniversalCompressor in Bus mode)");
    compButton.onClick = [this]
    {
        params.compEnabled.store (compButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (compButton);

    styleSmallKnob (compThreshold, -30.0,  0.0,  -10.0, params.compThreshDb.load(),   compGold, "",   1);
    styleSmallKnob (compRatio,       1.0, 10.0,    4.0, params.compRatio.load(),       compGold, ":1", 1);
    styleSmallKnob (compAttack,      0.1, 50.0,    5.0, params.compAttackMs.load(),    compGold, "",   1);
    styleSmallKnob (compRelease,    50.0,1000.0, 200.0, params.compReleaseMs.load(),   compGold, "",   0);
    styleSmallKnob (compMakeup,    -10.0, 20.0,    0.0, params.compMakeupDb.load(),    compGold, "",   1);

    compThreshold.onValueChange = [this] { params.compThreshDb .store ((float) compThreshold.getValue(), std::memory_order_relaxed); };
    compRatio    .onValueChange = [this] { params.compRatio    .store ((float) compRatio    .getValue(), std::memory_order_relaxed); };
    compAttack   .onValueChange = [this] { params.compAttackMs .store ((float) compAttack   .getValue(), std::memory_order_relaxed); };
    compRelease  .onValueChange = [this] { params.compReleaseMs.store ((float) compRelease  .getValue(), std::memory_order_relaxed); };
    compMakeup   .onValueChange = [this] { params.compMakeupDb .store ((float) compMakeup   .getValue(), std::memory_order_relaxed); };

    addAndMakeVisible (compThreshold); addAndMakeVisible (compRatio);
    addAndMakeVisible (compAttack);    addAndMakeVisible (compRelease);
    addAndMakeVisible (compMakeup);

    styleSmallLabel (compThrLabel, "THR", compGold);
    styleSmallLabel (compRatLabel, "RAT", compGold);
    styleSmallLabel (compAtkLabel, "ATK", compGold);
    styleSmallLabel (compRelLabel, "REL", compGold);
    styleSmallLabel (compMakLabel, "MAK", compGold);
    addAndMakeVisible (compThrLabel); addAndMakeVisible (compRatLabel);
    addAndMakeVisible (compAtkLabel); addAndMakeVisible (compRelLabel);
    addAndMakeVisible (compMakLabel);

    styleToggle (tapeButton, juce::Colour (0xffd0a060));
    styleToggle (hqButton,   juce::Colour (0xff7090c0));

    tapeButton.setToggleState (params.tapeEnabled.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    tapeButton.setTooltip ("Master tape saturation on/off - harmonic colour at the bus output");
    tapeButton.onClick = [this]
    {
        params.tapeEnabled.store (tapeButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (tapeButton);

    hqButton.setToggleState (params.tapeHQ.load (std::memory_order_relaxed),
                              juce::dontSendNotification);
    hqButton.setTooltip (juce::CharPointer_UTF8 (
        "HQ - 4× oversampling for tape stage (more CPU, lower aliasing)"));
    hqButton.onClick = [this]
    {
        params.tapeHQ.store (hqButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (hqButton);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (params.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    faderSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
    faderSlider.setTextValueSuffix (" dB");
    faderSlider.onValueChange = [this]
    {
        params.faderDb.store ((float) faderSlider.getValue(), std::memory_order_relaxed);
    };
    addAndMakeVisible (faderSlider);

    // Output meter readouts (peak dBFS + GR dB).
    auto styleReadout = [] (juce::Label& lbl, juce::Colour col)
    {
        lbl.setJustificationType (juce::Justification::centred);
        lbl.setColour (juce::Label::textColourId,        col);
        lbl.setColour (juce::Label::backgroundColourId,  juce::Colour (0xff0a0a0c));
        // No outline - sits next to the fader textbox; the 1 px border looked
        // like the two boxes were overlapping.
        lbl.setColour (juce::Label::outlineColourId,     juce::Colours::transparentBlack);
        lbl.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      11.0f, juce::Font::bold)));
    };
    outputPeakLabel.setText ("-inf", juce::dontSendNotification);
    styleReadout (outputPeakLabel, juce::Colour (0xffd0d0d0));
    addAndMakeVisible (outputPeakLabel);
    grPeakLabel.setText ("0.0", juce::dontSendNotification);
    styleReadout (grPeakLabel, juce::Colour (0xff606064));
    addAndMakeVisible (grPeakLabel);

    startTimerHz (30);
}

MasterStripComponent::~MasterStripComponent() = default;

void MasterStripComponent::timerCallback()
{
    // Output peak per channel - fast attack, slow release; with peak-hold.
    auto smoothChannel = [] (float& displayed, float& peakHold, int& peakFrames, float src)
    {
        if (src > displayed) displayed = src;
        else                  displayed += (src - displayed) * 0.15f;

        if (src >= peakHold) { peakHold = src; peakFrames = 18; }
        else if (peakFrames > 0) --peakFrames;
        else peakHold = juce::jmax (-100.0f, peakHold - 1.5f);
    };
    const float outL = params.meterPostMasterLDb.load (std::memory_order_relaxed);
    const float outR = params.meterPostMasterRDb.load (std::memory_order_relaxed);
    smoothChannel (displayedOutputLDb, outputPeakHoldLDb, outputPeakHoldFramesL, outL);
    smoothChannel (displayedOutputRDb, outputPeakHoldRDb, outputPeakHoldFramesR, outR);

    // Numeric readout shows the louder of the two channels (typical mixer
    // convention - we don't have room for two separate values).
    const float maxHold = juce::jmax (outputPeakHoldLDb, outputPeakHoldRDb);
    if (maxHold <= -60.0f)
        outputPeakLabel.setText ("-inf", juce::dontSendNotification);
    else
        outputPeakLabel.setText (juce::String (maxHold, 1), juce::dontSendNotification);
    outputPeakLabel.setColour (juce::Label::textColourId,
        maxHold >= -3.0f  ? juce::Colour (0xffff5050) :
        maxHold >= -12.0f ? juce::Colour (0xffe0c050) :
                             juce::Colour (0xffd0d0d0));

    // Bus-comp GR.
    const float gr = params.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb) displayedGrDb = gr;
    else                    displayedGrDb += (gr - displayedGrDb) * 0.18f;

    if (displayedGrDb <= -0.05f)
    {
        grPeakLabel.setText (juce::String (displayedGrDb, 1), juce::dontSendNotification);
        grPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0c050));
    }
    else
    {
        grPeakLabel.setText ("0.0", juce::dontSendNotification);
        grPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xff606064));
    }

    if (! meterArea.isEmpty())
        repaint (meterArea);
    if (! grMeterArea.isEmpty())
        repaint (grMeterArea.expanded (2, 10));   // include "GR" caption
}

void MasterStripComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    g.setColour (juce::Colour (0xff202024));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xffd0a060));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xff3a3a3e));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.5f);

    // Stereo LED meter - two vertical bars (L | R) inside meterArea.
    if (! meterArea.isEmpty())
    {
        constexpr float kMinDb = -60.0f, kMaxDb = 6.0f;
        constexpr float kBarGap = 1.0f;

        auto drawColumn = [&] (juce::Rectangle<float> bar, float displayedDb)
        {
            g.setColour (juce::Colour (0xff0c0c0e));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (juce::Colour (0xff2a2a2e));
            g.drawRoundedRectangle (bar, 1.5f, 0.6f);

            const float frac = juce::jlimit (0.0f, 1.0f,
                (displayedDb - kMinDb) / (kMaxDb - kMinDb));
            const float fillH = (bar.getHeight() - 4.0f) * frac;
            if (fillH > 0.5f)
            {
                auto fillRect = juce::Rectangle<float> (bar.getX() + 1.5f,
                                                         bar.getBottom() - 2.0f - fillH,
                                                         bar.getWidth() - 3.0f, fillH);
                juce::ColourGradient grad (juce::Colour (0xff70c060),
                                            fillRect.getX(), fillRect.getBottom(),
                                            juce::Colour (0xffff5050),
                                            fillRect.getX(), bar.getY(), false);
                grad.addColour (0.65, juce::Colour (0xffe0c050));
                g.setGradientFill (grad);
                g.fillRoundedRectangle (fillRect, 1.0f);
            }
        };

        const auto fullBar = meterArea.toFloat();
        const float colW   = (fullBar.getWidth() - kBarGap) * 0.5f;
        drawColumn (juce::Rectangle<float> (fullBar.getX(), fullBar.getY(),
                                              colW, fullBar.getHeight()),
                     displayedOutputLDb);
        drawColumn (juce::Rectangle<float> (fullBar.getX() + colW + kBarGap, fullBar.getY(),
                                              colW, fullBar.getHeight()),
                     displayedOutputRDb);
    }

    // Master-bus comp GR meter - fills DOWN from the top as compression
    // bites. Same gold→red colour story as the channel and aux strips.
    if (! grMeterArea.isEmpty())
    {
        const auto bar = grMeterArea.toFloat();
        g.setColour (juce::Colour (0xff0c0c0e));
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (juce::Colour (0xff2a2a2e));
        g.drawRoundedRectangle (bar, 1.5f, 0.5f);

        constexpr float kGrFloorDb = 20.0f;
        const float grAbs = juce::jlimit (0.0f, kGrFloorDb, std::abs (displayedGrDb));
        if (grAbs > 0.05f)
        {
            const float frac = grAbs / kGrFloorDb;
            const float fillH = (bar.getHeight() - 4.0f) * frac;
            auto fillRect = juce::Rectangle<float> (bar.getX() + 1.5f,
                                                      bar.getY() + 2.0f,
                                                      bar.getWidth() - 3.0f, fillH);
            juce::ColourGradient grad (juce::Colour (0xffe0c050).brighter (0.2f),
                                         bar.getX(), bar.getY(),
                                         juce::Colour (0xffe05050).brighter (0.1f),
                                         bar.getX(), bar.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (fillRect, 1.0f);
        }

        // "GR" caption above the bar.
        g.setColour (juce::Colour (0xff909094));
        g.setFont (juce::Font (juce::FontOptions (7.0f, juce::Font::bold)));
        g.drawText ("GR",
                     juce::Rectangle<float> (bar.getX() - 2.0f, bar.getY() - 9.0f,
                                              bar.getWidth() + 4.0f, 8.0f),
                     juce::Justification::centred, false);
    }

    // Fader dB scale labels - aligned with the LookAndFeel-drawn ticks on
    // the fader track. Same kFaderTicks set as the channel + aux strips.
    if (! faderScaleArea.isEmpty())
    {
        const auto& range = faderSlider.getNormalisableRange();
        for (const auto& t : kFaderTicks)
        {
            if (t.db < range.start - 0.01f || t.db > range.end + 0.01f) continue;
            const float y = faderYForDb (faderSlider, t.db);
            const bool isZero = (std::abs (t.db) < 0.01f);
            g.setColour (isZero ? juce::Colour (0xffe0e0e0) : juce::Colour (0xff909094));
            g.setFont (juce::Font (juce::FontOptions (isZero ? 9.5f : 8.5f,
                                                        isZero ? juce::Font::bold
                                                                : juce::Font::plain)));
            const auto rect = juce::Rectangle<float> ((float) faderScaleArea.getX(), y - 5.0f,
                                                        (float) faderScaleArea.getWidth(), 10.0f);
            g.drawText (t.label, rect, juce::Justification::centredRight, false);
        }
    }
}

void MasterStripComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);
    nameLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (6);

    // 26 px rotary diameter (matches channel strip) + 14 px textbox below.
    // Block width is 40 - 28 px was too narrow and clipped both the bottom
    // value readout (e.g. "4.0:1" became "4...." ) and the top label.
    constexpr int kKnobDia    = 26;
    constexpr int kTextBoxH   = 14;
    constexpr int kKnobBlockH = kKnobDia + kTextBoxH + 2;   // 42
    constexpr int kKnobBlockW = 40;                          // textbox fits "4.0:1", "1100"

    auto layKnobRow = [] (juce::Rectangle<int>& parent, int n)
                       -> std::pair<juce::Rectangle<int>, juce::Rectangle<int>>
    {
        auto labelRow = parent.removeFromTop (10);
        auto knobRow  = parent.removeFromTop (kKnobBlockH);
        const int totalW = n * kKnobBlockW;
        const int leftPad = juce::jmax (0, (labelRow.getWidth() - totalW) / 2);
        labelRow.removeFromLeft (leftPad);
        knobRow .removeFromLeft (leftPad);
        return { labelRow, knobRow };
    };

    // EQ section: bypass toggle, then 5 fixed-size knob blocks centred in row.
    eqButton.setBounds (area.removeFromTop (20).reduced (4, 0));
    area.removeFromTop (2);
    {
        auto rows = layKnobRow (area, 5);
        auto& lblRow  = rows.first;
        auto& knobRow = rows.second;
        eqLfLabel     .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqHfBoostLabel.setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqHfAttenLabel.setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqDriveLabel  .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqOutLabel    .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        eqLfBoost     .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqHfBoost     .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqHfAtten     .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqTubeDrive   .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        eqOutputGain  .setBounds (knobRow.removeFromLeft (kKnobBlockW));
    }
    area.removeFromTop (4);

    // Comp section: same shape.
    compButton.setBounds (area.removeFromTop (20).reduced (4, 0));
    area.removeFromTop (2);
    {
        auto rows = layKnobRow (area, 5);
        auto& lblRow  = rows.first;
        auto& knobRow = rows.second;
        compThrLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compRatLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compAtkLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compRelLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compMakLabel .setBounds (lblRow .removeFromLeft (kKnobBlockW));
        compThreshold.setBounds (knobRow.removeFromLeft (kKnobBlockW));
        compRatio    .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        compAttack   .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        compRelease  .setBounds (knobRow.removeFromLeft (kKnobBlockW));
        compMakeup   .setBounds (knobRow.removeFromLeft (kKnobBlockW));
    }
    area.removeFromTop (6);

    // TAPE + HQ row.
    auto buttonRow = area.removeFromTop (24);
    const int colW = buttonRow.getWidth() / 2;
    tapeButton.setBounds (buttonRow.removeFromLeft (colW).reduced (2, 0));
    hqButton  .setBounds (buttonRow.reduced (2, 0));

    area.removeFromTop (4);

    // Peak readouts above the meter.
    auto peakRow = area.removeFromBottom (16);
    const int prW = peakRow.getWidth() / 2;
    outputPeakLabel.setBounds (peakRow.removeFromLeft (prW));
    grPeakLabel    .setBounds (peakRow);
    area.removeFromBottom (2);

    // Stereo LED meter on the RIGHT side of the fader (matches channel-strip
    // visual hierarchy - fader on the left, meter on the right edge of the
    // strip). 16 px = 2 × 7 px columns + 2 px gap; same proportions as the
    // channel strip's meter column. A slim GR bar sits to the meter's left
    // so the bus comp's gain reduction is visible alongside output level.
    constexpr int kMeterW       = 16;
    constexpr int kGrMeterW     = 9;
    constexpr int kGrGap        = 2;
    constexpr int kFaderScaleW  = 16;
    constexpr int kFaderScaleGap = 3;
    meterArea   = area.removeFromRight (kMeterW);
    area.removeFromRight (kGrGap);
    grMeterArea = area.removeFromRight (kGrMeterW);
    area.removeFromRight (kFaderScaleGap);
    faderScaleArea = area.removeFromRight (kFaderScaleW);
    area.removeFromRight (4);
    faderSlider.setBounds (area);
}
} // namespace focal
