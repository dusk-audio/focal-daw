#include "TransportBar.h"

namespace adhdaw
{
TransportBar::TransportBar (AudioEngine& engineRef) : engine (engineRef)
{
    auto styleTransportButton = [] (juce::TextButton& b, juce::Colour onColour)
    {
        b.setClickingTogglesState (false);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, onColour);
        b.setColour (juce::TextButton::textColourOffId,  juce::Colours::lightgrey);
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    };
    styleTransportButton (playButton,   juce::Colour (0xff60c060));
    styleTransportButton (stopButton,   juce::Colour (0xffc0c0c0));
    styleTransportButton (recordButton, juce::Colour (0xffd03030));

    playButton.onClick   = [this] { engine.play();   refreshButtonStates(); };
    stopButton.onClick   = [this] { engine.stop();   refreshButtonStates(); };
    recordButton.onClick = [this] { engine.record(); refreshButtonStates(); };

    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (recordButton);

    clockLabel.setJustificationType (juce::Justification::centred);
    clockLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
    clockLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff121214));
    clockLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                        18.0f, juce::Font::bold)));
    clockLabel.setText ("00:00.000", juce::dontSendNotification);
    addAndMakeVisible (clockLabel);

    hintLabel.setJustificationType (juce::Justification::centredLeft);
    hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8090a0));
    hintLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (hintLabel);

    tapeToggle.setClickingTogglesState (true);
    tapeToggle.setToggleState (true, juce::dontSendNotification);   // expanded by default
    tapeToggle.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xbe SUMMARY")); // "▾ SUMMARY"
    tapeToggle.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    tapeToggle.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2a3a48));
    tapeToggle.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff7090a8));
    tapeToggle.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffd0e0f0));
    tapeToggle.setTooltip ("Show / hide the SUMMARY arrangement view");
    tapeToggle.onClick = [this]
    {
        const bool expanded = tapeToggle.getToggleState();
        tapeToggle.setButtonText (expanded ? juce::CharPointer_UTF8 ("\xe2\x96\xbe SUMMARY")    // "▾ SUMMARY"
                                            : juce::CharPointer_UTF8 ("\xe2\x96\xb8 SUMMARY")); // "▸ SUMMARY"
        if (onTapeStripToggle) onTapeStripToggle (expanded);
    };
    addAndMakeVisible (tapeToggle);

    refreshButtonStates();
    startTimerHz (20);
}

void TransportBar::setTapeStripExpanded (bool expanded)
{
    tapeToggle.setToggleState (expanded, juce::dontSendNotification);
    tapeToggle.setButtonText (expanded ? juce::CharPointer_UTF8 ("\xe2\x96\xbe SUMMARY")
                                        : juce::CharPointer_UTF8 ("\xe2\x96\xb8 SUMMARY"));
}

bool TransportBar::isTapeStripExpanded() const
{
    return tapeToggle.getToggleState();
}

TransportBar::~TransportBar() = default;

void TransportBar::timerCallback()
{
    const double sr = engine.getCurrentSampleRate();
    const auto playhead = engine.getTransport().getPlayhead();
    const double seconds = (sr > 0.0) ? (double) playhead / sr : 0.0;
    const int mins   = (int) (seconds / 60.0);
    const int secs   = (int) seconds % 60;
    const int millis = (int) ((seconds - std::floor (seconds)) * 1000.0);
    clockLabel.setText (juce::String::formatted ("%02d:%02d.%03d", mins, secs, millis),
                         juce::dontSendNotification);

    refreshButtonStates();
}

void TransportBar::refreshButtonStates()
{
    const auto state = engine.getTransport().getState();
    playButton.setToggleState   (state == Transport::State::Playing,   juce::dontSendNotification);
    stopButton.setToggleState   (state == Transport::State::Stopped,   juce::dontSendNotification);
    recordButton.setToggleState (state == Transport::State::Recording, juce::dontSendNotification);

    juce::String hint;
    switch (state)
    {
        case Transport::State::Stopped:
            hint = engine.getRecordManager().isActive() ? "Finalising..." :
                   "Arm a track and press REC to record. Press PLAY to play back.";
            break;
        case Transport::State::Playing:   hint = "Playing...";   break;
        case Transport::State::Recording: hint = "Recording — press STOP to commit"; break;
    }
    hintLabel.setText (hint, juce::dontSendNotification);
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawRect (getLocalBounds(), 1);
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    auto buttons = area.removeFromLeft (240);
    const int btnW = buttons.getWidth() / 3;
    stopButton.setBounds   (buttons.removeFromLeft (btnW).reduced (2));
    playButton.setBounds   (buttons.removeFromLeft (btnW).reduced (2));
    recordButton.setBounds (buttons.reduced (2));

    area.removeFromLeft (12);
    clockLabel.setBounds (area.removeFromLeft (130));

    // TAPE toggle on the right edge of the bar.
    tapeToggle.setBounds (area.removeFromRight (84).reduced (1));
    area.removeFromRight (12);

    area.removeFromLeft (12);
    hintLabel.setBounds (area);
}
} // namespace adhdaw
