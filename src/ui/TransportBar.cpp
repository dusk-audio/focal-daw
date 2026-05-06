#include "TransportBar.h"

namespace focal
{
TransportIconButton::TransportIconButton (const juce::String& name, Icon icon,
                                            juce::Colour active)
    : juce::Button (name), iconType (icon), activeColour (active)
{
    setClickingTogglesState (false);
}

void TransportIconButton::paintButton (juce::Graphics& g, bool isMouseOver,
                                         bool isButtonDown)
{
    const bool active = getToggleState();
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);

    // Disc body - a darker shade of the active colour when on, near-black
    // otherwise. The rim is the saturated active colour so it reads as a
    // ring around the icon.
    const auto bg  = active ? activeColour.darker (0.55f) : juce::Colour (0xff262630);
    const auto rim = active ? activeColour                : juce::Colour (0xff3a3a44);

    auto disc = bg;
    if (isButtonDown) disc = disc.brighter (0.10f);
    else if (isMouseOver) disc = disc.brighter (0.06f);

    g.setColour (disc);
    g.fillEllipse (bounds);
    g.setColour (rim);
    g.drawEllipse (bounds, isButtonDown ? 2.4f : 1.6f);

    // Icon glyph - fits inside an inner square at ~46 % of the disc.
    const float iconExtent = bounds.getWidth() * 0.46f;
    auto iconBox = juce::Rectangle<float> (iconExtent, iconExtent)
                       .withCentre (bounds.getCentre());
    const auto iconColour = active ? juce::Colours::white
                                   : juce::Colour (0xffd0d0d0);
    g.setColour (iconColour);

    switch (iconType)
    {
        case Icon::Stop:
        {
            // Slightly inset so the square doesn't touch the disc rim.
            g.fillRoundedRectangle (iconBox.reduced (1.0f), 1.5f);
            break;
        }
        case Icon::Play:
        {
            // Right-pointing triangle, optically nudged so its visual centre
            // matches the geometric centre of the disc (triangles look
            // off-centre when their bounding box is centred).
            auto p = iconBox.translated (iconBox.getWidth() * 0.10f, 0.0f);
            juce::Path tri;
            tri.addTriangle (p.getX(),       p.getY(),
                              p.getX(),       p.getBottom(),
                              p.getRight(),   p.getCentreY());
            g.fillPath (tri);
            break;
        }
        case Icon::Record:
        {
            g.fillEllipse (iconBox);
            break;
        }
    }
}

TransportBar::TransportBar (AudioEngine& engineRef) : engine (engineRef)
{
    playButton.onClick   = [this] { engine.play();   refreshButtonStates(); };
    stopButton.onClick   = [this] { engine.stop();   refreshButtonStates(); };
    recordButton.onClick = [this] { engine.record(); refreshButtonStates(); };

    playButton  .setTooltip ("Play (Space)");
    stopButton  .setTooltip ("Stop");
    recordButton.setTooltip ("Record - arm a track first");

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

    auto styleModeToggle = [] (juce::TextButton& b, juce::Colour onColour)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, onColour.darker (0.45f));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    };

    styleModeToggle (loopToggle,  juce::Colour (0xff3aa860));   // green when on (matches loop markers)
    styleModeToggle (punchToggle, juce::Colour (0xffd05a5a));   // record-red when on
    styleModeToggle (snapToggle,  juce::Colour (0xffd0a040));   // amber when on
    styleModeToggle (clickToggle,   juce::Colour (0xff60c060));   // green when on
    styleModeToggle (countInToggle, juce::Colour (0xff60c060));   // green: same family as CLICK

    loopToggle.setTooltip ("Loop the timeline between IN and OUT during playback (L). "
                           "Right-click the ruler in the SUMMARY view to set the points.");
    punchToggle.setTooltip ("Only commit recorded audio between PUNCH IN and PUNCH OUT (P). "
                            "Right-click the ruler in the SUMMARY view to set the points.");
    snapToggle.setTooltip ("Snap region drags to 1-second boundaries.");

    loopToggle.setToggleState (engine.getTransport().isLoopEnabled(),  juce::dontSendNotification);
    punchToggle.setToggleState (engine.getTransport().isPunchEnabled(), juce::dontSendNotification);
    snapToggle.setToggleState (engine.getSession().snapToGrid,          juce::dontSendNotification);

    loopToggle.onClick = [this]
    {
        engine.getTransport().setLoopEnabled (loopToggle.getToggleState());
    };
    punchToggle.onClick = [this]
    {
        engine.getTransport().setPunchEnabled (punchToggle.getToggleState());
    };
    snapToggle.onClick = [this]
    {
        engine.getSession().snapToGrid = snapToggle.getToggleState();
    };

    addAndMakeVisible (loopToggle);
    addAndMakeVisible (punchToggle);
    addAndMakeVisible (snapToggle);

    // CLICK toggle + BPM editable display.
    clickToggle.setTooltip ("Toggle the metronome click. The click is mixed into the "
                             "master output but never recorded - it's a monitoring aid.");
    clickToggle.setToggleState (engine.getSession().metronomeEnabled.load(),
                                  juce::dontSendNotification);
    clickToggle.onClick = [this]
    {
        engine.getSession().metronomeEnabled.store (clickToggle.getToggleState());
    };
    addAndMakeVisible (clickToggle);

    countInToggle.setTooltip ("Count-in: when on, hitting Record rolls the playhead "
                               "back one bar so the metronome ticks a full bar before "
                               "the take begins. The pre-roll audio is NOT recorded.");
    countInToggle.setToggleState (engine.getSession().countInEnabled.load(),
                                    juce::dontSendNotification);
    countInToggle.onClick = [this]
    {
        engine.getSession().countInEnabled.store (countInToggle.getToggleState());
    };
    addAndMakeVisible (countInToggle);

    bpmCaption.setText ("BPM", juce::dontSendNotification);
    bpmCaption.setJustificationType (juce::Justification::centredRight);
    bpmCaption.setColour (juce::Label::textColourId, juce::Colour (0xff707074));
    bpmCaption.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    addAndMakeVisible (bpmCaption);

    bpmValue.setJustificationType (juce::Justification::centred);
    bpmValue.setColour (juce::Label::textColourId,        juce::Colour (0xffe0e0e0));
    bpmValue.setColour (juce::Label::backgroundColourId,  juce::Colour (0xff121214));
    bpmValue.setColour (juce::Label::outlineColourId,     juce::Colour (0xff2a2a32));
    bpmValue.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                      14.0f, juce::Font::bold)));
    bpmValue.setText (juce::String ((int) engine.getSession().tempoBpm.load()),
                       juce::dontSendNotification);
    bpmValue.setEditable (false, true);  // double-click to edit; Enter commits
    bpmValue.setTooltip ("Tempo in beats per minute. Double-click to edit. "
                          "Drives the metronome click and the snap-to-beat grid.");
    bpmValue.onTextChange = [this]
    {
        const auto raw = bpmValue.getText().getFloatValue();
        const float clamped = juce::jlimit (30.0f, 300.0f, raw);
        engine.getSession().tempoBpm.store (clamped);
        bpmValue.setText (juce::String ((int) clamped), juce::dontSendNotification);
    };
    addAndMakeVisible (bpmValue);

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

    // Sync mode-toggle visuals with the engine - they may have been
    // changed externally (session load, or a hotkey).
    loopToggle.setToggleState  (engine.getTransport().isLoopEnabled(),
                                juce::dontSendNotification);
    punchToggle.setToggleState (engine.getTransport().isPunchEnabled(),
                                juce::dontSendNotification);
    snapToggle.setToggleState  (engine.getSession().snapToGrid,
                                juce::dontSendNotification);
    clickToggle.setToggleState (engine.getSession().metronomeEnabled.load(),
                                juce::dontSendNotification);
    countInToggle.setToggleState (engine.getSession().countInEnabled.load(),
                                   juce::dontSendNotification);

    // Avoid stomping the user's mid-edit text - only refresh the BPM
    // display when the label isn't currently being typed into.
    if (! bpmValue.isBeingEdited())
        bpmValue.setText (juce::String ((int) engine.getSession().tempoBpm.load()),
                           juce::dontSendNotification);

    juce::String hint;
    switch (state)
    {
        case Transport::State::Stopped:
            hint = engine.getRecordManager().isActive() ? "Finalising..." :
                   "Arm a track and press REC to record. Press PLAY to play back.";
            break;
        case Transport::State::Playing:   hint = "Playing...";   break;
        case Transport::State::Recording: hint = "Recording - press STOP to commit"; break;
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

    // Circular transport buttons. Square bounds so the disc renders true.
    // 36 px diameter with 4 px spacing; centred vertically in the row.
    constexpr int kBtnDia = 36;
    constexpr int kBtnGap = 4;
    auto buttons = area.removeFromLeft (kBtnDia * 3 + kBtnGap * 2);
    const int yPad = juce::jmax (0, (buttons.getHeight() - kBtnDia) / 2);
    auto buttonRow = buttons.reduced (0, yPad);

    auto place = [&] (juce::Component& c)
    {
        c.setBounds (buttonRow.removeFromLeft (kBtnDia));
        if (buttonRow.getWidth() >= kBtnGap)
            buttonRow.removeFromLeft (kBtnGap);
    };
    place (stopButton);
    place (playButton);
    place (recordButton);

    area.removeFromLeft (12);
    clockLabel.setBounds (area.removeFromLeft (130));

    // TAPE toggle on the right edge of the bar; SNAP/PUNCH/LOOP sit inside it.
    tapeToggle.setBounds (area.removeFromRight (84).reduced (1));
    area.removeFromRight (12);

    // BPM editor + CLICK + C/I toggles.
    countInToggle.setBounds (area.removeFromRight (44).reduced (1, 4));
    area.removeFromRight (4);
    clickToggle.setBounds (area.removeFromRight (60).reduced (1, 4));
    area.removeFromRight (4);
    bpmValue.setBounds   (area.removeFromRight (52).reduced (1, 4));
    bpmCaption.setBounds (area.removeFromRight (32).reduced (1, 4));
    area.removeFromRight (12);

    snapToggle.setBounds  (area.removeFromRight (54).reduced (1, 4));
    area.removeFromRight (4);
    punchToggle.setBounds (area.removeFromRight (60).reduced (1, 4));
    area.removeFromRight (4);
    loopToggle.setBounds  (area.removeFromRight (54).reduced (1, 4));
    area.removeFromRight (12);

    area.removeFromLeft (12);
    hintLabel.setBounds (area);
}

void TransportBar::setHintVisible (bool visible)
{
    hintLabel.setVisible (visible);
}
} // namespace focal
