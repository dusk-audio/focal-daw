#include "AudioRegionEditor.h"
#include "../engine/AudioEngine.h"
#include "../engine/Transport.h"
#include "../session/RegionEditActions.h"
#include <cmath>
#include <limits>

namespace focal
{
namespace
{
const juce::Colour kBgDark        { 0xff181820 };
const juce::Colour kHeaderBg      { 0xff202028 };
const juce::Colour kHeaderText    { 0xffb0b0b8 };
const juce::Colour kBarLine       { 0xff5a5a64 };
const juce::Colour kBeatLine      { 0xff3c3c46 };
const juce::Colour kWaveformFill  { 0xff80b0e0 };
const juce::Colour kFadeStroke    { 0xffe0c060 };
const juce::Colour kEditCursor    { 0xffffd060 };
// Soft cream so it reads against the yellow edit cursor without
// competing — same colour the piano roll uses for its transport line.
const juce::Colour kTransportPlayhead { 0xffffe8c0 };

// Region-properties popup colour palette. Mirrors the palette in
// [src/ui/TapeStrip.cpp] so all three surfaces (tape strip menu,
// audio editor properties, piano roll properties) share the same
// 8 swatches + reset.
struct PaletteEntry { const char* label; juce::uint32 argb; };
constexpr PaletteEntry kPalette[] = {
    { "Reset to track colour", 0x00000000 },
    { "Red",     0xffd05f5f }, { "Orange",  0xffd09060 },
    { "Yellow",  0xffd0c060 }, { "Green",   0xff60c070 },
    { "Cyan",    0xff60c0c0 }, { "Blue",    0xff6090d0 },
    { "Purple",  0xff9070c0 }, { "Magenta", 0xffc060a0 },
};
constexpr int kPaletteCount = (int) (sizeof (kPalette) / sizeof (kPalette[0]));
} // namespace

AudioRegionEditor::AudioRegionEditor (Session& s, AudioEngine& e, int t, int r)
    : session (s), engine (e), trackIdx (t), regionIdx (r)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    formatManager.registerBasicFormats();

    // Top icon row.
    auto wireIcon = [this] (IconButton& b, const juce::String& tip,
                              std::function<void()> onClick)
    {
        addAndMakeVisible (b);
        b.setTooltip (tip);
        b.onClick = std::move (onClick);
    };
    wireIcon (undoButton,       "Undo",                [this] { engine.getUndoManager().undo(); refreshStatusBarReadouts(); repaint(); });
    wireIcon (redoButton,       "Redo",                [this] { engine.getUndoManager().redo(); refreshStatusBarReadouts(); repaint(); });
    wireIcon (splitButton,      "Split at edit cursor",[this] { splitAtCursor(); });
    wireIcon (normalizeButton,  "Normalize",           [this] { normalizeRegion(); });
    wireIcon (reverseButton,    "Reverse",             [this] { reverseRegion(); });
    wireIcon (takeCycleButton,  "Cycle take",          [this] { cycleTake(); });
    wireIcon (zoomFitButton,    "Zoom to fit region",  [this] { zoomFit(); });
    wireIcon (propertiesButton, "Region properties...", [this] { showRegionPropertiesPopup(); });

    // Bottom status-bar readouts.
    auto styleReadout = [] (juce::Label& l, juce::Justification just)
    {
        l.setJustificationType (just);
        l.setColour (juce::Label::textColourId,       juce::Colour (0xffd0d0d8));
        l.setColour (juce::Label::backgroundColourId, juce::Colour (0xff181820));
        l.setColour (juce::Label::outlineColourId,    juce::Colour (0xff3a3a44));
        l.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        l.setEditable (false, false, false);
    };
    styleReadout (positionLabel, juce::Justification::centredLeft);
    styleReadout (gainLabel,     juce::Justification::centredLeft);
    styleReadout (fadeLabel,     juce::Justification::centredLeft);
    styleReadout (infoLabel,     juce::Justification::centredRight);

    // Make gain + fade click-to-edit. Double-click enters edit mode;
    // Enter commits, Esc reverts. Parse is liberal: strips trailing
    // " dB" / " ms" / " / N ms" so the user can retype the displayed
    // string verbatim. Bad input reverts on the next status refresh.
    gainLabel.setEditable (false, true, false);
    gainLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202028));
    gainLabel.setColour (juce::Label::textWhenEditingColourId,        juce::Colours::white);
    gainLabel.setTooltip ("Double-click to type a dB value (range -24 to +12).");
    gainLabel.onTextChange = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        // Strip non-numeric tail so "3.5 dB" parses to 3.5.
        auto txt = gainLabel.getText().trim().retainCharacters ("0123456789.-+");
        if (txt.isEmpty()) { refreshStatusBarReadouts(); return; }
        const float target = juce::jlimit (-24.0f, 12.0f, (float) txt.getDoubleValue());
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.gainDb = target;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Set region gain");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };

    fadeLabel.setEditable (false, true, false);
    fadeLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202028));
    fadeLabel.setColour (juce::Label::textWhenEditingColourId,        juce::Colours::white);
    fadeLabel.setTooltip ("Double-click to type \"IN / OUT\" fade lengths in ms "
                            "(e.g. \"50 / 200\"). Single value sets fade-in only.");
    fadeLabel.onTextChange = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
        // Accept "IN / OUT" or "IN" (single number = fade-in only).
        // Slash, comma, or "fade" prefix all tolerated so the user can
        // retype the displayed "fade 50 / 200 ms" verbatim.
        auto raw = fadeLabel.getText().trim().toLowerCase()
                        .replace ("fade", "").replace ("ms", "");
        const int slash = raw.indexOfChar ('/');
        const auto inStr  = (slash < 0) ? raw : raw.substring (0, slash);
        const auto outStr = (slash < 0) ? juce::String() : raw.substring (slash + 1);
        const double inMs  = juce::jmax (0.0, inStr.trim().getDoubleValue());
        const double outMs = juce::jmax (0.0, outStr.trim().getDoubleValue());
        const AudioRegion before = *r;
        AudioRegion after = before;
        const auto maxLen = juce::jmax<juce::int64> (0, r->lengthInSamples);
        after.fadeInSamples  = juce::jlimit<juce::int64> (0, maxLen,
                                                              (juce::int64) std::round (inMs * sr / 1000.0));
        after.fadeOutSamples = juce::jlimit<juce::int64> (0, juce::jmax<juce::int64> (0, maxLen - after.fadeInSamples),
                                                              (juce::int64) std::round (outMs * sr / 1000.0));
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Set region fades");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };

    // Take counter next to the TakeCycle toolbar button. Small label,
    // styled like the other readouts but transparent background so it
    // sits inside the toolbar band cleanly.
    takeReadout.setJustificationType (juce::Justification::centredLeft);
    takeReadout.setColour (juce::Label::textColourId, juce::Colour (0xff9090a0));
    takeReadout.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    takeReadout.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (takeReadout);

    // Title line above the bar ruler. Shows region label when set,
    // falling back to the source WAV filename. Editable so the user
    // can rename without opening the Properties popup.
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d8));
    titleLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff181820));
    titleLabel.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
    titleLabel.setEditable (false, true, false);
    titleLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202028));
    titleLabel.setColour (juce::Label::textWhenEditingColourId,        juce::Colours::white);
    titleLabel.setTooltip ("Double-click to rename the region. Empty = use filename.");
    titleLabel.onTextChange = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        auto txt = titleLabel.getText().trim();
        // Don't let the filename fallback persist as the label - if
        // the user accepts the displayed filename verbatim, treat it
        // as "no label" so the fallback continues to track file rename.
        if (txt == r->file.getFileName()) txt = {};
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.label = txt;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Rename region");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };
    addAndMakeVisible (titleLabel);

    addAndMakeVisible (positionLabel);
    addAndMakeVisible (gainLabel);
    addAndMakeVisible (fadeLabel);
    addAndMakeVisible (infoLabel);

    muteToggle.setButtonText ("Mute");
    lockToggle.setButtonText ("Lock");
    auto styleToggle = [] (juce::ToggleButton& t)
    {
        t.setColour (juce::ToggleButton::textColourId, juce::Colour (0xffd0d0d8));
    };
    styleToggle (muteToggle);
    styleToggle (lockToggle);
    muteToggle.onClick = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.muted = muteToggle.getToggleState();
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (after.muted ? "Mute region" : "Unmute region");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };
    lockToggle.onClick = [this]
    {
        auto* r = region();
        if (r == nullptr) return;
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.locked = lockToggle.getToggleState();
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (after.locked ? "Lock region" : "Unlock region");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
    };
    addAndMakeVisible (muteToggle);
    addAndMakeVisible (lockToggle);

    refreshStatusBarReadouts();

    // 30 Hz playhead poll - matches the meter cadence elsewhere. The
    // tick is cheap when transport is stopped (no repaint) so leaving
    // it running unconditionally keeps the start-of-playback latency
    // at one tick (~33 ms) without burning CPU at idle.
    startTimerHz (30);
}

AudioRegionEditor::~AudioRegionEditor()
{
    stopTimer();
    // ChangeListener removal is handled by AudioThumbnail's destructor
    // (it removes its listeners on the broadcast list it owns).
    if (thumb != nullptr) thumb->removeChangeListener (this);
}

AudioRegion* AudioRegionEditor::region()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    auto& v = session.track (trackIdx).regions;
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

const AudioRegion* AudioRegionEditor::region() const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    const auto& v = session.track (trackIdx).regions;
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

void AudioRegionEditor::rebuildThumbIfNeeded()
{
    const auto* r = region();
    if (r == nullptr) return;
    if (thumb != nullptr && r->file == loadedFile) return;

    if (thumb != nullptr) thumb->removeChangeListener (this);
    thumb = std::make_unique<juce::AudioThumbnail> (512, formatManager, thumbCache);
    if (r->file.existsAsFile())
        thumb->setSource (new juce::FileInputSource (r->file));
    else
        thumb->setSource (nullptr);
    thumb->addChangeListener (this);
    loadedFile = r->file;
}

void AudioRegionEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::resized()
{
    layoutIconRow   (juce::Rectangle<int> (0, 0, getWidth(), kIconRowHeight));
    layoutStatusBar (juce::Rectangle<int> (0, getHeight() - kStatusBarH,
                                                getWidth(), kStatusBarH));

    const auto* r = region();
    if (r == nullptr) return;
    const auto waveArea = juce::Rectangle<int> (
        0, kIconRowHeight + kRulerHeight,
        getWidth(),
        getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);
    zoomFitToArea (waveArea);
}

void AudioRegionEditor::zoomFitToArea (juce::Rectangle<int> area)
{
    const auto* r = region();
    if (r == nullptr || r->lengthInSamples <= 0) return;
    // Pin the anchor range to the focused region's CURRENT timeline
    // span. Splits later mutate region.lengthInSamples but anchor
    // stays put -> the waveform doesn't shift on screen, cuts just
    // add visible boundary lines.
    anchorTimelineStart  = r->timelineStart;
    anchorTimelineLength = r->lengthInSamples;
    const float w = (float) juce::jmax (1, area.getWidth() - 8);
    pixelsPerSample = w / (float) anchorTimelineLength;
    scrollSamples   = 0;
    editCursorSample = r->sourceOffset;
}

int AudioRegionEditor::xForTimelineSample (juce::int64 timelineSample,
                                              juce::Rectangle<int> area) const
{
    const auto rel = timelineSample - anchorTimelineStart - scrollSamples;
    return area.getX() + 4 + (int) std::round ((double) rel * pixelsPerSample);
}

juce::int64 AudioRegionEditor::timelineSampleForX (int x, juce::Rectangle<int> area) const
{
    if (pixelsPerSample <= 0.0f) return anchorTimelineStart;
    const auto rel = (juce::int64) std::round (
        (double) (x - area.getX() - 4) / pixelsPerSample);
    return anchorTimelineStart + scrollSamples + rel;
}

int AudioRegionEditor::xForSample (juce::int64 absSample,
                                      juce::Rectangle<int> area) const
{
    // File-sample form: convert through the focused region's
    // sourceOffset -> timelineStart mapping, then forward to the
    // timeline-domain primitive. Keeps existing call sites (fade
    // discs, trim strips, edit cursor) working unchanged.
    const auto* r = region();
    if (r == nullptr) return area.getX();
    const auto t = r->timelineStart + (absSample - r->sourceOffset);
    return xForTimelineSample (t, area);
}

juce::int64 AudioRegionEditor::sampleForX (int x, juce::Rectangle<int> area) const
{
    const auto* r = region();
    if (r == nullptr) return 0;
    const auto t = timelineSampleForX (x, area);
    const auto fileSample = r->sourceOffset + (t - r->timelineStart);
    return juce::jlimit<juce::int64> (
        r->sourceOffset,
        r->sourceOffset + r->lengthInSamples,
        fileSample);
}

int AudioRegionEditor::regionIndexAtX (int x, juce::Rectangle<int> area) const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return -1;
    const auto t = timelineSampleForX (x, area);
    const auto& regs = session.track (trackIdx).regions;
    for (int i = 0; i < (int) regs.size(); ++i)
    {
        const auto& reg = regs[(size_t) i];
        if (t >= reg.timelineStart
            && t < reg.timelineStart + reg.lengthInSamples)
            return i;
    }
    return -1;
}

void AudioRegionEditor::paint (juce::Graphics& g)
{
    rebuildThumbIfNeeded();

    g.fillAll (kBgDark);

    // Top icon row band - flat fill; icons paint themselves.
    const auto iconRowArea = juce::Rectangle<int> (0, 0, getWidth(), kIconRowHeight);
    g.setColour (juce::Colour (0xff20202c));
    g.fillRect (iconRowArea);
    g.setColour (kBarLine);
    g.drawHorizontalLine (iconRowArea.getBottom() - 1,
                            (float) iconRowArea.getX(), (float) iconRowArea.getRight());

    const auto rulerArea = juce::Rectangle<int> (0, kIconRowHeight,
                                                    getWidth(), kRulerHeight);
    const auto waveArea  = juce::Rectangle<int> (
        0, kIconRowHeight + kRulerHeight,
        getWidth(),
        getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);

    // Bottom status-bar band background - children paint over.
    const auto statusArea = juce::Rectangle<int> (
        0, getHeight() - kStatusBarH, getWidth(), kStatusBarH);
    g.setColour (juce::Colour (0xff181820));
    g.fillRect (statusArea);
    g.setColour (kBarLine);
    g.drawHorizontalLine (statusArea.getY(),
                            (float) statusArea.getX(), (float) statusArea.getRight());

    paintRuler         (g, rulerArea);
    paintWaveform      (g, waveArea);
    paintFadeEnvelopes (g, waveArea);

    // Visible drag affordances - painted under the edit cursor so the
    // cursor always wins on top. Handles are intentionally bold against
    // the dark waveform background; subtle styling reads as "decoration".
    if (region() != nullptr)
    {
        // Gain line - 2 px green horizontal, full width of wave area.
        const int gy = gainLineY (waveArea);
        g.setColour (juce::Colour (0xff80e070));
        g.fillRect (waveArea.getX() + 4, gy - 1,
                     waveArea.getWidth() - 8, 2);
        // Centre drag chip - oversized so the user has a clear grab target.
        g.setColour (juce::Colour (0xff80ff70));
        g.fillRoundedRectangle ((float) (waveArea.getCentreX() - 22),
                                   (float) (gy - 5), 44.0f, 10.0f, 2.0f);
        g.setColour (juce::Colour (0xff182018));
        g.drawRoundedRectangle ((float) (waveArea.getCentreX() - 22),
                                   (float) (gy - 5), 44.0f, 10.0f, 2.0f, 1.2f);
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        const auto db = juce::String (region()->gainDb, 1) + " dB";
        g.drawText (db, waveArea.getCentreX() - 22, gy - 6, 44, 12,
                     juce::Justification::centred, false);

        // Fade-in handle - 18 px yellow disc at the current fadeIn end.
        // Painted as filled ellipse with a strong dark outline so it
        // pops against the waveform.
        auto paintFadeDisc = [&] (juce::Rectangle<int> r)
        {
            if (r.isEmpty()) return;
            const auto rf = r.toFloat();
            g.setColour (juce::Colour (0xffffd040));
            g.fillEllipse (rf);
            g.setColour (juce::Colour (0xff181820));
            g.drawEllipse (rf, 2.0f);
        };
        paintFadeDisc (fadeInHandleRect  (waveArea));
        paintFadeDisc (fadeOutHandleRect (waveArea));

        // Trim grab strips - subtle 3 px bands at the focused slice's
        // edges. Reaper-style: barely visible until you hover them so
        // the slice boundary doesn't look like a hard wall. Hit-test
        // rect stays the original 8 px width (see trimStartRect /
        // trimEndRect) so the grab target isn't tiny.
        g.setColour (juce::Colour (0xff707078).withAlpha (0.5f));
        {
            auto ts = trimStartRect (waveArea);
            auto te = trimEndRect   (waveArea);
            ts.setWidth (3); ts.translate (3, 0);
            te.setWidth (3); te.translate (2, 0);
            g.fillRect (ts);
            g.fillRect (te);
        }
    }

    // Range band painted BELOW the cursor / playhead so the
    // active edit indicators stay visible on top.
    if (rangeActive && region() != nullptr)
    {
        const int xa = xForSample (juce::jmin (rangeStartSample, rangeEndSample), waveArea);
        const int xb = xForSample (juce::jmax (rangeStartSample, rangeEndSample), waveArea);
        if (xb > xa)
        {
            g.setColour (juce::Colour (0xffffd060).withAlpha (0.18f));
            g.fillRect (xa, waveArea.getY(), xb - xa, waveArea.getHeight());
            g.setColour (juce::Colour (0xffffd060).withAlpha (0.6f));
            g.drawVerticalLine (xa, (float) waveArea.getY(), (float) waveArea.getBottom());
            g.drawVerticalLine (xb, (float) waveArea.getY(), (float) waveArea.getBottom());
        }
    }
    paintEditCursor    (g, waveArea);
    paintTransportPlayhead (g, waveArea);

    // Stale-region guard: if the indices fell out of range (e.g. user
    // deleted the region while the editor was open), paint a hint.
    if (region() == nullptr)
    {
        g.setColour (kHeaderText);
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::plain)));
        g.drawText ("region unavailable", getLocalBounds(),
                     juce::Justification::centred, false);
    }
}

void AudioRegionEditor::paintRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (kHeaderBg);
    g.fillRect (area);
    g.setColour (kBarLine);
    g.drawHorizontalLine (area.getBottom() - 1,
                            (float) area.getX(), (float) area.getRight());

    const auto* r = region();
    if (r == nullptr || r->lengthInSamples <= 0) return;
    const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());

    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));

    const auto mode = (TimeDisplayMode) session.timeDisplayMode.load (std::memory_order_relaxed);
    if (mode == TimeDisplayMode::Bars)
    {
        const double bpm = juce::jmax (1.0, (double) session.tempoBpm.load (std::memory_order_relaxed));
        const int beatsPerBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const double samplesPerBeat = sr * 60.0 / bpm;
        const double samplesPerBar  = samplesPerBeat * beatsPerBar;

        // Iterate bars across the WHOLE anchor range so splits inside
        // the editor still paint a continuous ruler. anchorTimeline*
        // is captured at open and stays fixed under splits.
        const double anchorStart = (double) anchorTimelineStart;
        const double anchorEnd   = anchorStart + (double) anchorTimelineLength;
        const double firstBar = std::floor (anchorStart / samplesPerBar);
        const double lastBar  = std::ceil  (anchorEnd   / samplesPerBar);

        for (double bar = firstBar; bar <= lastBar; bar += 1.0)
        {
            const auto barTimeline = (juce::int64) std::round (bar * samplesPerBar);
            if (barTimeline < anchorTimelineStart
                || barTimeline > anchorTimelineStart + anchorTimelineLength) continue;
            const int x = xForTimelineSample (barTimeline, area);
            if (x < area.getX() || x > area.getRight()) continue;
            g.setColour (kBarLine);
            g.drawVerticalLine (x, (float) area.getY(), (float) area.getBottom());
            g.setColour (kHeaderText);
            g.drawText (juce::String ((int) bar + 1),
                         juce::Rectangle<int> (x + 3, area.getY(), 40, area.getHeight()),
                         juce::Justification::centredLeft, false);
        }
    }
    else
    {
        // Time mode - tick across whole anchor range, same density
        // the tape strip uses (1 / 5 / 10 / 30 s steps).
        const double pxPerSec = (double) pixelsPerSample * sr;
        double tickEverySec = 1.0;
        if      (pxPerSec < 6.0)  tickEverySec = 30.0;
        else if (pxPerSec < 16.0) tickEverySec = 10.0;
        else if (pxPerSec < 40.0) tickEverySec = 5.0;

        const double anchorStartSec = (double) anchorTimelineStart / sr;
        const double anchorEndSec   = anchorStartSec + (double) anchorTimelineLength / sr;
        const double firstSec = std::floor (anchorStartSec / tickEverySec) * tickEverySec;

        for (double sec = firstSec; sec <= anchorEndSec; sec += tickEverySec)
        {
            const auto secTimeline = (juce::int64) std::round (sec * sr);
            if (secTimeline < anchorTimelineStart
                || secTimeline > anchorTimelineStart + anchorTimelineLength) continue;
            const int x = xForTimelineSample (secTimeline, area);
            if (x < area.getX() || x > area.getRight()) continue;
            g.setColour (kBarLine);
            g.drawVerticalLine (x, (float) area.getY(), (float) area.getBottom());
            g.setColour (kHeaderText);
            const int mins = (int) (sec / 60.0);
            const int secs = (int) sec % 60;
            g.drawText (juce::String::formatted ("%d:%02d", mins, secs),
                         juce::Rectangle<int> (x + 3, area.getY(), 60, area.getHeight()),
                         juce::Justification::centredLeft, false);
        }
    }
}

void AudioRegionEditor::paintWaveform (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto* focused = region();
    if (focused == nullptr || thumb == nullptr) return;

    const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
    auto inset = area.reduced (4, 4);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (inset);

    // Neighborhood view: iterate every region on this track and paint
    // any whose timeline span intersects the anchor range. Slices
    // sharing the focused file render via the cached thumbnail at
    // their own sourceOffset / length. The focused slice gets full
    // brightness; the others are dimmed so the user can still see
    // them but knows what edits apply to.
    const auto anchorEnd = anchorTimelineStart + anchorTimelineLength;
    const auto& regs = session.track (trackIdx).regions;

    for (int i = 0; i < (int) regs.size(); ++i)
    {
        const auto& reg = regs[(size_t) i];
        if (reg.lengthInSamples <= 0) continue;
        const auto regEnd = reg.timelineStart + reg.lengthInSamples;
        if (regEnd <= anchorTimelineStart || reg.timelineStart >= anchorEnd) continue;
        if (reg.file != loadedFile) continue;  // different audio source, can't share thumbnail

        const int xa = xForTimelineSample (reg.timelineStart, area);
        const int xb = xForTimelineSample (regEnd, area);
        if (xb <= xa) continue;

        const auto slice = juce::Rectangle<int> (
            juce::jmax (xa, inset.getX()),
            inset.getY(),
            juce::jmin (xb, inset.getRight()) - juce::jmax (xa, inset.getX()),
            inset.getHeight());
        if (slice.getWidth() <= 0) continue;

        // Per-slice background tint so unfocused slices read as
        // separate clips.
        const bool isFocused = (i == regionIdx);
        if (! isFocused)
        {
            g.setColour (juce::Colour (0xff181820).withAlpha (0.6f));
            g.fillRect (slice);
        }

        const auto sliceColour = (reg.customColour.isTransparent()
                                      ? kWaveformFill
                                      : reg.customColour);
        g.setColour (sliceColour.withMultipliedBrightness (isFocused ? 1.05f : 0.55f)
                                  .withAlpha (isFocused ? 1.0f : 0.85f));

        if (thumb->isFullyLoaded() || thumb->getNumChannels() > 0)
        {
            const double t0 = (double) reg.sourceOffset / sr;
            const double t1 = t0 + (double) reg.lengthInSamples / sr;
            // Clip so we only draw inside the slice's x-range even when
            // it overflows the inset (e.g. region partially outside the
            // anchor window).
            juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (slice);
            thumb->drawChannels (g,
                                  juce::Rectangle<int> (xa, inset.getY(),
                                                          xb - xa, inset.getHeight()),
                                  t0, t1, 0.95f);
        }
    }

    // Slice boundary lines: subtle dark stroke at each region's edge.
    // Reaper-style — visible enough to identify a cut but not so loud
    // it competes with the waveform itself.
    g.setColour (juce::Colour (0xff404048).withAlpha (0.55f));
    for (const auto& reg : regs)
    {
        if (reg.lengthInSamples <= 0) continue;
        if (reg.file != loadedFile) continue;
        const auto regEnd = reg.timelineStart + reg.lengthInSamples;
        if (reg.timelineStart > anchorTimelineStart
            && reg.timelineStart < anchorEnd)
        {
            const int x = xForTimelineSample (reg.timelineStart, area);
            g.drawVerticalLine (x, (float) inset.getY(), (float) inset.getBottom());
        }
        if (regEnd > anchorTimelineStart && regEnd < anchorEnd)
        {
            const int x = xForTimelineSample (regEnd, area);
            g.drawVerticalLine (x, (float) inset.getY(), (float) inset.getBottom());
        }
    }

    // Centre rule (zero amplitude reference).
    g.setColour (kBeatLine.withAlpha (0.6f));
    g.drawHorizontalLine (inset.getCentreY(),
                            (float) inset.getX(), (float) inset.getRight());
}

void AudioRegionEditor::paintFadeEnvelopes (juce::Graphics& g,
                                              juce::Rectangle<int> area)
{
    const auto* r = region();
    if (r == nullptr || r->lengthInSamples <= 0) return;
    auto inset = area.reduced (4, 4);

    g.setColour (kFadeStroke.withAlpha (0.9f));

    if (r->fadeInSamples > 0)
    {
        const int xStart = xForSample (r->sourceOffset, area);
        const int xEnd   = xForSample (r->sourceOffset + r->fadeInSamples, area);
        juce::Path p;
        p.startNewSubPath ((float) xStart, (float) inset.getBottom());
        p.lineTo          ((float) xEnd,   (float) inset.getY());
        g.strokePath (p, juce::PathStrokeType (1.4f));
    }
    if (r->fadeOutSamples > 0)
    {
        const int xStart = xForSample (r->sourceOffset + r->lengthInSamples - r->fadeOutSamples, area);
        const int xEnd   = xForSample (r->sourceOffset + r->lengthInSamples, area);
        juce::Path p;
        p.startNewSubPath ((float) xStart, (float) inset.getY());
        p.lineTo          ((float) xEnd,   (float) inset.getBottom());
        g.strokePath (p, juce::PathStrokeType (1.4f));
    }
}

void AudioRegionEditor::paintEditCursor (juce::Graphics& g,
                                            juce::Rectangle<int> area)
{
    const int cx = xForSample (editCursorSample, area);
    if (cx < area.getX() - 1 || cx > area.getRight() + 1) return;

    g.setColour (kEditCursor.withAlpha (0.7f));
    g.drawVerticalLine (cx, (float) area.getY(), (float) area.getBottom());

    juce::Path tri;
    tri.addTriangle ((float) cx - 4.0f, (float) area.getY(),
                       (float) cx + 4.0f, (float) area.getY(),
                       (float) cx,         (float) area.getY() + 5.0f);
    g.setColour (kEditCursor);
    g.fillPath (tri);
}

// ── Handle hit-test rects ────────────────────────────────────────────
// All rects are in screen coords inside the waveform area. Each handle
// gets a generous grab slop wider than the painted glyph (forgiving for
// mouse precision, especially at low pixelsPerSample).
juce::Rectangle<int> AudioRegionEditor::fadeInHandleRect (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return {};
    const int x = xForSample (r->sourceOffset + r->fadeInSamples, waveArea);
    return juce::Rectangle<int> (x - 9, waveArea.getY() + 4, 18, 18);
}

juce::Rectangle<int> AudioRegionEditor::fadeOutHandleRect (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return {};
    const int x = xForSample (r->sourceOffset + r->lengthInSamples - r->fadeOutSamples, waveArea);
    return juce::Rectangle<int> (x - 9, waveArea.getY() + 4, 18, 18);
}

juce::Rectangle<int> AudioRegionEditor::trimStartRect (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return {};
    const int x = xForSample (r->sourceOffset, waveArea);
    return juce::Rectangle<int> (x - 4, waveArea.getY() + 18,
                                    8, waveArea.getHeight() - 22);
}

juce::Rectangle<int> AudioRegionEditor::trimEndRect (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return {};
    const int x = xForSample (r->sourceOffset + r->lengthInSamples, waveArea);
    return juce::Rectangle<int> (x - 4, waveArea.getY() + 18,
                                    8, waveArea.getHeight() - 22);
}

int AudioRegionEditor::gainLineY (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr) return waveArea.getCentreY();
    // 0 dB sits at the vertical centre. Positive gain raises the line
    // (smaller y); negative drops it. Range: ±24 dB across half the
    // waveform area's height.
    const auto inset = waveArea.reduced (4, 4);
    const float spanPx = (float) inset.getHeight() * 0.5f;
    const float frac = juce::jlimit (-24.0f, 12.0f, r->gainDb) / 24.0f;
    return inset.getCentreY() - (int) std::round (frac * spanPx);
}

void AudioRegionEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);

    if (fadeInHandleRect (waveArea).contains (e.x, e.y)
        || fadeOutHandleRect (waveArea).contains (e.x, e.y))
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
    if (trimStartRect (waveArea).contains (e.x, e.y)
        || trimEndRect (waveArea).contains (e.x, e.y))
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
    // Within ±4 px of the gain line → vertical-resize cursor.
    if (waveArea.contains (e.x, e.y) && std::abs (e.y - gainLineY (waveArea)) <= 4)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void AudioRegionEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const auto* r = region();
    if (r == nullptr) return;
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);

    // Right-click → context menu (split / reset gain / reset fades / mute /
    // lock / colour). Captured here so it doesn't slip through to the
    // edit-cursor branch below.
    if (e.mods.isPopupMenu())
    {
        if (waveArea.contains (e.x, e.y))
        {
            editCursorSample = sampleForX (e.x, waveArea);
            refreshStatusBarReadouts();
        }
        showContextMenu (e.getScreenPosition());
        return;
    }

    // Drag in the bar/time ruler at the top of the modal → start a
    // range selection. Captured BEFORE the handle / waveform branches
    // so it never gets shadowed by them. Plain click in the ruler
    // clears an existing range without forcing a drag-to-clear.
    const auto rulerArea = juce::Rectangle<int> (0, kIconRowHeight,
                                                    getWidth(), kRulerHeight);
    if (rulerArea.contains (e.x, e.y))
    {
        rangeStartSample = sampleForX (e.x, waveArea);
        rangeEndSample   = rangeStartSample;
        rangeActive      = false;   // becomes true once the drag distance > 0
        dragMode         = DragMode::Range;
        repaint();
        return;
    }

    // Locked regions: visible but un-mutatable. Cursor still drops.
    const bool locked = r->locked;

    // Snapshot the region's current state for the eventual RegionEditAction.
    regionAtDragStart = *r;
    dragOriginMouseY  = e.y;
    dragOriginGainDb  = r->gainDb;

    if (! locked)
    {
        if (fadeInHandleRect (waveArea).contains (e.x, e.y))
        {
            dragMode = DragMode::FadeIn;
            return;
        }
        if (fadeOutHandleRect (waveArea).contains (e.x, e.y))
        {
            dragMode = DragMode::FadeOut;
            return;
        }
        if (trimStartRect (waveArea).contains (e.x, e.y))
        {
            dragMode = DragMode::TrimStart;
            return;
        }
        if (trimEndRect (waveArea).contains (e.x, e.y))
        {
            dragMode = DragMode::TrimEnd;
            return;
        }
        if (waveArea.contains (e.x, e.y) && std::abs (e.y - gainLineY (waveArea)) <= 4)
        {
            dragMode = DragMode::Gain;
            return;
        }
    }

    if (waveArea.contains (e.x, e.y))
    {
        // Click-to-focus inside the neighborhood view: if the click
        // landed on a DIFFERENT slice than the currently-focused one,
        // swap regionIdx to that slice and re-anchor the edit cursor
        // inside it. Same-slice clicks fall through to the existing
        // edit-cursor-drop + MoveRegion-prep behaviour.
        const int hitIdx = regionIndexAtX (e.x, waveArea);
        if (hitIdx >= 0 && hitIdx != regionIdx)
        {
            regionIdx = hitIdx;
            if (auto* nr = region())
            {
                regionAtDragStart = *nr;
                dragOriginGainDb = nr->gainDb;
                editCursorSample = nr->sourceOffset
                    + juce::jlimit<juce::int64> (0, nr->lengthInSamples,
                        timelineSampleForX (e.x, waveArea) - nr->timelineStart);
                dragOriginTimelineSample = nr->timelineStart;
            }
            engine.getTransport().setPlayhead (timelineSampleForX (e.x, waveArea));
            dragMode = DragMode::MoveRegion;
            refreshStatusBarReadouts();
            repaint();
            return;
        }

        // Same-slice click: edit cursor follows + transport playhead
        // snaps to the click so the user can audition from the drop
        // point. dragOriginTimeline tracked so a follow-up horizontal
        // drag can promote to MoveRegion.
        editCursorSample = sampleForX (e.x, waveArea);
        dragOriginTimelineSample = r->timelineStart;
        engine.getTransport().setPlayhead (timelineSampleForX (e.x, waveArea));
        dragMode = DragMode::MoveCursor;
        refreshStatusBarReadouts();
        repaint();
    }
}

void AudioRegionEditor::mouseDrag (const juce::MouseEvent& e)
{
    auto* r = region();
    if (r == nullptr || dragMode == DragMode::None) return;
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);

    if (dragMode == DragMode::MoveCursor)
    {
        editCursorSample = sampleForX (e.x, waveArea);
        engine.getTransport().setPlayhead (timelineSampleForX (e.x, waveArea));
        refreshStatusBarReadouts();
        repaint();
        return;
    }

    if (dragMode == DragMode::MoveRegion)
    {
        // Live-mutate timelineStart so the user sees the slice slide
        // while dragging. Commit (RegionEditAction) fires on mouseUp.
        // Anchor stays put -> view doesn't shift, only the focused
        // slice moves visually.
        if (r->locked) return;
        const auto cursorTimeline = timelineSampleForX (e.x, waveArea);
        const auto dragStartTimeline = timelineSampleForX (e.getMouseDownX(), waveArea);
        const auto delta = cursorTimeline - dragStartTimeline;
        r->timelineStart = juce::jmax<juce::int64> (0,
            dragOriginTimelineSample + delta);
        repaint();
        return;
    }

    if (dragMode == DragMode::Range)
    {
        rangeEndSample = sampleForX (e.x, waveArea);
        rangeActive = (rangeEndSample != rangeStartSample);
        repaint();
        return;
    }

    switch (dragMode)
    {
        case DragMode::FadeIn:
        {
            const auto sample = sampleForX (e.x, waveArea);
            const auto fadeIn = juce::jlimit<juce::int64> (
                0,
                juce::jmax<juce::int64> (0, r->lengthInSamples - r->fadeOutSamples),
                sample - r->sourceOffset);
            r->fadeInSamples = fadeIn;
            break;
        }
        case DragMode::FadeOut:
        {
            const auto sample = sampleForX (e.x, waveArea);
            const auto endAbs = r->sourceOffset + r->lengthInSamples;
            const auto fadeOut = juce::jlimit<juce::int64> (
                0,
                juce::jmax<juce::int64> (0, r->lengthInSamples - r->fadeInSamples),
                endAbs - sample);
            r->fadeOutSamples = fadeOut;
            break;
        }
        case DragMode::Gain:
        {
            // 0.1 dB per pixel matches TapeStrip's Alt-drag scale, so the
            // muscle-memory carries over.
            const float deltaDb = (float) (dragOriginMouseY - e.y) * 0.1f;
            r->gainDb = juce::jlimit (-24.0f, 12.0f, dragOriginGainDb + deltaDb);
            break;
        }
        case DragMode::TrimStart:
        {
            // Trim-start moves the slice's leading edge along the source
            // file. sourceOffset shifts by the delta so the audio stays
            // continuous (mirrors TapeStrip's TrimStart op). lengthInSamples
            // shrinks/grows by the same amount.
            const auto newSourceOffset = juce::jlimit<juce::int64> (
                0,
                regionAtDragStart.sourceOffset
                    + regionAtDragStart.lengthInSamples - 1,
                sampleForX (e.x, waveArea));
            const auto delta = newSourceOffset - regionAtDragStart.sourceOffset;
            r->sourceOffset    = newSourceOffset;
            r->lengthInSamples = regionAtDragStart.lengthInSamples - delta;
            r->timelineStart   = regionAtDragStart.timelineStart   + delta;
            // Clamp fades against the new length.
            r->fadeInSamples  = juce::jlimit<juce::int64> (0, r->lengthInSamples, r->fadeInSamples);
            r->fadeOutSamples = juce::jlimit<juce::int64> (0, r->lengthInSamples - r->fadeInSamples, r->fadeOutSamples);
            break;
        }
        case DragMode::TrimEnd:
        {
            const auto endAbs = sampleForX (e.x, waveArea);
            const auto newLength = juce::jlimit<juce::int64> (
                1,
                std::numeric_limits<juce::int64>::max(),
                endAbs - regionAtDragStart.sourceOffset);
            r->lengthInSamples = newLength;
            r->fadeInSamples  = juce::jlimit<juce::int64> (0, newLength, r->fadeInSamples);
            r->fadeOutSamples = juce::jlimit<juce::int64> (0, newLength - r->fadeInSamples, r->fadeOutSamples);
            break;
        }
        default: break;
    }
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::mouseUp (const juce::MouseEvent&)
{
    auto* r = region();
    if (dragMode == DragMode::Range)
    {
        // Normalise so start <= end (drag-leftwards still works).
        if (rangeEndSample < rangeStartSample)
            std::swap (rangeStartSample, rangeEndSample);
        rangeActive = (rangeEndSample > rangeStartSample);
        dragMode = DragMode::None;
        return;
    }
    const bool wasUndoableEdit = (r != nullptr
                                    && dragMode != DragMode::None
                                    && dragMode != DragMode::MoveCursor);
    if (wasUndoableEdit)
    {
        const AudioRegion afterState = *r;
        // Skip the action if nothing actually changed (e.g. user grabbed
        // a handle and didn't drag). UndoManager would happily record a
        // no-op transaction otherwise.
        const bool changed =
            afterState.sourceOffset    != regionAtDragStart.sourceOffset
         || afterState.timelineStart   != regionAtDragStart.timelineStart
         || afterState.lengthInSamples != regionAtDragStart.lengthInSamples
         || afterState.fadeInSamples   != regionAtDragStart.fadeInSamples
         || afterState.fadeOutSamples  != regionAtDragStart.fadeOutSamples
         || afterState.gainDb          != regionAtDragStart.gainDb;
        if (changed)
        {
            // Roll the live state back to the before-state, then submit
            // the action - UndoManager.perform() calls perform() which
            // applies after-state. This way the action's stored before
            // is the authoritative one.
            *r = regionAtDragStart;
            auto& um = engine.getUndoManager();
            um.beginNewTransaction (
                dragMode == DragMode::FadeIn     ? "Fade-in"      :
                dragMode == DragMode::FadeOut    ? "Fade-out"     :
                dragMode == DragMode::Gain       ? "Region gain"  :
                dragMode == DragMode::TrimStart  ? "Trim start"   :
                dragMode == DragMode::TrimEnd    ? "Trim end"     :
                dragMode == DragMode::MoveRegion ? "Move region"  :
                                                       "Edit region");
            um.perform (new RegionEditAction (
                session, engine, trackIdx, regionIdx,
                regionAtDragStart, afterState));
        }
    }
    dragMode = DragMode::None;
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::showContextMenu (juce::Point<int> screenPos)
{
    auto* r = region();
    if (r == nullptr) return;

    juce::PopupMenu m;
    m.addItem (1, "Split at edit cursor");
    m.addSeparator();
    m.addItem (2, "Reset gain (0 dB)");
    m.addItem (3, "Reset fades");
    m.addSeparator();
    m.addItem (4, r->muted  ? "Unmute"  : "Mute");
    m.addItem (5, r->locked ? "Unlock"  : "Lock");

    juce::Component::SafePointer<AudioRegionEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options()
                        .withTargetScreenArea (juce::Rectangle<int> (
                            screenPos.x, screenPos.y, 1, 1)),
        [safe] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || chosen <= 0) return;
            auto* rr = self->region();
            if (rr == nullptr) return;

            auto& um = self->engine.getUndoManager();

            if (chosen == 1)
            {
                // Split needs an absolute timeline-sample, not a file
                // sample. Map the editor's editCursorSample (which is
                // file-relative inside the slice) back onto the timeline.
                const auto timelineSplit =
                    rr->timelineStart + (self->editCursorSample - rr->sourceOffset);
                um.beginNewTransaction ("Split region");
                um.perform (new SplitRegionAction (
                    self->session, self->engine,
                    self->trackIdx, self->regionIdx, timelineSplit));
                self->refreshStatusBarReadouts();
                self->repaint();
                return;
            }

            // The remaining actions all become RegionEditActions.
            const AudioRegion before = *rr;
            AudioRegion after = before;
            const char* label = "Edit region";
            switch (chosen)
            {
                case 2: after.gainDb         = 0.0f;  label = "Reset gain";  break;
                case 3: after.fadeInSamples  = 0;
                          after.fadeOutSamples = 0;     label = "Reset fades"; break;
                case 4: after.muted          = ! before.muted;
                        label = before.muted ? "Unmute" : "Mute"; break;
                case 5: after.locked         = ! before.locked;
                        label = before.locked ? "Unlock" : "Lock"; break;
                default: return;
            }
            um.beginNewTransaction (label);
            um.perform (new RegionEditAction (
                self->session, self->engine,
                self->trackIdx, self->regionIdx,
                before, after));
            self->refreshStatusBarReadouts();
            self->repaint();
        });
}

void AudioRegionEditor::mouseWheelMove (const juce::MouseEvent& e,
                                          const juce::MouseWheelDetails& w)
{
    const auto* r = region();
    if (r == nullptr) return;
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);

    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        // Zoom anchored on the cursor's current sample.
        const auto cursorSampleBefore = sampleForX (e.x, waveArea);
        const float factor = w.deltaY > 0.0f ? 1.15f : (1.0f / 1.15f);
        pixelsPerSample = juce::jlimit (1.0e-5f, 1.0f, pixelsPerSample * factor);
        // After zoom, adjust scrollSamples so the same sample stays at
        // the cursor's x-position.
        const auto cursorSampleAfterRaw = sampleForX (e.x, waveArea);
        scrollSamples += (cursorSampleBefore - cursorSampleAfterRaw);
        scrollSamples = juce::jlimit<juce::int64> (0,
            juce::jmax<juce::int64> (0, r->lengthInSamples - 1), scrollSamples);
        repaint();
        return;
    }

    // Plain wheel = horizontal scroll.
    const auto step = (juce::int64) std::round (
        - (double) w.deltaY * 64.0 / juce::jmax (1.0e-5f, pixelsPerSample));
    scrollSamples = juce::jlimit<juce::int64> (0,
        juce::jmax<juce::int64> (0, r->lengthInSamples - 1),
        scrollSamples + step);
    repaint();
}

bool AudioRegionEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        // First Esc clears an active range; second Esc closes. Matches
        // the piano roll's "Esc clears selection then closes" pattern.
        if (rangeActive)
        {
            rangeActive = false;
            repaint();
            return true;
        }
        if (onCloseRequested) onCloseRequested();
        return true;
    }
    const bool cmdOrCtrl = k.getModifiers().isCommandDown()
                              || k.getModifiers().isCtrlDown();
    // Local Cmd+Z / Cmd+Shift+Z. The engine's UndoManager has a global
    // keyboard binding, but it doesn't know to invalidate this editor's
    // view — so without this handler the audio data reverts but the
    // modal still paints the pre-undo geometry. Calling undo() here
    // and then repainting + refreshing the readouts keeps the view in
    // sync with the model on every step of the undo stack.
    if (cmdOrCtrl && (k.getTextCharacter() == 'z' || k.getTextCharacter() == 'Z'))
    {
        if (k.getModifiers().isShiftDown()) engine.getUndoManager().redo();
        else                                  engine.getUndoManager().undo();
        rangeActive = false;
        refreshStatusBarReadouts();
        repaint();
        return true;
    }
    // Region navigation - same-track only, no wrap.
    if (cmdOrCtrl && k.getTextCharacter() == ']') { navigateRegion (+1); return true; }
    if (cmdOrCtrl && k.getTextCharacter() == '[') { navigateRegion (-1); return true; }

    // Split: 'S' or Cmd+E. With a range active, splits at BOTH
    // boundaries (3 regions). Without a range, splits at the edit
    // cursor (existing single-cut behaviour from splitAtCursor()).
    const auto ch = k.getTextCharacter();
    const bool isSplit = (ch == 's' || ch == 'S')
                          || (cmdOrCtrl && (ch == 'e' || ch == 'E'));
    if (isSplit)
    {
        auto* r = region();
        if (r == nullptr) return true;
        if (rangeActive)
        {
            auto& um = engine.getUndoManager();
            um.beginNewTransaction ("Split range");
            // Split at the right boundary FIRST so the left boundary's
            // sample index stays valid in the same regions vector.
            const auto a = juce::jmin (rangeStartSample, rangeEndSample);
            const auto b = juce::jmax (rangeStartSample, rangeEndSample);
            const auto tlA = r->timelineStart + (a - r->sourceOffset);
            const auto tlB = r->timelineStart + (b - r->sourceOffset);
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlB));
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlA));
            rangeActive = false;
            refreshStatusBarReadouts();
            repaint();
            return true;
        }
        splitAtCursor();
        return true;
    }

    // Fade-fit: F = fade-in spans the selection length;
    // Shift+F = fade-out spans the selection length. Both are
    // single RegionEditAction edits, so Cmd+Z reverts cleanly.
    if ((ch == 'f' || ch == 'F') && rangeActive)
    {
        auto* r = region();
        if (r == nullptr) return true;
        const auto fadeLen = juce::jmax<juce::int64> (
            0, juce::jmax (rangeStartSample, rangeEndSample)
                 - juce::jmin (rangeStartSample, rangeEndSample));
        const AudioRegion before = *r;
        AudioRegion after = before;
        if (k.getModifiers().isShiftDown())
            after.fadeOutSamples = juce::jmin (fadeLen,
                                                  juce::jmax<juce::int64> (0, r->lengthInSamples - r->fadeInSamples));
        else
            after.fadeInSamples = juce::jmin (fadeLen,
                                                 juce::jmax<juce::int64> (0, r->lengthInSamples - r->fadeOutSamples));
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (k.getModifiers().isShiftDown() ? "Fade-out to selection"
                                                                  : "Fade-in to selection");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
        return true;
    }

    // Clipboard - bridges to the engine-wide region clipboard
    // shared with TapeStrip. Pattern matches TapeStrip's
    // copySelectedRegion / cutSelectedRegion / pasteAtPlayhead.
    //
    // With rangeActive: C/X operate on the SELECTED CHUNK rather than
    // the whole region. The clipboard region is constructed as if it
    // were a free-standing slice of the source file - same audio file,
    // sourceOffset advanced to the chunk's start, length = chunk
    // length. Paste then drops it as a new region at the edit cursor.
    if (cmdOrCtrl && (k.getTextCharacter() == 'c' || k.getTextCharacter() == 'C'))
    {
        auto* r = region();
        if (r == nullptr) return true;
        auto& clip = engine.getRegionClipboard();
        if (rangeActive)
        {
            const auto a = juce::jmin (rangeStartSample, rangeEndSample);
            const auto b = juce::jmax (rangeStartSample, rangeEndSample);
            AudioRegion chunk = *r;
            chunk.sourceOffset    = a;
            chunk.lengthInSamples = b - a;
            chunk.timelineStart   = 0;            // paste rewrites this
            chunk.fadeInSamples   = 0;            // chunk has no inherited fades
            chunk.fadeOutSamples  = 0;
            chunk.previousTakes.clear();          // takes don't carry across paste
            clip.region      = chunk;
        }
        else
        {
            clip.region = *r;
        }
        clip.sourceTrack = trackIdx;
        clip.hasContent  = true;
        return true;
    }
    if (cmdOrCtrl && (k.getTextCharacter() == 'x' || k.getTextCharacter() == 'X'))
    {
        auto* r = region();
        if (r == nullptr) return true;
        auto& clip = engine.getRegionClipboard();
        auto& um = engine.getUndoManager();

        if (rangeActive)
        {
            // Cut chunk: same shape as Copy + the Backspace-with-range
            // path. Editor stays open on the left slice (regionIdx
            // remains valid because SplitRegionAction inserts the
            // right slice at regionIdx+1).
            const auto a = juce::jmin (rangeStartSample, rangeEndSample);
            const auto b = juce::jmax (rangeStartSample, rangeEndSample);
            AudioRegion chunk = *r;
            chunk.sourceOffset    = a;
            chunk.lengthInSamples = b - a;
            chunk.timelineStart   = 0;
            chunk.fadeInSamples   = 0;
            chunk.fadeOutSamples  = 0;
            chunk.previousTakes.clear();
            clip.region      = chunk;
            clip.sourceTrack = trackIdx;
            clip.hasContent  = true;

            const auto tlA = r->timelineStart + (a - r->sourceOffset);
            const auto tlB = r->timelineStart + (b - r->sourceOffset);
            um.beginNewTransaction ("Cut chunk");
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlB));
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlA));
            um.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx + 1));
            rangeActive = false;
            refreshStatusBarReadouts();
            repaint();
            return true;
        }

        // Whole-region cut. Editor closes (its anchor region is gone).
        clip.region      = *r;
        clip.sourceTrack = trackIdx;
        clip.hasContent  = true;
        um.beginNewTransaction ("Cut region");
        um.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx));
        if (onCloseRequested) onCloseRequested();
        return true;
    }
    if (cmdOrCtrl && (k.getTextCharacter() == 'v' || k.getTextCharacter() == 'V'))
    {
        auto& clip = engine.getRegionClipboard();
        if (! clip.hasContent) return true;
        // Paste at the EDIT CURSOR's timeline position when the editor
        // owns the gesture. The editor's edit cursor is what the user
        // last clicked / dropped, so paste-at-cursor is what they
        // expect. Falls back to the transport playhead only when no
        // region is open (defensive; shouldn't fire today).
        auto* r = region();
        AudioRegion pasted = clip.region;
        if (r != nullptr)
            pasted.timelineStart = r->timelineStart + (editCursorSample - r->sourceOffset);
        else
            pasted.timelineStart = engine.getTransport().getPlayhead();
        const int targetTrack = (clip.sourceTrack >= 0 && clip.sourceTrack < Session::kNumTracks)
                                  ? clip.sourceTrack : trackIdx;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Paste region");
        um.perform (new PasteRegionAction (session, engine, targetTrack, pasted));
        return true;
    }

    // Delete - two paths.
    //   • Range active: split at both boundaries, then delete the
    //     middle slice. Region's anchor stays valid (the left slice
    //     keeps the original regionIdx) so the editor stays open.
    //   • No range: delete the whole region (Editor closes).
    // Both routed through UndoManager - Cmd+Z reverts.
    if (k == juce::KeyPress::backspaceKey || k == juce::KeyPress::deleteKey)
    {
        auto* r = region();
        if (rangeActive && r != nullptr)
        {
            auto& um = engine.getUndoManager();
            um.beginNewTransaction ("Delete chunk");
            const auto a = juce::jmin (rangeStartSample, rangeEndSample);
            const auto b = juce::jmax (rangeStartSample, rangeEndSample);
            const auto tlA = r->timelineStart + (a - r->sourceOffset);
            const auto tlB = r->timelineStart + (b - r->sourceOffset);
            // Split at B first, then A, so regionIdx still points at
            // the LEFT slice after both splits. The middle slice is
            // then regionIdx + 1.
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlB));
            um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, tlA));
            um.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx + 1));
            rangeActive = false;
            refreshStatusBarReadouts();
            repaint();
            return true;
        }
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Delete region");
        um.perform (new DeleteRegionAction (session, engine, trackIdx, regionIdx));
        if (onCloseRequested) onCloseRequested();
        return true;
    }

    // Move - arrow keys nudge timelineStart by 1 beat (Shift = 1 bar).
    // Up / Down are reserved for future gain-nudge; keeping them no-op
    // for now keeps the surface focused on horizontal motion.
    if (k == juce::KeyPress::leftKey || k == juce::KeyPress::rightKey)
    {
        auto* r = region();
        if (r == nullptr) return true;
        const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
        const double bpm = juce::jmax (1.0, (double) session.tempoBpm.load (std::memory_order_relaxed));
        const int    bpb = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
        const auto samplesPerBeat = (juce::int64) std::round (sr * 60.0 / bpm);
        const auto step = (k.getModifiers().isShiftDown() ? samplesPerBeat * bpb
                                                              : samplesPerBeat);
        const auto delta = (k == juce::KeyPress::leftKey ? -step : step);
        const AudioRegion before = *r;
        AudioRegion after = before;
        after.timelineStart = juce::jmax<juce::int64> (0, before.timelineStart + delta);
        if (after.timelineStart == before.timelineStart) return true;
        auto& um = engine.getUndoManager();
        um.beginNewTransaction (delta < 0 ? "Nudge region left" : "Nudge region right");
        um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
        refreshStatusBarReadouts();
        repaint();
        return true;
    }

    // Zoom - '=' (or '+') and '-'. Anchors on the edit cursor so the
    // visible part of the slice stays under the user's finger across
    // a zoom step. Mirrors mouseWheelMove's Cmd+wheel ramp.
    if (ch == '=' || ch == '+' || ch == '-')
    {
        const auto* r = region();
        if (r == nullptr) return true;
        const auto waveArea = juce::Rectangle<int> (
            0, kIconRowHeight + kRulerHeight,
            getWidth(),
            getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);
        const float factor = (ch == '-') ? (1.0f / 1.15f) : 1.15f;
        const auto anchorXBefore = xForSample (editCursorSample, waveArea);
        pixelsPerSample = juce::jlimit (1.0e-5f, 1.0f, pixelsPerSample * factor);
        const auto anchorSampleAfterRaw = sampleForX (anchorXBefore, waveArea);
        scrollSamples += (editCursorSample - anchorSampleAfterRaw);
        scrollSamples = juce::jlimit<juce::int64> (0,
            juce::jmax<juce::int64> (0, r->lengthInSamples - 1), scrollSamples);
        repaint();
        return true;
    }
    return false;
}

// ── Top icon-row buttons ──────────────────────────────────────────────
AudioRegionEditor::IconButton::IconButton (const juce::String& name, Glyph g)
    : juce::Button (name), glyph (g)
{
    setClickingTogglesState (false);
}

void AudioRegionEditor::IconButton::paintButton (juce::Graphics& g,
                                                    bool isMouseOver, bool isButtonDown)
{
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    auto disc = juce::Colour (0xff262630);
    if (isButtonDown)      disc = disc.brighter (0.14f);
    else if (isMouseOver)  disc = disc.brighter (0.08f);
    g.setColour (disc);
    g.fillEllipse (bounds);
    g.setColour (juce::Colour (0xff3a3a44));
    g.drawEllipse (bounds, isButtonDown ? 2.0f : 1.4f);

    const auto centre = bounds.getCentre();
    const float r = bounds.getWidth() * 0.30f;
    g.setColour (juce::Colour (0xffd0d0d0));

    auto strokeArrowhead = [&] (juce::Point<float> tip, juce::Point<float> from, float size)
    {
        const auto dir = (tip - from);
        const auto len = juce::jmax (0.001f, dir.getDistanceFromOrigin());
        const auto u   = dir / len;
        const auto perp = juce::Point<float> (-u.y, u.x);
        const auto a = tip - u * size + perp * size * 0.6f;
        const auto b = tip - u * size - perp * size * 0.6f;
        juce::Path p;
        p.startNewSubPath (a);
        p.lineTo (tip);
        p.lineTo (b);
        g.strokePath (p, juce::PathStrokeType (1.6f));
    };

    switch (glyph)
    {
        case Glyph::Undo:
        case Glyph::Redo:
        {
            const float radius = r * 0.95f;
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, radius, radius,
                                  0.0f,
                                  juce::MathConstants<float>::pi * 0.30f,
                                  juce::MathConstants<float>::pi * 1.85f, true);
            if (glyph == Glyph::Redo)
                arc.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f, centre.x, centre.y));
            g.strokePath (arc, juce::PathStrokeType (1.6f));
            const float startAng = juce::MathConstants<float>::pi * 0.30f;
            juce::Point<float> start (centre.x + std::sin (startAng) * radius,
                                          centre.y - std::cos (startAng) * radius);
            if (glyph == Glyph::Redo) start = juce::Point<float> (2 * centre.x - start.x, start.y);
            const auto inwards = juce::Point<float> (centre.x, centre.y) - start;
            strokeArrowhead (start, start + inwards * 0.001f, 4.0f);
            break;
        }
        case Glyph::Split:
        {
            // Vertical bar with arrows pointing apart.
            const float h = r * 1.6f;
            g.fillRect (juce::Rectangle<float> (centre.x - 0.8f, centre.y - h * 0.5f, 1.6f, h));
            juce::Path l, ar;
            l.startNewSubPath (centre.x - r * 0.95f, centre.y);
            l.lineTo (centre.x - 2.0f, centre.y);
            ar.startNewSubPath (centre.x + 2.0f, centre.y);
            ar.lineTo (centre.x + r * 0.95f, centre.y);
            g.strokePath (l,  juce::PathStrokeType (1.4f));
            g.strokePath (ar, juce::PathStrokeType (1.4f));
            strokeArrowhead ({ centre.x - r * 0.95f, centre.y },
                                { centre.x - 2.0f,         centre.y }, 3.5f);
            strokeArrowhead ({ centre.x + r * 0.95f, centre.y },
                                { centre.x + 2.0f,         centre.y }, 3.5f);
            break;
        }
        case Glyph::Normalize:
        {
            // Waveform peak hitting a ceiling line.
            const float yTop = centre.y - r * 0.85f;
            const float yMid = centre.y;
            const float yBot = centre.y + r * 0.85f;
            // Ceiling line.
            g.drawLine (centre.x - r, yTop, centre.x + r, yTop, 1.2f);
            // Waveform peaks - bars of varying heights, the middle one
            // touching the ceiling.
            const float bw = 1.6f;
            const float xs[5] = { centre.x - r * 0.7f, centre.x - r * 0.35f, centre.x,
                                     centre.x + r * 0.35f, centre.x + r * 0.7f };
            const float hs[5] = { r * 0.4f, r * 0.7f, r * 0.85f, r * 0.55f, r * 0.3f };
            for (int i = 0; i < 5; ++i)
                g.fillRect (juce::Rectangle<float> (xs[i] - bw * 0.5f, yMid - hs[i],
                                                       bw, hs[i] * 2.0f));
            (void) yBot;
            break;
        }
        case Glyph::Reverse:
        {
            // Left-pointing chevron arrow over a wave-line.
            juce::Path p;
            p.startNewSubPath (centre.x + r * 0.7f, centre.y - r * 0.6f);
            p.lineTo          (centre.x - r * 0.7f, centre.y);
            p.lineTo          (centre.x + r * 0.7f, centre.y + r * 0.6f);
            g.strokePath (p, juce::PathStrokeType (1.6f));
            // Tiny wavy line below to suggest "audio".
            juce::Path wave;
            wave.startNewSubPath (centre.x - r * 0.7f, centre.y + r * 0.85f);
            wave.quadraticTo    (centre.x - r * 0.35f, centre.y + r * 0.6f,
                                    centre.x,          centre.y + r * 0.85f);
            wave.quadraticTo    (centre.x + r * 0.35f, centre.y + r * 1.1f,
                                    centre.x + r * 0.7f, centre.y + r * 0.85f);
            g.strokePath (wave, juce::PathStrokeType (1.0f));
            break;
        }
        case Glyph::TakeCycle:
        {
            // Two circular arrows forming a refresh-cycle.
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, r * 0.9f, r * 0.9f,
                                  0.0f,
                                  juce::MathConstants<float>::pi * 0.35f,
                                  juce::MathConstants<float>::pi * 1.40f, true);
            g.strokePath (arc, juce::PathStrokeType (1.4f));
            // Arrowhead at the end of the arc.
            const float endAng = juce::MathConstants<float>::pi * 1.40f;
            juce::Point<float> end (centre.x + std::sin (endAng) * r * 0.9f,
                                       centre.y - std::cos (endAng) * r * 0.9f);
            strokeArrowhead (end, end + juce::Point<float> (0.5f, 0.5f), 3.0f);
            break;
        }
        case Glyph::ZoomFit:
        {
            const float w = r * 0.95f;
            const float h = r * 1.10f;
            const float bracketLen = h * 0.9f;
            const float armLen     = r * 0.45f;
            g.drawLine (centre.x - w, centre.y - bracketLen * 0.5f,
                          centre.x - w, centre.y + bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x - w, centre.y - bracketLen * 0.5f,
                          centre.x - w + armLen, centre.y - bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x - w, centre.y + bracketLen * 0.5f,
                          centre.x - w + armLen, centre.y + bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x + w, centre.y - bracketLen * 0.5f,
                          centre.x + w, centre.y + bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x + w, centre.y - bracketLen * 0.5f,
                          centre.x + w - armLen, centre.y - bracketLen * 0.5f, 1.4f);
            g.drawLine (centre.x + w, centre.y + bracketLen * 0.5f,
                          centre.x + w - armLen, centre.y + bracketLen * 0.5f, 1.4f);
            g.fillEllipse (centre.x - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
            break;
        }
        case Glyph::Properties:
        {
            g.drawEllipse (centre.x - r * 0.55f, centre.y - r * 0.55f,
                              r * 1.10f, r * 1.10f, 1.4f);
            for (int i = 0; i < 6; ++i)
            {
                const float ang = juce::MathConstants<float>::twoPi * (float) i / 6.0f;
                g.drawLine (centre.x + std::cos (ang) * r * 0.65f,
                              centre.y + std::sin (ang) * r * 0.65f,
                              centre.x + std::cos (ang) * r * 0.95f,
                              centre.y + std::sin (ang) * r * 0.95f, 1.4f);
            }
            g.fillEllipse (centre.x - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
            break;
        }
    }
}

void AudioRegionEditor::layoutIconRow (juce::Rectangle<int> area)
{
    auto inner = area.reduced (8, 4);
    const int dia = juce::jmin (inner.getHeight(), 28);
    const int gap = 6;
    auto place = [&] (IconButton& b)
    {
        b.setBounds (inner.removeFromLeft (dia).withSizeKeepingCentre (dia, dia));
        inner.removeFromLeft (gap);
    };
    place (undoButton);
    place (redoButton);
    inner.removeFromLeft (gap);
    place (splitButton);
    place (normalizeButton);
    place (reverseButton);
    place (takeCycleButton);
    // Tiny take readout right after the TakeCycle button. Sized so
    // "T99/99" fits comfortably; longer strings clip rather than push
    // the rest of the toolbar.
    takeReadout.setBounds (inner.removeFromLeft (44)
                                  .withSizeKeepingCentre (44, area.getHeight() - 8));
    inner.removeFromLeft (gap);
    place (zoomFitButton);
    place (propertiesButton);
    // Title strip occupies what's left of the toolbar band. Stretches
    // so long region labels / filenames don't get truncated below the
    // available width.
    if (inner.getWidth() > 0)
        titleLabel.setBounds (inner.reduced (8, 2));
}

void AudioRegionEditor::layoutStatusBar (juce::Rectangle<int> area)
{
    auto inner = area.reduced (8, 4);
    positionLabel.setBounds (inner.removeFromLeft (110));
    inner.removeFromLeft (4);
    gainLabel    .setBounds (inner.removeFromLeft (84));
    inner.removeFromLeft (4);
    fadeLabel    .setBounds (inner.removeFromLeft (162));
    inner.removeFromLeft (10);

    // Right-aligned cluster: lock, mute, info readout.
    lockToggle.setBounds (inner.removeFromRight (60));
    inner.removeFromRight (4);
    muteToggle.setBounds (inner.removeFromRight (60));
    inner.removeFromRight (10);
    infoLabel .setBounds (inner);
}

juce::String formatBarBeatTickAt (juce::int64 sliceFileSample,
                                     const AudioRegion& r,
                                     double sampleRate, double bpm, int beatsPerBar)
{
    const double samplesPerBeat = juce::jmax (1.0, sampleRate * 60.0 / juce::jmax (1.0, bpm));
    const double samplesPerBar  = samplesPerBeat * juce::jmax (1, beatsPerBar);
    // Convert file-sample → timeline-sample (slice-relative offset added to timelineStart).
    const double timelineSample = (double) r.timelineStart
                                    + (double) (sliceFileSample - r.sourceOffset);
    const double bar  = std::floor (timelineSample / samplesPerBar);
    const double rem1 = timelineSample - bar * samplesPerBar;
    const double beat = std::floor (rem1 / samplesPerBeat);
    const double rem2 = rem1 - beat * samplesPerBeat;
    const int    sub  = (int) std::round ((rem2 / samplesPerBeat) * 1000.0);
    return juce::String ((int) bar + 1) + "."
         + juce::String ((int) beat + 1) + "."
         + juce::String (juce::jlimit (0, 999, sub)).paddedLeft ('0', 3);
}

void AudioRegionEditor::refreshStatusBarReadouts()
{
    const auto* r = region();
    const double sr  = juce::jmax (1.0, engine.getCurrentSampleRate());
    const double bpm = juce::jmax (1.0, (double) session.tempoBpm.load (std::memory_order_relaxed));
    const int    bpb = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));

    if (r != nullptr)
    {
        const auto mode = (TimeDisplayMode) session.timeDisplayMode.load (std::memory_order_relaxed);
        const auto timelineSample = r->timelineStart + (editCursorSample - r->sourceOffset);
        positionLabel.setText ("pos " + formatSamplePosition (timelineSample, sr,
                                                                (float) bpm, bpb, mode),
                                  juce::dontSendNotification);
        gainLabel.setText (juce::String (r->gainDb, 1) + " dB",
                              juce::dontSendNotification);
        const double fadeInMs  = (double) r->fadeInSamples  * 1000.0 / sr;
        const double fadeOutMs = (double) r->fadeOutSamples * 1000.0 / sr;
        fadeLabel.setText ("fade " + juce::String (fadeInMs, 0)
                              + " / " + juce::String (fadeOutMs, 0) + " ms",
                              juce::dontSendNotification);
        const int channels = juce::jmax (1, r->numChannels);
        const double durSec = (double) r->lengthInSamples / sr;
        const int mins = (int) (durSec / 60.0);
        const double secs = durSec - mins * 60.0;
        infoLabel.setText (juce::String (channels) + "ch - "
                              + juce::String ((int) std::round (sr / 1000.0)) + " kHz - "
                              + juce::String (mins) + ":"
                              + juce::String (secs, 3),
                              juce::dontSendNotification);
        muteToggle.setToggleState (r->muted,  juce::dontSendNotification);
        lockToggle.setToggleState (r->locked, juce::dontSendNotification);

        // Take counter: live take is index 1; previousTakes stack
        // alongside. "T1/3" = live + 2 stacked older takes.
        const int totalTakes = 1 + (int) r->previousTakes.size();
        takeReadout.setText ("T1/" + juce::String (totalTakes),
                                juce::dontSendNotification);

        // Title: prefer user label, fall back to filename. Skip update
        // while the label is in edit mode so we don't stomp typed input.
        if (! titleLabel.isBeingEdited())
        {
            const auto displayTitle = r->label.isNotEmpty()
                                          ? r->label
                                          : r->file.getFileName();
            titleLabel.setText (displayTitle, juce::dontSendNotification);
        }
    }
}

void AudioRegionEditor::splitAtCursor()
{
    auto* r = region();
    if (r == nullptr) return;
    const auto timelineSplit = r->timelineStart + (editCursorSample - r->sourceOffset);
    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Split region");
    um.perform (new SplitRegionAction (session, engine, trackIdx, regionIdx, timelineSplit));
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::zoomFit()
{
    const auto waveArea = juce::Rectangle<int> (0, kIconRowHeight + kRulerHeight,
                                                   getWidth(),
                                                   getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);
    zoomFitToArea (waveArea);
    repaint();
}

void AudioRegionEditor::cycleTake()
{
    auto* r = region();
    if (r == nullptr || r->previousTakes.empty()) return;
    const AudioRegion before = *r;
    AudioRegion after = before;

    // Rotate: front of previousTakes becomes the new active take; the
    // current active take goes to the back. Same shape as TapeStrip's
    // TakeBadge rotation logic.
    TakeRef nextTake = after.previousTakes.front();
    after.previousTakes.erase (after.previousTakes.begin());
    TakeRef oldActive;
    oldActive.file            = after.file;
    oldActive.sourceOffset    = after.sourceOffset;
    oldActive.lengthInSamples = after.lengthInSamples;
    after.previousTakes.push_back (oldActive);
    after.file            = nextTake.file;
    after.sourceOffset    = nextTake.sourceOffset;
    after.lengthInSamples = nextTake.lengthInSamples;
    after.fadeInSamples   = juce::jlimit<juce::int64> (0, after.lengthInSamples, after.fadeInSamples);
    after.fadeOutSamples  = juce::jlimit<juce::int64> (0, after.lengthInSamples - after.fadeInSamples,
                                                            after.fadeOutSamples);

    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Cycle take");
    um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
    rebuildThumbIfNeeded();
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::normalizeRegion()
{
    auto* r = region();
    if (r == nullptr || ! r->file.existsAsFile() || r->lengthInSamples <= 0) return;

    // Non-destructive: scan the slice in chunks to find peak, then adjust
    // gainDb so peak hits ~-0.1 dBFS. Source file untouched. Chunked so a
    // multi-minute region doesn't allocate gigabytes.
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (r->file));
    if (reader == nullptr) return;

    const int channels = juce::jmax (1, (int) reader->numChannels);
    constexpr int kChunkSamples = 1 << 16;   // 64k frames per chunk
    juce::AudioBuffer<float> buf (channels, kChunkSamples);

    float peak = 0.0f;
    juce::int64 done = 0;
    while (done < r->lengthInSamples)
    {
        const int thisChunk = (int) juce::jmin<juce::int64> (kChunkSamples,
                                                                  r->lengthInSamples - done);
        reader->read (&buf, 0, thisChunk, r->sourceOffset + done, true, true);
        for (int c = 0; c < channels; ++c)
        {
            const auto rng = buf.findMinMax (c, 0, thisChunk);
            peak = juce::jmax (peak, std::abs (rng.getStart()), std::abs (rng.getEnd()));
        }
        done += thisChunk;
    }
    if (peak <= 1.0e-6f) return;   // silence; nothing to normalize

    const float targetPeak = 0.99f;   // -0.087 dBFS
    const float deltaDb    = juce::Decibels::gainToDecibels (targetPeak / peak);

    const AudioRegion before = *r;
    AudioRegion after = before;
    after.gainDb = juce::jlimit (-24.0f, 12.0f, before.gainDb + deltaDb);

    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Normalize");
    um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::reverseRegion()
{
    auto* r = region();
    if (r == nullptr || ! r->file.existsAsFile() || r->lengthInSamples <= 0) return;

    auto warn = [] (const juce::String& msg)
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon, "Reverse region", msg);
    };

    // Cap the in-memory reverse buffer at ~1 GiB of float storage across all
    // channels (≈ 30 min mono / 15 min stereo @ 96 kHz). Above this the alloc
    // becomes a real OOM risk; ask the user to split the region first.
    constexpr juce::int64 kMaxReverseFloats = 256ll * 1024 * 1024;
    const juce::int64 channelCount = juce::jmax<juce::int64> (1, r->numChannels);
    if (r->lengthInSamples * channelCount > kMaxReverseFloats)
    {
        warn ("Region is too large to reverse in memory. Split it first, then reverse the pieces.");
        return;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (r->file));
    if (reader == nullptr) { warn ("Could not open the source file for reading."); return; }

    const auto numSamples = (int) juce::jlimit<juce::int64> (
        1, std::numeric_limits<int>::max(), r->lengthInSamples);
    const int channels = juce::jmax (1, (int) reader->numChannels);
    juce::AudioBuffer<float> buf (channels, numSamples);
    reader->read (&buf, 0, numSamples, r->sourceOffset, true, true);

    // In-place reverse per channel.
    for (int c = 0; c < channels; ++c)
        std::reverse (buf.getWritePointer (c), buf.getWritePointer (c) + numSamples);

    // Write to <session>/takes/<originalStem>-rev-<uuid>.wav.
    auto takesDir = session.getSessionDirectory().getChildFile ("takes");
    if (! takesDir.exists())
    {
        const auto res = takesDir.createDirectory();
        if (res.failed()) { warn ("Could not create takes directory: " + res.getErrorMessage()); return; }
    }
    const auto outFile = takesDir.getNonexistentChildFile (
        r->file.getFileNameWithoutExtension() + "-rev", ".wav", false);

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> out (outFile.createOutputStream());
    if (out == nullptr) { warn ("Could not open the output file for writing."); return; }
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (out.get(), reader->sampleRate, (juce::uint32) channels,
                                (int) reader->bitsPerSample, {}, 0));
    if (writer == nullptr)
    {
        // out still owns the stream; let it close on scope exit before deleting.
        out.reset();
        outFile.deleteFile();
        warn ("Could not create the WAV writer.");
        return;
    }
    out.release();   // ownership transferred to writer
    if (! writer->writeFromAudioSampleBuffer (buf, 0, numSamples))
    {
        writer.reset();
        outFile.deleteFile();
        warn ("Failed to write the reversed audio to disk.");
        return;
    }
    writer.reset();   // flush + close

    const AudioRegion before = *r;
    AudioRegion after = before;
    after.file            = outFile;
    after.sourceOffset    = 0;
    after.lengthInSamples = numSamples;
    // Rotate the original into previousTakes so the user can flip back via
    // the Take-cycle icon - matches what cycleTake expects.
    TakeRef oldTake;
    oldTake.file            = before.file;
    oldTake.sourceOffset    = before.sourceOffset;
    oldTake.lengthInSamples = before.lengthInSamples;
    after.previousTakes.insert (after.previousTakes.begin(), oldTake);

    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Reverse region");
    um.perform (new RegionEditAction (session, engine, trackIdx, regionIdx, before, after));
    rebuildThumbIfNeeded();
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::showRegionPropertiesPopup()
{
    auto* r = region();
    if (r == nullptr) return;

    juce::PopupMenu m;
    m.addSectionHeader (juce::String::formatted ("Track %d  region %d",
                                                    trackIdx + 1, regionIdx + 1));

    // Rename / label - AlertWindow modal text input. Lambda captures
    // a SafePointer so the editor's death between menu-show and OK
    // is a no-op rather than a use-after-free.
    const auto currentLabel = r->label;
    const juce::String renameLabel = currentLabel.isEmpty()
        ? juce::String ("Add label...")
        : juce::String ("Rename label...");
    m.addItem (renameLabel,
                [safe = juce::Component::SafePointer<AudioRegionEditor> (this),
                 currentLabel]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto window = std::make_shared<juce::AlertWindow> (
                        "Audio region label",
                        "Type a label for this region:",
                        juce::AlertWindow::NoIcon);
                    window->addTextEditor ("text", currentLabel,
                                              juce::String(), false);
                    window->addButton ("OK",     1,
                                          juce::KeyPress (juce::KeyPress::returnKey));
                    window->addButton ("Cancel", 0,
                                          juce::KeyPress (juce::KeyPress::escapeKey));
                    auto* raw = window.get();
                    raw->enterModalState (true, juce::ModalCallbackFunction::create (
                        [safe, holder = window] (int result) mutable
                        {
                            auto* s = safe.getComponent();
                            if (result != 1 || s == nullptr) return;
                            auto* rr = s->region();
                            if (rr == nullptr) return;
                            const AudioRegion before = *rr;
                            AudioRegion after = before;
                            after.label = holder->getTextEditorContents ("text");
                            auto& um = s->engine.getUndoManager();
                            um.beginNewTransaction ("Rename region");
                            um.perform (new RegionEditAction (s->session, s->engine,
                                                                s->trackIdx, s->regionIdx,
                                                                before, after));
                            s->refreshStatusBarReadouts();
                            s->repaint();
                        }), false);
                });

    m.addSeparator();

    // Mute / Lock toggles - same path as the status-bar checkboxes,
    // exposed here as a discoverable surface alongside rename/colour.
    m.addItem (r->muted ? "Unmute region" : "Mute region",
                [safe = juce::Component::SafePointer<AudioRegionEditor> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto* rr = self->region();
                    if (rr == nullptr) return;
                    const AudioRegion before = *rr;
                    AudioRegion after = before;
                    after.muted = ! before.muted;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction (after.muted ? "Mute region" : "Unmute region");
                    um.perform (new RegionEditAction (self->session, self->engine,
                                                        self->trackIdx, self->regionIdx,
                                                        before, after));
                    self->muteToggle.setToggleState (after.muted, juce::dontSendNotification);
                    self->refreshStatusBarReadouts();
                    self->repaint();
                });
    m.addItem (r->locked ? "Unlock region" : "Lock region",
                [safe = juce::Component::SafePointer<AudioRegionEditor> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto* rr = self->region();
                    if (rr == nullptr) return;
                    const AudioRegion before = *rr;
                    AudioRegion after = before;
                    after.locked = ! before.locked;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction (after.locked ? "Lock region" : "Unlock region");
                    um.perform (new RegionEditAction (self->session, self->engine,
                                                        self->trackIdx, self->regionIdx,
                                                        before, after));
                    self->lockToggle.setToggleState (after.locked, juce::dontSendNotification);
                    self->refreshStatusBarReadouts();
                    self->repaint();
                });

    m.addSeparator();

    // Colour submenu - 8 swatches + "Reset to track colour".
    juce::PopupMenu colourSub;
    for (int i = 0; i < kPaletteCount; ++i)
    {
        const bool isReset = (i == 0);
        colourSub.addItem (5000 + i,
                            kPalette[i].label,
                            true,
                            isReset ? r->customColour.isTransparent()
                                    : r->customColour.getARGB() == kPalette[i].argb);
    }
    m.addSubMenu ("Color", colourSub);

    m.addSeparator();
    // Delete - no confirmation, trust the UndoManager. Closing the
    // editor on success matches "the region you were editing is gone";
    // the host pops the modal away.
    m.addItem ("Delete region",
                [safe = juce::Component::SafePointer<AudioRegionEditor> (this)]
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    auto& um = self->engine.getUndoManager();
                    um.beginNewTransaction ("Delete region");
                    um.perform (new DeleteRegionAction (self->session, self->engine,
                                                          self->trackIdx, self->regionIdx));
                    if (self->onCloseRequested) self->onCloseRequested();
                });

    // Read-only metadata footer. Disabled item so it can't be picked
    // but still reads as part of the menu.
    m.addSeparator();
    {
        const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
        const double durSec = (double) r->lengthInSamples / sr;
        const int mins = (int) (durSec / 60.0);
        const double secRem = durSec - 60.0 * mins;
        const auto info = r->file.getFileName() + "  -  "
                            + juce::String ((int) std::round (sr / 1000.0)) + " kHz  -  "
                            + juce::String (r->numChannels) + "ch  -  "
                            + juce::String (mins)
                            + ":" + juce::String (secRem, 3).paddedLeft ('0', 6);
        m.addItem (9999, info, false /*disabled*/, false);
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&propertiesButton),
        [safe = juce::Component::SafePointer<AudioRegionEditor> (this)] (int chosen)
        {
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            // Only the colour-submenu IDs land here; the per-item
            // lambdas above handle everything else.
            if (chosen < 5000 || chosen >= 5000 + kPaletteCount) return;
            auto* rr = self->region();
            if (rr == nullptr) return;
            const juce::Colour newColour { kPalette[chosen - 5000].argb };
            const AudioRegion before = *rr;
            if (before.customColour == newColour) return;
            AudioRegion after = before;
            after.customColour = newColour;
            auto& um = self->engine.getUndoManager();
            um.beginNewTransaction ("Set region colour");
            um.perform (new RegionEditAction (self->session, self->engine,
                                                self->trackIdx, self->regionIdx,
                                                before, after));
            self->refreshStatusBarReadouts();
            self->repaint();
        });
}

int AudioRegionEditor::nextRegionIndex (int delta) const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return -1;
    const auto& regs = session.track (trackIdx).regions;
    if (regionIdx < 0 || regionIdx >= (int) regs.size()) return -1;

    // Sort by (timelineStart, storageIndex) so ties are deterministic.
    // delta=+1 → smallest key strictly greater than current's key;
    // delta=-1 → largest key strictly less than current's key.
    using Key = std::pair<juce::int64, int>;
    const Key cur { regs[(size_t) regionIdx].timelineStart, regionIdx };
    int bestIdx = -1;
    Key best = (delta > 0)
        ? Key { std::numeric_limits<juce::int64>::max(), std::numeric_limits<int>::max() }
        : Key { std::numeric_limits<juce::int64>::min(), std::numeric_limits<int>::min() };
    for (int i = 0; i < (int) regs.size(); ++i)
    {
        if (i == regionIdx) continue;
        const Key k { regs[(size_t) i].timelineStart, i };
        if (delta > 0 ? (k > cur && k < best) : (k < cur && k > best))
        {
            best = k;
            bestIdx = i;
        }
    }
    return bestIdx;
}

void AudioRegionEditor::navigateRegion (int delta)
{
    const int newIdx = nextRegionIndex (delta);
    if (newIdx < 0) return;     // boundary: no neighbour, no-op
    if (onNavigateToRegion) onNavigateToRegion (trackIdx, newIdx);
}

int AudioRegionEditor::transportPlayheadX (juce::Rectangle<int> waveArea) const
{
    const auto* r = region();
    if (r == nullptr || r->lengthInSamples <= 0) return -1;
    const auto playheadTimeline = engine.getTransport().getPlayhead();
    const auto fileSample = r->sourceOffset
                              + (playheadTimeline - r->timelineStart);
    if (fileSample < r->sourceOffset
        || fileSample > r->sourceOffset + r->lengthInSamples) return -1;
    const int x = xForSample (fileSample, waveArea);
    if (x < waveArea.getX() || x > waveArea.getRight()) return -1;
    return x;
}

void AudioRegionEditor::paintTransportPlayhead (juce::Graphics& g,
                                                  juce::Rectangle<int> waveArea)
{
    const int x = transportPlayheadX (waveArea);
    if (x < 0) return;
    g.setColour (kTransportPlayhead.withAlpha (0.85f));
    g.drawVerticalLine (x, (float) waveArea.getY(), (float) waveArea.getBottom());
}

void AudioRegionEditor::timerCallback()
{
    const auto waveArea = juce::Rectangle<int> (
        0, kIconRowHeight + kRulerHeight,
        getWidth(),
        getHeight() - kIconRowHeight - kRulerHeight - kStatusBarH);
    const int x = transportPlayheadX (waveArea);
    if (x == lastPlayheadX) return;
    // Repaint just the strip the playhead vacated AND the strip it
    // entered - 4 px wide each so the line + a tiny halo redraws
    // cleanly without invalidating the whole waveform area.
    auto invalidateAt = [this, &waveArea] (int xx)
    {
        if (xx < 0) return;
        repaint (juce::Rectangle<int> (xx - 2, waveArea.getY(),
                                          5, waveArea.getHeight()));
    };
    invalidateAt (lastPlayheadX);
    invalidateAt (x);
    lastPlayheadX = x;
}
} // namespace focal
