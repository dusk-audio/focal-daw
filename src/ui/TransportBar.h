#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"

namespace adhdaw
{
// Circular icon button used for the transport row (Stop / Play / Record).
// Replaces the text-only buttons with a real-mixer look: a dark filled disc
// with a coloured rim that lights up when the button is the active state,
// and a vector icon (square / triangle / disc) drawn in the centre. Reuses
// JUCE's Button base for click/hover/state plumbing.
class TransportIconButton final : public juce::Button
{
public:
    enum class Icon { Stop, Play, Record };

    TransportIconButton (const juce::String& name, Icon icon, juce::Colour activeColour);

    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    Icon iconType;
    juce::Colour activeColour;
};

class TransportBar final : public juce::Component, private juce::Timer
{
public:
    explicit TransportBar (AudioEngine& engineRef);
    ~TransportBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // MainComponent overlays the stage + bank buttons on top of this bar so
    // it can show all the controls in a single row. When that overlay is
    // active the hintLabel ("Arm a track and press REC to record...") is
    // hidden so it doesn't clash with the buttons sitting on top of it.
    void setHintVisible (bool visible);

private:
    void timerCallback() override;
    void refreshButtonStates();

    AudioEngine& engine;
    TransportIconButton stopButton   { "Stop",   TransportIconButton::Icon::Stop,
                                        juce::Colour (0xffd0d0d0) };
    TransportIconButton playButton   { "Play",   TransportIconButton::Icon::Play,
                                        juce::Colour (0xff60c060) };
    TransportIconButton recordButton { "Record", TransportIconButton::Icon::Record,
                                        juce::Colour (0xffd03030) };
    juce::TextButton loopToggle    { "LOOP" };
    juce::TextButton punchToggle   { "PUNCH" };
    juce::TextButton snapToggle    { "SNAP" };
    juce::TextButton clickToggle   { "CLICK" };
    juce::TextButton countInToggle { "C/I" };
    juce::Label      bpmCaption;
    juce::Label      bpmValue;
    juce::TextButton tapeToggle    { juce::CharPointer_UTF8 ("\xe2\x96\xbe SUMMARY") };  // "▾ SUMMARY" - toggles the arrangement/summary view
    juce::Label      clockLabel;
    juce::Label      hintLabel;

public:
    // Set by MainComponent to receive toggle clicks. Fires after the button's
    // toggle state has flipped - the new collapsed/expanded state is the
    // boolean argument (true = expanded).
    std::function<void (bool)> onTapeStripToggle;

    void setTapeStripExpanded (bool expanded);
    bool isTapeStripExpanded() const;
};
} // namespace adhdaw
