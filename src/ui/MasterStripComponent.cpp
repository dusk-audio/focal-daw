#include "MasterStripComponent.h"

namespace adhdaw
{
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
    styleToggle (tapeButton, juce::Colour (0xffd0a060));
    styleToggle (hqButton,   juce::Colour (0xff7090c0));

    tapeButton.setToggleState (params.tapeEnabled.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    tapeButton.setTooltip ("Master tape saturation on/off — harmonic colour at the bus output");
    tapeButton.onClick = [this]
    {
        params.tapeEnabled.store (tapeButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (tapeButton);

    hqButton.setToggleState (params.tapeHQ.load (std::memory_order_relaxed),
                              juce::dontSendNotification);
    hqButton.setTooltip ("HQ — 4× oversampling for tape stage (more CPU, lower aliasing)");
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
}

void MasterStripComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);
    nameLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (8);

    // TAPE + HQ side by side. Both fit comfortably in the master strip width.
    auto buttonRow = area.removeFromTop (24);
    const int colW = buttonRow.getWidth() / 2;
    tapeButton.setBounds (buttonRow.removeFromLeft (colW).reduced (2, 0));
    hqButton.setBounds   (buttonRow.reduced (2, 0));

    area.removeFromTop (8);
    faderSlider.setBounds (area);
}
} // namespace adhdaw
