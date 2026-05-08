#include "TransportBar.h"
#include <algorithm>

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
        case Icon::Rewind:
        {
            // Two left-pointing triangles, classic deck glyph. Nudged left
            // so the optical centre matches the disc centre (triangles).
            auto p = iconBox.translated (-iconBox.getWidth() * 0.05f, 0.0f);
            const float halfW = p.getWidth() * 0.5f;
            juce::Path g1, g2;
            g1.addTriangle (p.getX(),           p.getCentreY(),
                              p.getX() + halfW,    p.getY(),
                              p.getX() + halfW,    p.getBottom());
            g2.addTriangle (p.getX() + halfW,    p.getCentreY(),
                              p.getRight(),        p.getY(),
                              p.getRight(),        p.getBottom());
            g.fillPath (g1);
            g.fillPath (g2);
            break;
        }
        case Icon::Forward:
        {
            auto p = iconBox.translated (iconBox.getWidth() * 0.05f, 0.0f);
            const float halfW = p.getWidth() * 0.5f;
            juce::Path g1, g2;
            g1.addTriangle (p.getX(),           p.getY(),
                              p.getX(),           p.getBottom(),
                              p.getX() + halfW,    p.getCentreY());
            g2.addTriangle (p.getX() + halfW,    p.getY(),
                              p.getX() + halfW,    p.getBottom(),
                              p.getRight(),        p.getCentreY());
            g.fillPath (g1);
            g.fillPath (g2);
            break;
        }
    }
}

void TransportIconButton::buttonStateChanged()
{
    juce::Button::buttonStateChanged();
    heldDown = isDown();
}

TransportBar::TransportBar (AudioEngine& engineRef) : engine (engineRef)
{
    playButton.onClick   = [this] { engine.play();   refreshButtonStates(); };
    stopButton.onClick   = [this] { engine.stop();   refreshButtonStates(); };
    recordButton.onClick = [this] { engine.record(); refreshButtonStates(); };

    playButton  .setTooltip ("Play (Space). Right-click for MIDI Learn.");
    stopButton  .setTooltip ("Stop. Right-click for MIDI Learn.");
    recordButton.setTooltip ("Record - arm a track first. Right-click for MIDI Learn.");

    // Right-click on the transport buttons opens the MIDI Learn menu.
    // We listen on each button so the routed mouseDown reaches us with
    // the source component as eventComponent.
    playButton  .addMouseListener (this, false);
    stopButton  .addMouseListener (this, false);
    recordButton.addMouseListener (this, false);

    // REW / FFWD - dual-action.
    //   Brief press (< kHoldThresholdMs):
    //     Stopped -> REW = goto zero, FFWD = goto last record point.
    //     Rolling -> REW = jump to prev marker, FFWD = jump to next marker.
    //   Held: 10x scrub via the timer (drives the playhead while button down).
    rewButton.setTooltip  ("Rewind: hold = 10\xe2\x9c\x95 scrub. Tap = prev marker (rolling) "
                            "or jump to zero (stopped).");
    ffwdButton.setTooltip ("Forward: hold = 10\xe2\x9c\x95 scrub. Tap = next marker (rolling) "
                            "or jump to last record point (stopped).");

    rewButton.onStateChange = [this]
    {
        if (rewButton.isHeldDown())
        {
            if (rewPressedAtMs == 0)   // edge: just pressed
                rewPressedAtMs = juce::Time::currentTimeMillis();
        }
        else if (rewPressedAtMs != 0)
        {
            const auto held = juce::Time::currentTimeMillis() - rewPressedAtMs;
            rewPressedAtMs = 0;
            if (rewIsScrubbing) { rewIsScrubbing = false; }
            else if (held < kHoldThresholdMs)
            {
                if (engine.getTransport().isStopped()) engine.jumpToZero();
                else                                    engine.jumpToPrevMarker();
            }
        }
    };

    ffwdButton.onStateChange = [this]
    {
        if (ffwdButton.isHeldDown())
        {
            if (ffwdPressedAtMs == 0)
                ffwdPressedAtMs = juce::Time::currentTimeMillis();
        }
        else if (ffwdPressedAtMs != 0)
        {
            const auto held = juce::Time::currentTimeMillis() - ffwdPressedAtMs;
            ffwdPressedAtMs = 0;
            if (ffwdIsScrubbing) { ffwdIsScrubbing = false; }
            else if (held < kHoldThresholdMs)
            {
                if (engine.getTransport().isStopped()) engine.jumpToLastRecordPoint();
                else                                    engine.jumpToNextMarker();
            }
        }
    };

    addAndMakeVisible (stopButton);
    addAndMakeVisible (rewButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (ffwdButton);
    addAndMakeVisible (recordButton);

    clockLabel.setJustificationType (juce::Justification::centred);
    clockLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
    clockLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff121214));
    clockLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                        18.0f, juce::Font::bold)));
    clockLabel.setText ("00:00.000", juce::dontSendNotification);
    // Direct numeric location entry. Double-click → text editor; Enter
    // commits via the parser; Esc reverts. Three formats accepted:
    //   1:23 / 1:23.456    minutes : seconds[.ms]
    //   12.3.4 / 12|3|4    bar . beat . subdivision (1-indexed bar/beat,
    //                       subdivision is sixteenths within the beat).
    //   7.5 / 90           bare number = seconds (or ":secondsonly")
    // Bad input reverts the label to the running-time format on the next
    // timer tick, no error dialog (cheap + stays out of the way).
    clockLabel.setEditable (false, true, false);
    clockLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202028));
    clockLabel.setColour (juce::Label::textWhenEditingColourId,        juce::Colours::white);
    clockLabel.setTooltip ("Playhead position. Double-click to type a time "
                            "(e.g. 1:23, 1:23.456, 12.3.4 for bar.beat.sub, "
                            "or a plain number of seconds).");
    clockLabel.onTextChange = [this]
    {
        const auto text = clockLabel.getText().trim();
        const double sr = engine.getCurrentSampleRate();
        if (text.isEmpty() || sr <= 0.0) return;

        // Bar.beat.sub: any '.' or '|' separator and no ':'. Sub is
        // 1-indexed sixteenths within the beat (1..4 in 4/4) for parity
        // with the snap denominations the piano roll uses.
        if (! text.containsChar (':')
            && (text.containsChar ('.') || text.containsChar ('|')))
        {
            auto parts = juce::StringArray::fromTokens (text, ".|", "");
            parts.removeEmptyStrings();
            if (parts.size() >= 1 && parts.size() <= 3)
            {
                const int bar  = juce::jmax (1, parts[0].getIntValue());
                const int beat = parts.size() >= 2 ? juce::jmax (1, parts[1].getIntValue()) : 1;
                const int sub  = parts.size() >= 3 ? juce::jmax (1, parts[2].getIntValue()) : 1;
                const float bpm = engine.getSession().tempoBpm.load (std::memory_order_relaxed);
                const int beatsPerBar = juce::jmax (1, engine.getSession().beatsPerBar.load (std::memory_order_relaxed));
                if (bpm > 0.0f)
                {
                    const double secondsPerBeat = 60.0 / (double) bpm;
                    const double secondsPerSub  = secondsPerBeat / 4.0;   // 16th
                    const double total = (bar - 1) * beatsPerBar * secondsPerBeat
                                       + (beat - 1) * secondsPerBeat
                                       + (sub  - 1) * secondsPerSub;
                    engine.getTransport().setPlayhead (
                        (juce::int64) std::round (total * sr));
                    return;
                }
            }
        }

        // mm:ss[.ms] or :ss[.ms]
        if (text.containsChar (':'))
        {
            const int colon = text.indexOfChar (':');
            const auto minStr = text.substring (0, colon);
            const auto secStr = text.substring (colon + 1);
            const double mins = minStr.isEmpty() ? 0.0 : (double) minStr.getDoubleValue();
            const double secs = (double) secStr.getDoubleValue();
            const auto target = (juce::int64) std::round ((mins * 60.0 + secs) * sr);
            engine.getTransport().setPlayhead (juce::jmax<juce::int64> (0, target));
            return;
        }

        // Bare number = seconds.
        const double secs = (double) text.getDoubleValue();
        const auto target = (juce::int64) std::round (secs * sr);
        engine.getTransport().setPlayhead (juce::jmax<juce::int64> (0, target));
    };
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
    punchToggle.addMouseListener (this, false);
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

    tapButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    tapButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffe0c050));
    tapButton.setTooltip ("Tap to set tempo. Click in time with the music; "
                          "BPM updates after the second tap and averages "
                          "across the most recent few. Two-second silence "
                          "resets the pulse.");
    tapButton.onClick = [this] { onTap(); };
    addAndMakeVisible (tapButton);

    // Jumpback ("« Ns"). Click = rewind by jumpbackSeconds. Right-click =
    // preset menu. Label re-renders whenever the value changes (preset
    // pick) or on session load (the timer's BPM-style refresh).
    jumpbackButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff202024));
    jumpbackButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff70b0e0));
    jumpbackButton.setTooltip ("Jumpback - rewind by the configured number of seconds. "
                                "Right-click to change the amount.");
    jumpbackButton.onClick = [this] { engine.jumpbackBySeconds(); };
    jumpbackButton.addMouseListener (this, false);
    refreshJumpbackLabel();
    addAndMakeVisible (jumpbackButton);

    // TUNE button - opens a modal pitch-tracker overlay on click.
    // Selection of which track to tune lives on MainComponent (it knows
    // the focused track from the TapeStrip selection); this button just
    // toggles the overlay.
    tuneButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff202024));
    tuneButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff70d0a0));
    tuneButton.setTooltip ("Tuner - shows the pitch of the selected track's input. "
                            "Click a track first, then press TUNE.");
    tuneButton.onClick = [this] { if (onTunerToggle) onTunerToggle(); };
    addAndMakeVisible (tuneButton);

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
    // 10x scrub. Once a REW / FFWD button has been held past
    // kHoldThresholdMs, advance the playhead by (sr * kScrubMultiplier *
    // tickPeriod) samples per tick. Continues until the button releases.
    // Direct setPlayhead - scrub does NOT engage the transport so audio
    // stays silent and the audio thread sees a "playhead jumped" event
    // (which fires the existing All Notes Off MIDI flush; safe).
    const auto nowMs = juce::Time::currentTimeMillis();
    const double sr  = engine.getCurrentSampleRate();
    if (sr > 0.0)
    {
        const auto handleScrub = [&] (TransportIconButton& btn,
                                        juce::int64& pressedAt,
                                        bool& scrubbing,
                                        int direction)
        {
            if (! btn.isHeldDown() || pressedAt == 0) return;
            const auto held = nowMs - pressedAt;
            if (held < kHoldThresholdMs) return;
            scrubbing = true;
            // First scrub tick: seed lastScrubTickMs so the delta is the
            // tick period, not "ms since press" (which would teleport on
            // the threshold crossing).
            if (lastScrubTickMs == 0 || lastScrubTickMs < pressedAt)
                lastScrubTickMs = nowMs;
            const auto dtMs = nowMs - lastScrubTickMs;
            lastScrubTickMs = nowMs;
            const auto delta = (juce::int64) ((double) dtMs * 0.001 * sr * kScrubMultiplier);
            const auto cur = engine.getTransport().getPlayhead();
            engine.getTransport().setPlayhead (juce::jmax<juce::int64> (0,
                cur + (juce::int64) direction * delta));
        };
        handleScrub (rewButton,  rewPressedAtMs,  rewIsScrubbing,  -1);
        handleScrub (ffwdButton, ffwdPressedAtMs, ffwdIsScrubbing, +1);
        if (! rewIsScrubbing && ! ffwdIsScrubbing) lastScrubTickMs = 0;
    }

    const auto playhead = engine.getTransport().getPlayhead();
    const double seconds = (sr > 0.0) ? (double) playhead / sr : 0.0;
    const int mins   = (int) (seconds / 60.0);
    const int secs   = (int) seconds % 60;
    const int millis = (int) ((seconds - std::floor (seconds)) * 1000.0);
    // Skip the periodic refresh while the user is mid-edit so we don't
    // stomp their typed input. Same pattern the BPM editor uses below.
    // When MIDI learn is pending, the clock label dims and shows a
    // "MIDI LEARN…" prompt so the user knows the next CC/Note will be
    // captured. Cleared on capture (drained below in the same tick).
    if (! clockLabel.isBeingEdited())
    {
        const int learnTarget = engine.getSession().midiLearnPending.load (std::memory_order_relaxed);
        if (learnTarget >= 0)
        {
            clockLabel.setColour (juce::Label::textColourId, juce::Colour (0xffffd060));
            clockLabel.setText ("MIDI LEARN\xe2\x80\xa6", juce::dontSendNotification);
        }
        else
        {
            clockLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e0));
            clockLabel.setText (juce::String::formatted ("%02d:%02d.%03d", mins, secs, millis),
                                 juce::dontSendNotification);
        }
    }

    refreshButtonStates();

    // Auto-punch post-roll: while recording with punch enabled and the
    // playhead has crossed punchOut + postRoll samples, auto-stop. Done
    // here on the message thread so engine.stop()'s teardown is safe.
    // postRoll == 0 disables the auto-stop (matches the previous behaviour
    // where punch never auto-stopped); punch-disabled also disables it.
    {
        auto& transport = engine.getTransport();
        if (transport.isRecording() && transport.isPunchEnabled())
        {
            const auto pIn  = transport.getPunchIn();
            const auto pOut = transport.getPunchOut();
            const float postRoll = engine.getSession().postRollSeconds.load (std::memory_order_relaxed);
            if (pOut > pIn && postRoll > 0.0f && sr > 0.0)
            {
                const auto stopAt = pOut + (juce::int64) ((double) postRoll * sr);
                if (engine.getTransport().getPlayhead() >= stopAt)
                    engine.stop();
            }
        }
    }

    // Drain the audio-thread queues for the MIDI controller infrastructure.
    // Both are atom-based handoffs - no allocation, no contention.
    {
        auto& s = engine.getSession();

        // Transport-action queue: a binding hit on the audio thread (which
        // can't safely call engine.play/stop/record) pokes the queue; we
        // call the right method here on the message thread, then clear.
        const auto pending = (PendingTransportAction)
            s.pendingTransportAction.exchange ((int) PendingTransportAction::None,
                                                  std::memory_order_relaxed);
        switch (pending)
        {
            case PendingTransportAction::Play:   engine.play();   break;
            case PendingTransportAction::Stop:   engine.stop();   break;
            case PendingTransportAction::Record: engine.record(); break;
            case PendingTransportAction::Toggle:
                if (engine.getTransport().isStopped()) engine.play();
                else                                    engine.stop();
                break;
            case PendingTransportAction::None:   break;
        }

        // Learn capture: when learnPending is set and the audio thread has
        // stamped a captured (channel, dataNumber, trigger), append a
        // binding (replacing any existing one on the same source) and
        // clear both signals. The new binding starts driving its target
        // immediately on the next block.
        const auto learnTarget = s.midiLearnPending.load (std::memory_order_relaxed);
        const auto cap         = s.midiLearnCapture.load (std::memory_order_relaxed);
        if (learnTarget >= 0 && learnCaptureIsValid (cap))
        {
            MidiBinding b;
            b.channel     = unpackLearnCaptureChannel (cap);
            b.dataNumber  = unpackLearnCaptureDataNumber (cap);
            b.trigger     = unpackLearnCaptureTrigger (cap);
            b.target      = unpackLearnTargetKind (learnTarget);
            b.targetIndex = unpackLearnTargetIndex (learnTarget);
            // Drop any existing binding from the same source before
            // appending the new one - prevents stacking duplicate
            // mappings if a user re-learns the same fader.
            auto& binds = s.midiBindings;
            binds.erase (std::remove_if (binds.begin(), binds.end(),
                [&] (const MidiBinding& x)
                {
                    return x.sourceMatches (b.channel, b.dataNumber, b.trigger);
                }), binds.end());
            binds.push_back (b);
            s.midiLearnCapture.store (0, std::memory_order_relaxed);
            s.midiLearnPending.store (-1, std::memory_order_relaxed);
        }
    }
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

    // On macOS, the OS traffic-light buttons sit at the top-left of the window
    // when not in fullscreen and overlap the leftmost transport button. Reserve
    // space so Stop is fully visible. In fullscreen the lights are gone and the
    // pad collapses to zero. Querying the peer here works because resized() is
    // re-invoked whenever the window enters / leaves fullscreen.
   #if JUCE_MAC
    int macTitlePad = 78;
    if (auto* top = getTopLevelComponent())
        if (auto* peer = top->getPeer())
            if (peer->isFullScreen() || peer->isKioskMode())
                macTitlePad = 0;
    area.removeFromLeft (macTitlePad);
   #endif

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
    place (rewButton);
    place (playButton);
    place (ffwdButton);
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
    tapButton.setBounds  (area.removeFromRight (40).reduced (1, 4));
    area.removeFromRight (4);
    bpmValue.setBounds   (area.removeFromRight (52).reduced (1, 4));
    bpmCaption.setBounds (area.removeFromRight (32).reduced (1, 4));
    area.removeFromRight (8);
    jumpbackButton.setBounds (area.removeFromRight (54).reduced (1, 4));
    area.removeFromRight (4);
    tuneButton.setBounds     (area.removeFromRight (50).reduced (1, 4));
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

void TransportBar::mouseDown (const juce::MouseEvent& e)
{
    // Right-click on the jumpback button opens its preset menu. Other
    // sources fall through to the default Component handling - we only
    // listen on jumpbackButton so this catches both children-routed
    // events and direct hits on the bar background near the button.
    if (e.eventComponent == &jumpbackButton && e.mods.isPopupMenu())
    {
        showJumpbackMenu();
        return;
    }
    // Right-click on PUNCH opens the auto-punch settings (pre-roll /
    // post-roll). Plain click still toggles punch on/off.
    if (e.eventComponent == &punchToggle && e.mods.isPopupMenu())
    {
        showPunchSettingsMenu();
        return;
    }
    // Right-click on the transport buttons opens the MIDI Learn menu.
    if (e.mods.isPopupMenu())
    {
        if (e.eventComponent == &playButton)
        {
            midilearn::showLearnMenu (playButton, engine.getSession(),
                                        MidiBindingTarget::TransportPlay);
            return;
        }
        if (e.eventComponent == &stopButton)
        {
            midilearn::showLearnMenu (stopButton, engine.getSession(),
                                        MidiBindingTarget::TransportStop);
            return;
        }
        if (e.eventComponent == &recordButton)
        {
            midilearn::showLearnMenu (recordButton, engine.getSession(),
                                        MidiBindingTarget::TransportRecord);
            return;
        }
    }
    juce::Component::mouseDown (e);
}

void TransportBar::refreshJumpbackLabel()
{
    const float secs = engine.getSession().jumpbackSeconds.load (std::memory_order_relaxed);
    // Whole-second values render without a decimal; fractional ones get
    // a single decimal place. "« 5s" / "« 7.5s".
    const auto numeric = (std::abs (secs - std::round (secs)) < 0.05f)
                        ? juce::String ((int) std::round (secs))
                        : juce::String (secs, 1);
    jumpbackButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc2\xab ")) + numeric + "s");
}

void TransportBar::showJumpbackMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Jumpback amount");
    static constexpr float kPresets[] = { 1.0f, 3.0f, 5.0f, 10.0f, 15.0f, 30.0f };
    const float current = engine.getSession().jumpbackSeconds.load (std::memory_order_relaxed);
    for (const float v : kPresets)
    {
        const auto label = (std::abs (v - std::round (v)) < 0.05f)
                            ? juce::String ((int) std::round (v))
                            : juce::String (v, 1);
        m.addItem (label + " seconds", true,
                    std::abs (current - v) < 0.05f,
                    [this, v]
                    {
                        engine.getSession().jumpbackSeconds.store (v, std::memory_order_relaxed);
                        refreshJumpbackLabel();
                    });
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&jumpbackButton));
}

void TransportBar::showPunchSettingsMenu()
{
    juce::PopupMenu m;
    auto& s = engine.getSession();
    const float pre  = s.preRollSeconds.load (std::memory_order_relaxed);
    const float post = s.postRollSeconds.load (std::memory_order_relaxed);

    auto formatSecs = [] (float v)
    {
        if (v <= 0.0f) return juce::String ("Off");
        const auto numeric = (std::abs (v - std::round (v)) < 0.05f)
                                ? juce::String ((int) std::round (v))
                                : juce::String (v, 1);
        return numeric + " s";
    };

    static constexpr float kPresets[] = { 0.0f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f };

    juce::PopupMenu preMenu;
    for (const float v : kPresets)
        preMenu.addItem (formatSecs (v), true, std::abs (pre - v) < 0.05f,
            [&s, v] { s.preRollSeconds.store  (v, std::memory_order_relaxed); });

    juce::PopupMenu postMenu;
    for (const float v : kPresets)
        postMenu.addItem (formatSecs (v), true, std::abs (post - v) < 0.05f,
            [&s, v] { s.postRollSeconds.store (v, std::memory_order_relaxed); });

    m.addSectionHeader ("Auto-punch");
    m.addItem ("Roll IN before punch (audible context)", false, false, []{});
    m.addSubMenu ("Pre-roll: "  + formatSecs (pre),  preMenu);
    m.addItem ("Stop after punch-out (auto-stop)",     false, false, []{});
    m.addSubMenu ("Post-roll: " + formatSecs (post), postMenu);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&punchToggle));
}

void TransportBar::onTap()
{
    // Stamp this tap. If the previous tap was longer than the timeout
    // ago, treat this as the start of a new pulse and reset the ring.
    // Otherwise append to the ring (oldest stamp falls off).
    const auto nowMs = juce::Time::currentTimeMillis();
    if (tapStampCount > 0)
    {
        const auto& lastStamp = tapStamps[(size_t) (tapStampCount - 1) % (size_t) kTapWindow];
        if (nowMs - lastStamp > kTapTimeoutMs)
            tapStampCount = 0;
    }

    if (tapStampCount < kTapWindow)
    {
        tapStamps[(size_t) tapStampCount] = nowMs;
        ++tapStampCount;
    }
    else
    {
        // Shift left by one - drop oldest, append newest. Small N so a
        // memmove-style shift is fine; no allocation.
        for (int i = 1; i < kTapWindow; ++i)
            tapStamps[(size_t) (i - 1)] = tapStamps[(size_t) i];
        tapStamps[(size_t) (kTapWindow - 1)] = nowMs;
    }

    if (tapStampCount < 2) return;   // need at least one interval

    // Average inter-tap interval over the active window. The session BPM
    // is clamped to the same 30..300 range as the manual editor.
    const int intervals = tapStampCount - 1;
    juce::int64 sumMs = 0;
    for (int i = 1; i < tapStampCount; ++i)
        sumMs += tapStamps[(size_t) i] - tapStamps[(size_t) (i - 1)];
    const double avgMs = (double) sumMs / (double) intervals;
    if (avgMs <= 0.0) return;
    const float bpm = juce::jlimit (30.0f, 300.0f, (float) (60000.0 / avgMs));
    engine.getSession().tempoBpm.store (bpm);
    bpmValue.setText (juce::String ((int) bpm), juce::dontSendNotification);
}
} // namespace focal
