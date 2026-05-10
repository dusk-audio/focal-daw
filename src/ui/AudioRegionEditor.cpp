#include "AudioRegionEditor.h"
#include "../engine/AudioEngine.h"
#include "../session/RegionEditActions.h"
#include <cmath>

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
    wireIcon (propertiesButton, "Region properties\xe2\x80\xa6", [this] { showPropertiesPopup(); });

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
}

AudioRegionEditor::~AudioRegionEditor()
{
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
    const float w = (float) juce::jmax (1, area.getWidth() - 8);
    pixelsPerSample = w / (float) r->lengthInSamples;
    scrollSamples   = 0;
    editCursorSample = r->sourceOffset;
}

int AudioRegionEditor::xForSample (juce::int64 absSample,
                                      juce::Rectangle<int> area) const
{
    const auto* r = region();
    if (r == nullptr) return area.getX();
    const auto rel = absSample - r->sourceOffset - scrollSamples;
    return area.getX() + 4 + (int) std::round ((double) rel * pixelsPerSample);
}

juce::int64 AudioRegionEditor::sampleForX (int x, juce::Rectangle<int> area) const
{
    const auto* r = region();
    if (r == nullptr) return 0;
    if (pixelsPerSample <= 0.0f) return r->sourceOffset;
    const auto rel = (juce::int64) std::round (
        (double) (x - area.getX() - 4) / pixelsPerSample);
    return juce::jlimit<juce::int64> (
        r->sourceOffset,
        r->sourceOffset + r->lengthInSamples,
        r->sourceOffset + scrollSamples + rel);
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

        // Trim grab strips - 8 px wide bright bands at left / right edges.
        g.setColour (juce::Colour (0xffb0b0b8));
        g.fillRect (trimStartRect (waveArea));
        g.fillRect (trimEndRect   (waveArea));
    }

    paintEditCursor    (g, waveArea);

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
    const double bpm = juce::jmax (1.0, (double) session.tempoBpm.load (std::memory_order_relaxed));
    const int beatsPerBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
    const double samplesPerBeat = sr * 60.0 / bpm;
    const double samplesPerBar  = samplesPerBeat * beatsPerBar;

    // Compute the slice's absolute timeline position so bar numbers
    // are relative to the project (matches what the user sees on the
    // tape lane behind the modal).
    const double sliceStartTimeline = (double) r->timelineStart;
    const double firstBar = std::floor (sliceStartTimeline / samplesPerBar);
    const double sliceEndTimeline   = sliceStartTimeline + (double) r->lengthInSamples;
    const double lastBar            = std::ceil (sliceEndTimeline / samplesPerBar);

    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    for (double bar = firstBar; bar <= lastBar; bar += 1.0)
    {
        const auto barTimeline = (juce::int64) std::round (bar * samplesPerBar);
        // Convert timeline sample → absolute file sample inside the slice.
        const auto absInSlice = r->sourceOffset + (barTimeline - r->timelineStart);
        if (absInSlice < r->sourceOffset
            || absInSlice > r->sourceOffset + r->lengthInSamples) continue;
        const int x = xForSample (absInSlice, area);
        if (x < area.getX() || x > area.getRight()) continue;
        g.setColour (kBarLine);
        g.drawVerticalLine (x, (float) area.getY(), (float) area.getBottom());
        g.setColour (kHeaderText);
        g.drawText (juce::String ((int) bar + 1),
                     juce::Rectangle<int> (x + 3, area.getY(), 40, area.getHeight()),
                     juce::Justification::centredLeft, false);
    }
}

void AudioRegionEditor::paintWaveform (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto* r = region();
    if (r == nullptr || thumb == nullptr) return;

    const double sr = juce::jmax (1.0, engine.getCurrentSampleRate());
    const double t0 = (double) (r->sourceOffset + scrollSamples) / sr;
    const double t1 = t0 + (double) r->lengthInSamples / sr;

    auto inset = area.reduced (4, 4);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (inset);

    const auto waveColour = (r->customColour.isTransparent()
                                ? kWaveformFill
                                : r->customColour);
    g.setColour (waveColour.withMultipliedBrightness (1.05f));
    if (thumb->isFullyLoaded() || thumb->getNumChannels() > 0)
    {
        thumb->drawChannels (g, inset, t0, t1, 0.95f);
    }
    else
    {
        // Loading - centre line so the area doesn't read as empty
        // while the thumbnail builds.
        g.drawHorizontalLine (inset.getCentreY(),
                                (float) inset.getX(), (float) inset.getRight());
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
        showContextMenu();
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
        editCursorSample = sampleForX (e.x, waveArea);
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
        refreshStatusBarReadouts();
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
                dragMode == DragMode::FadeIn   ? "Fade-in"   :
                dragMode == DragMode::FadeOut  ? "Fade-out"  :
                dragMode == DragMode::Gain     ? "Region gain" :
                dragMode == DragMode::TrimStart? "Trim start" :
                                                   "Trim end");
            um.perform (new RegionEditAction (
                session, engine, trackIdx, regionIdx,
                regionAtDragStart, afterState));
        }
    }
    dragMode = DragMode::None;
    refreshStatusBarReadouts();
    repaint();
}

void AudioRegionEditor::showContextMenu()
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
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
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
        if (onCloseRequested) onCloseRequested();
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
    inner.removeFromLeft (gap);
    place (zoomFitButton);
    place (propertiesButton);
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
        positionLabel.setText ("pos " + formatBarBeatTickAt (editCursorSample, *r, sr, bpm, bpb),
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
        infoLabel.setText (juce::String (channels) + "ch \xc2\xb7 "
                              + juce::String ((int) std::round (sr / 1000.0)) + " kHz \xc2\xb7 "
                              + juce::String (mins) + ":"
                              + juce::String (secs, 3),
                              juce::dontSendNotification);
        muteToggle.setToggleState (r->muted,  juce::dontSendNotification);
        lockToggle.setToggleState (r->locked, juce::dontSendNotification);
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

void AudioRegionEditor::showPropertiesPopup()
{
    showContextMenu();
}
} // namespace focal
