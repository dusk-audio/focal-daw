#include "ChannelCompEditor.h"
#include "ADHDawLookAndFeel.h"

namespace adhdaw
{
namespace
{
void styleKnob (juce::Slider& k, juce::Colour fill,
                double mn, double mx, double defaultVal, double skewMid,
                const juce::String& suffix, int decimals)
{
    k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.setColour (juce::Slider::rotarySliderFillColourId, fill);
    k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    k.setRange (mn, mx, 0.01);
    if (skewMid > 0) k.setSkewFactorFromMidPoint (skewMid);
    k.setDoubleClickReturnValue (true, defaultVal);
    k.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
    k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffe0e0e0));
    k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
    k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
    k.setNumDecimalPlacesToDisplay (decimals);
    k.setTextValueSuffix (suffix);
}

void styleLabel (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    l.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
}
} // namespace

ChannelCompEditor::ChannelCompEditor (Track& t) : track (t)
{
    titleLabel.setText ("COMP — " + track.name, juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

    onButton.setClickingTogglesState (true);
    onButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    onButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kCompGold));
    onButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    onButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    onButton.setToggleState (track.strip.compEnabled.load (std::memory_order_relaxed),
                              juce::dontSendNotification);
    onButton.onClick = [this]
    {
        track.strip.compEnabled.store (onButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (onButton);

    auto styleModeBtn = [] (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (5, juce::dontSendNotification);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff181820));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kCompGold));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    };
    styleModeBtn (modeOpto);  styleModeBtn (modeFet);  styleModeBtn (modeVca);
    modeOpto.onClick = [this] { setMode (0); };
    modeFet.onClick  = [this] { setMode (1); };
    modeVca.onClick  = [this] { setMode (2); };
    addAndMakeVisible (modeOpto);
    addAndMakeVisible (modeFet);
    addAndMakeVisible (modeVca);
    refreshModeButtons();

    const auto gold = juce::Colour (fourKColors::kCompGold);
    styleKnob (threshKnob,  gold, ChannelStripParams::kCompThreshMin,  ChannelStripParams::kCompThreshMax,  -12.0,  -24.0, " dB", 1);
    styleKnob (ratioKnob,   gold, ChannelStripParams::kCompRatioMin,   ChannelStripParams::kCompRatioMax,    4.0,    4.0, ":1",  1);
    styleKnob (attackKnob,  gold, ChannelStripParams::kCompAttackMin,  ChannelStripParams::kCompAttackMax,  10.0,   10.0, " ms", 1);
    styleKnob (releaseKnob, gold, ChannelStripParams::kCompReleaseMin, ChannelStripParams::kCompReleaseMax,100.0,  200.0, " ms", 0);
    styleKnob (makeupKnob,  gold, ChannelStripParams::kCompMakeupMin,  ChannelStripParams::kCompMakeupMax,   0.0,    0.0, " dB", 1);

    threshKnob.setValue  (track.strip.compThresholdDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    ratioKnob.setValue   (track.strip.compRatio.load       (std::memory_order_relaxed), juce::dontSendNotification);
    attackKnob.setValue  (track.strip.compAttackMs.load    (std::memory_order_relaxed), juce::dontSendNotification);
    releaseKnob.setValue (track.strip.compReleaseMs.load   (std::memory_order_relaxed), juce::dontSendNotification);
    makeupKnob.setValue  (track.strip.compMakeupDb.load    (std::memory_order_relaxed), juce::dontSendNotification);

    threshKnob.onValueChange  = [this] { track.strip.compThresholdDb.store ((float) threshKnob.getValue(),  std::memory_order_relaxed); };
    ratioKnob.onValueChange   = [this] { track.strip.compRatio.store       ((float) ratioKnob.getValue(),   std::memory_order_relaxed); };
    attackKnob.onValueChange  = [this] { track.strip.compAttackMs.store    ((float) attackKnob.getValue(),  std::memory_order_relaxed); };
    releaseKnob.onValueChange = [this] { track.strip.compReleaseMs.store   ((float) releaseKnob.getValue(), std::memory_order_relaxed); };
    makeupKnob.onValueChange  = [this] { track.strip.compMakeupDb.store    ((float) makeupKnob.getValue(),  std::memory_order_relaxed); };

    addAndMakeVisible (threshKnob);  addAndMakeVisible (ratioKnob);
    addAndMakeVisible (attackKnob);  addAndMakeVisible (releaseKnob);
    addAndMakeVisible (makeupKnob);

    styleLabel (threshLabel,  "THRESHOLD");
    styleLabel (ratioLabel,   "RATIO");
    styleLabel (attackLabel,  "ATTACK");
    styleLabel (releaseLabel, "RELEASE");
    styleLabel (makeupLabel,  "MAKEUP");
    addAndMakeVisible (threshLabel);  addAndMakeVisible (ratioLabel);
    addAndMakeVisible (attackLabel);  addAndMakeVisible (releaseLabel);
    addAndMakeVisible (makeupLabel);

    setSize (340, 380);
    startTimerHz (30);
}

ChannelCompEditor::~ChannelCompEditor() = default;

void ChannelCompEditor::setMode (int modeIndex)
{
    track.strip.compMode.store (modeIndex, std::memory_order_relaxed);
    refreshModeButtons();
}

void ChannelCompEditor::refreshModeButtons()
{
    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    modeOpto.setToggleState (m == 0, juce::dontSendNotification);
    modeFet.setToggleState  (m == 1, juce::dontSendNotification);
    modeVca.setToggleState  (m == 2, juce::dontSendNotification);
}

void ChannelCompEditor::timerCallback()
{
    const float gr = track.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb)
        displayedGrDb = gr;
    else
        displayedGrDb += (gr - displayedGrDb) * 0.18f;
    if (! grMeterArea.isEmpty())
        repaint (grMeterArea);
}

void ChannelCompEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawRect (getLocalBounds(), 1);

    if (! grMeterArea.isEmpty())
    {
        const auto bar = grMeterArea.toFloat();
        g.setColour (juce::Colour (0xff0c0c0e));
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (juce::Colour (0xff2a2a2e));
        g.drawRoundedRectangle (bar, 2.0f, 0.8f);

        const float grAbs = juce::jlimit (0.0f, 20.0f, std::abs (displayedGrDb));
        if (grAbs > 0.05f)
        {
            const float frac = grAbs / 20.0f;
            const float fillW = (bar.getWidth() - 4.0f) * frac;
            auto fillRect = juce::Rectangle<float> (bar.getRight() - 2.0f - fillW,
                                                     bar.getY() + 2.0f,
                                                     fillW, bar.getHeight() - 4.0f);
            juce::ColourGradient grad (juce::Colour (fourKColors::kCompGold).brighter (0.2f),
                                        fillRect.getRight(), 0.0f,
                                        juce::Colour (fourKColors::kHfRed).brighter (0.1f),
                                        fillRect.getX(), 0.0f, false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (fillRect, 1.5f);
        }

        g.setColour (juce::Colour (0xffa0a0a0));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("GR " + juce::String (displayedGrDb, 1) + " dB",
                     bar.toNearestInt().withTrimmedRight (4),
                     juce::Justification::centredRight, false);
    }
}

void ChannelCompEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto header = area.removeFromTop (24);
    onButton.setBounds (header.removeFromRight (60).reduced (1));
    titleLabel.setBounds (header);
    area.removeFromTop (8);

    auto modeRow = area.removeFromTop (24);
    const int modeColW = modeRow.getWidth() / 3;
    modeOpto.setBounds (modeRow.removeFromLeft (modeColW).reduced (2, 0));
    modeFet.setBounds  (modeRow.removeFromLeft (modeColW).reduced (2, 0));
    modeVca.setBounds  (modeRow.reduced (2, 0));
    area.removeFromTop (12);

    constexpr int kKnobBlockH = 56 + 18 + 4;

    auto layoutPair = [&area] (juce::Slider& kA, juce::Label& lA,
                                juce::Slider& kB, juce::Label& lB)
    {
        auto labelRow = area.removeFromTop (14);
        auto knobRow  = area.removeFromTop (kKnobBlockH);
        const int colW = labelRow.getWidth() / 2;
        lA.setBounds (labelRow.removeFromLeft (colW));
        lB.setBounds (labelRow);
        kA.setBounds (knobRow.removeFromLeft (colW).reduced (4));
        kB.setBounds (knobRow.reduced (4));
        area.removeFromTop (6);
    };
    layoutPair (threshKnob,  threshLabel,  ratioKnob,   ratioLabel);
    layoutPair (attackKnob,  attackLabel,  releaseKnob, releaseLabel);

    grMeterArea = area.removeFromTop (16).reduced (4, 0);
    area.removeFromTop (6);

    auto makeupArea = area.removeFromTop (kKnobBlockH);
    makeupLabel.setBounds (makeupArea.removeFromTop (14));
    makeupKnob.setBounds (makeupArea.reduced (60, 4));
}
} // namespace adhdaw
