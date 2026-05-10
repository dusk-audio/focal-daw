#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
// juce_dsp must precede juce_audio_utils so the explicit
// SIMDNativeOps<int64> specialisation is visible before
// juce_audio_processors (transitively pulled by juce_audio_utils)
// instantiates SIMDRegister<int64> via its private dependencies.
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include "../session/Session.h"

namespace focal
{
class AudioEngine;

// Modal editor for one AudioRegion. Sister to PianoRollComponent. Shows
// the slice's waveform via juce::AudioThumbnail and overlays the fade
// envelopes + edit cursor. Phase 1 is display-only; later phases add
// drag-to-edit, the icon row, the bottom status bar, and destructive
// ops (normalize / reverse).
//
// Lifecycle contract: instances are owned by MainComponent. Construction
// happens on the message thread; the underlying AudioRegion may be
// mutated by other UI code (or by RecordManager on the message thread)
// while the editor is open. region() validates the (track, region)
// indices on every access; a stale view paints nothing rather than
// crashing.
class AudioRegionEditor final : public juce::Component,
                                  private juce::ChangeListener
{
public:
    AudioRegionEditor (Session& session, AudioEngine& engine,
                          int trackIndex, int regionIndex);
    ~AudioRegionEditor() override;

    // Esc-to-close hook. The host (MainComponent) sets this so the
    // user can dismiss the overlay without reaching for the mouse.
    std::function<void()> onCloseRequested;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove      (const juce::MouseEvent&) override;
    void mouseDown      (const juce::MouseEvent&) override;
    void mouseDrag      (const juce::MouseEvent&) override;
    void mouseUp        (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&,
                            const juce::MouseWheelDetails&) override;
    bool keyPressed     (const juce::KeyPress&) override;

    // Visual constants exposed so the host overlay can size itself.
    static constexpr int kIconRowHeight = 36;   // top - 8 action icons
    static constexpr int kRulerHeight   = 28;   // bar.beat ruler under icon row
    static constexpr int kStatusBarH    = 30;   // bottom - readouts + props
    static constexpr int kKeyboardWidth = 0;

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;

    AudioRegion*       region();
    const AudioRegion* region() const;

    // Cheap to construct (just registers basic file formats). A
    // dedicated manager per editor instance avoids cross-component
    // coupling - matches MasteringView's pattern in
    // [src/ui/MasteringView.h](src/ui/MasteringView.h).
    juce::AudioFormatManager formatManager;
    // Cache size of 8 thumbs is plenty - we only ever show one
    // region at a time, but leaving a few cached entries keeps
    // back-and-forth take cycling snappy.
    juce::AudioThumbnailCache thumbCache { 8 };
    std::unique_ptr<juce::AudioThumbnail> thumb;
    juce::File loadedFile;

    // pixelsPerSample maps sample position in the slice to x-pixels
    // in the waveform area. Recomputed in resized() to fit the
    // entire slice; mouseWheelMove (Cmd+wheel) zooms cursor-anchored.
    float pixelsPerSample = 0.0f;
    juce::int64 scrollSamples = 0;
    juce::int64 editCursorSample = 0;   // absolute file sample, [sourceOffset, sourceOffset+length)

    // Drag state. Mirrors TapeStrip's pattern in [src/ui/TapeStrip.cpp]:
    // mouseDown captures `regionAtDragStart` so mouseUp can submit a
    // RegionEditAction(before=regionAtDragStart, after=current). MoveCursor
    // is not undoable; the rest are.
    enum class DragMode { None, FadeIn, FadeOut, Gain, TrimStart, TrimEnd, MoveCursor };
    DragMode dragMode = DragMode::None;
    AudioRegion regionAtDragStart;        // before-state for RegionEditAction
    juce::int64 dragOriginSample  = 0;    // edit-cursor anchor for relative drags
    int         dragOriginMouseY  = 0;    // for the gain drag (vertical)
    float       dragOriginGainDb  = 0.0f;

    // Hit-test helpers - return the rect of each draggable handle within
    // the waveform area, or empty when the handle isn't currently visible
    // (e.g. fade handles when the region is locked / too narrow). Each
    // handle has a generous grab slop wider than the painted glyph.
    juce::Rectangle<int> fadeInHandleRect  (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> fadeOutHandleRect (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> trimStartRect     (juce::Rectangle<int> waveArea) const;
    juce::Rectangle<int> trimEndRect       (juce::Rectangle<int> waveArea) const;
    int                   gainLineY        (juce::Rectangle<int> waveArea) const;

    // Right-click context menu (split / reset gain / reset fades / mute /
    // lock / colour / label). All actions submitted through the engine's
    // UndoManager so Cmd+Z reverts.
    void showContextMenu();

    // Reaper-style top icon row. Compact circular buttons mirroring
    // TransportIconButton / PianoRollComponent::IconButton aesthetic.
    class IconButton final : public juce::Button
    {
    public:
        enum class Glyph { Undo, Redo, Split, Normalize, Reverse, TakeCycle, ZoomFit, Properties };
        IconButton (const juce::String& name, Glyph g);
        void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;
    private:
        Glyph glyph;
    };
    IconButton undoButton       { "Undo",       IconButton::Glyph::Undo };
    IconButton redoButton       { "Redo",       IconButton::Glyph::Redo };
    IconButton splitButton      { "Split",      IconButton::Glyph::Split };
    IconButton normalizeButton  { "Normalize",  IconButton::Glyph::Normalize };
    IconButton reverseButton    { "Reverse",    IconButton::Glyph::Reverse };
    IconButton takeCycleButton  { "Take",       IconButton::Glyph::TakeCycle };
    IconButton zoomFitButton    { "Zoom fit",   IconButton::Glyph::ZoomFit };
    IconButton propertiesButton { "Properties", IconButton::Glyph::Properties };

    // Reaper-style bottom status-bar children. Real interactive widgets,
    // not paint-only - JUCE handles dispatch / hover / focus.
    juce::Label        positionLabel;
    juce::Label        gainLabel;
    juce::Label        fadeLabel;
    juce::Label        infoLabel;
    juce::ToggleButton muteToggle;
    juce::ToggleButton lockToggle;

    // Layout helpers.
    void layoutIconRow   (juce::Rectangle<int>);
    void layoutStatusBar (juce::Rectangle<int>);
    void refreshStatusBarReadouts();

    // Action handlers. Each finalises through engine.getUndoManager()
    // so Cmd+Z reverts cleanly. normalize is non-destructive (gainDb
    // adjustment); reverse is destructive (rewrites the source file).
    void normalizeRegion();
    void reverseRegion();
    void cycleTake();
    void zoomFit();
    void showPropertiesPopup();
    void splitAtCursor();

    void rebuildThumbIfNeeded();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // Paint helpers - each takes the band's screen rect.
    void paintRuler         (juce::Graphics&, juce::Rectangle<int> area);
    void paintWaveform      (juce::Graphics&, juce::Rectangle<int> area);
    void paintFadeEnvelopes (juce::Graphics&, juce::Rectangle<int> area);
    void paintEditCursor    (juce::Graphics&, juce::Rectangle<int> area);

    // Sample <-> pixel mapping. `area` is the waveform band's screen
    // rect. Samples are absolute file-sample positions (the slice
    // starts at sourceOffset, ends at sourceOffset + lengthInSamples).
    int  xForSample (juce::int64 absSample, juce::Rectangle<int> area) const;
    juce::int64 sampleForX (int x, juce::Rectangle<int> area) const;

    // Recompute pixelsPerSample so the slice exactly fills `area`.
    void zoomFitToArea (juce::Rectangle<int> area);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRegionEditor)
};
} // namespace focal
