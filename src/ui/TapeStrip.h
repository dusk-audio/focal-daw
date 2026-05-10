#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace focal
{
// Phase 2 minimum tape strip:
//   - 16 horizontal rows (one per track), color-coded with track accent
//   - Recorded regions drawn as rounded blocks per row
//   - Vertical playhead line that follows the transport
//   - Time ruler at the top (seconds)
//   - Click anywhere on a row to seek the playhead to that position
// Region drag/split/trim, markers, loop brackets, and horizontal scroll all
// arrive in later phases per the spec; this component focuses on visibility.
class TapeStrip final : public juce::Component,
                         private juce::Timer,
                         private juce::ChangeListener
{
public:
    TapeStrip (Session& sessionRef, AudioEngine& engineRef);
    ~TapeStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseMove        (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int kTrackLabelW = 44;
    // Ruler is split into three vertical bands:
    //   y=0..kRulerTickBandH        - time-label row + tick marks
    //   y=kRulerTickBandH..kRulerH  - markers row + loop/punch pills
    // Loop/punch's solid bracket bar paints the bottom 4 px of the lower
    // band so it visually attaches the pills to the timeline.
    static constexpr int kRulerTickBandH = 14;
    static constexpr int kRulerPillBandH = 16;
    static constexpr int kRulerH         = kRulerTickBandH + kRulerPillBandH;  // 30
    static constexpr int kTrackRowH   = 14;
    static constexpr int kRowGap      = 1;

    // Total pixel height of the strip when sized to the natural row count.
    static int naturalHeight();

    // Clipboard / selection-driven editing surface, called from
    // MainComponent's keyboard handler. Each returns true if the operation
    // happened (so the caller can decide whether to swallow the keypress).
    // All edits go through the engine's UndoManager.
    bool copySelectedRegion();
    bool cutSelectedRegion();
    bool pasteAtPlayhead();
    bool deleteSelectedRegion();
    // Split the selected audio region at the current transport playhead.
    // No-op (returns false) when nothing is selected, the playhead is
    // outside the region's range, or the region is too short to split.
    // Same SplitRegionAction the right-click menu uses, so undo/redo
    // behaviour matches.
    bool splitSelectedAtPlayhead();
    // Duplicate the selected region. The clone is positioned right
    // after the original (timelineStart + lengthInSamples). Routes
    // through PasteRegionAction so undo/redo work. Returns false if
    // nothing is selected.
    bool duplicateSelectedRegion();
    // Nudge the selected region's timelineStart by deltaSamples,
    // routed through RegionEditAction so undo/redo work. Clamps so
    // the region can't move past the timeline origin. deltaSamples is
    // signed - negative values move the region earlier.
    bool nudgeSelectedRegion (juce::int64 deltaSamples);

    // Double-click hooks. Single-click is reserved for direct
    // manipulation (move / trim / fade / gain on audio; future drag
    // for MIDI). Double-click opens the dedicated editor modal:
    // PianoRollComponent for MIDI, AudioRegionEditor for audio. Both
    // share the same gesture so users can rely on one mental model
    // ("double-click any region to edit it").
    std::function<void (int trackIdx, int regionIdx)> onMidiRegionDoubleClicked;
    std::function<void (int trackIdx, int regionIdx)> onAudioRegionDoubleClicked;

    // Selected track index (the track that owns the most-recently-clicked
    // region) or -1 if nothing is selected. Used by keyboard shortcuts in
    // MainComponent to target arm / solo / mute toggles at "the focus
    // track". Returning the internal selection state avoids inventing a
    // separate selection model.
    int  getSelectedTrack() const noexcept { return selectedTrack; }

    // Set track focus from outside (e.g. ChannelStripComponent click).
    // Clears any region selection since the user's gesture wasn't
    // region-specific. Repaints so the highlighted row updates.
    void setSelectedTrack (int t) noexcept;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::Rectangle<int> labelColumnBounds() const noexcept;
    juce::Rectangle<int> rulerBounds() const noexcept;
    juce::Rectangle<int> tracksColumnBounds() const noexcept;
    juce::Rectangle<int> rowBounds (int trackIdx) const noexcept;

    double pixelsPerSecond() const noexcept;
    juce::int64 sampleAtX (int x) const noexcept;
    int xForSample (juce::int64 s) const noexcept;

    // Region hit-testing for the editing surface. `op` reports which
    // sub-area of the region the cursor lies over (body / left edge /
    // right edge / top-corner fade handles); kEdgeHitPx controls how
    // wide the resize gutter is, kFadeHandleH the height of the top
    // band reserved for fade handles.
    enum class RegionOp { None, Move, TrimStart, TrimEnd, TakeBadge, FadeIn, FadeOut, AdjustGain };
    static constexpr int kFadeHandleH = 6;
    static constexpr int kFadeHitPx   = 5;
    static constexpr int kEdgeHitPx = 6;
    struct RegionHit
    {
        int track    = -1;
        int regionIdx = -1;
        RegionOp op  = RegionOp::None;
    };
    RegionHit hitTestRegion (int x, int y) const noexcept;
    // Marker flag hit-test. Returns the index of the marker whose flag
    // sits under (x, y), or -1. Tested only inside the ruler band.
    int hitTestMarker (int x, int y) const noexcept;

    // Loop / punch bracket hit-test. Pills (in/out) and the bar between
    // them are draggable: pill drags reposition that endpoint; bar drags
    // translate the whole range while preserving its length.
    enum class BracketHit
    {
        None,
        LoopIn, LoopOut, LoopBar,
        PunchIn, PunchOut, PunchBar,
    };
    BracketHit hitTestBracket (int x, int y) const noexcept;
    void rebuildPlaybackIfStopped();
    void showRegionContextMenu (const RegionHit&, juce::Point<int> screenPos);
    // Right-click context for MIDI regions. Smaller than the audio
    // version (no edge gutters, no fade handles, no take badge yet)
    // - just Rename and Color. Mutates via midiRegions.currentMutable()
    // since MIDI regions don't currently have an undoable edit-action
    // surface; matches how the piano roll handles per-note edits.
    void showMidiRegionContextMenu (int trackIdx, int regionIdx,
                                       juce::Point<int> screenPos);

    Session& session;
    AudioEngine& engine;

    juce::int64 lastPlayhead = -1;

    // Caches so the timer can detect track color / name changes and repaint.
    // Without these, the strip's only repaint trigger is playhead motion, so
    // renaming a track or changing its color in the mixer wouldn't reflect
    // in the summary until the next play.
    std::array<juce::String, Session::kNumTracks> lastNames;
    std::array<juce::Colour, Session::kNumTracks> lastColours;

    // Loop / punch state caches - same pattern, so a TransportBar toggle
    // (or a session load) repaints the brackets without the user having to
    // move the playhead.
    bool        lastLoopEnabled  = false;
    juce::int64 lastLoopStart    = -1;
    juce::int64 lastLoopEnd      = -1;
    bool        lastPunchEnabled = false;
    juce::int64 lastPunchIn      = -1;
    juce::int64 lastPunchOut     = -1;

    // Transport-state cache so the timer can full-repaint on
    // Stopped <-> Recording transitions - the existing thin-band
    // playhead repaint isn't wide enough to cover the live-recording
    // overlay's initial paint at the moment of Record-press.
    bool        lastIsRecording  = false;

    // In-flight region drag. Captured on mouseDown when the click hits a
    // region's body or edge gutter; updated on mouseDrag; finalised on
    // mouseUp. Only one drag can be active at a time.
    struct ActiveDrag
    {
        int track     = -1;
        int regionIdx = -1;
        RegionOp op   = RegionOp::None;
        juce::int64 mouseDownSample = 0;
        juce::int64 origTimelineStart = 0;
        juce::int64 origLength        = 0;
        juce::int64 origSourceOffset  = 0;
        juce::int64 origFadeIn        = 0;
        juce::int64 origFadeOut       = 0;
        float       origGainDb        = 0.0f;

        // Per-additional-selection orig state captured at mouseDown
        // for group Move / AdjustGain drags. Empty vector = single-
        // region drag (only the anchor). track / regionIdx point
        // back into Session, NOT into additionalSelections - the
        // latter can be reordered between mouseDown and mouseUp by
        // a concurrent record / undo, so identifying by (track,
        // regionIdx) at capture time is the stable form.
        struct AdditionalOrig
        {
            int track;
            int regionIdx;
            juce::int64 origTimelineStart;
            float       origGainDb;
        };
        std::vector<AdditionalOrig> additional;
    };
    ActiveDrag drag;

    // In-flight ruler selection. Click+drag on the ruler defines a
    // candidate range that's painted as a neutral highlight. On mouseUp
    // we offer a menu ("Set loop here" / "Set punch in/out here") so the
    // user picks what the range is for - dragging itself doesn't
    // commit anything to transport state.
    struct RulerSelection
    {
        bool active     = false;
        juce::int64 originSample  = 0;
        juce::int64 currentSample = 0;
    };
    RulerSelection rulerSelection;

    // In-flight marker drag. Click on a flag captures the marker; if the
    // user moves before releasing, we treat it as a drag (update marker
    // position); otherwise it's a click (seek to marker on mouseUp).
    struct MarkerDrag
    {
        bool active   = false;
        bool moved    = false;     // true once drag delta exceeds threshold
        int  index    = -1;
        juce::int64 originSample = 0;
        juce::int64 mouseDownSample = 0;
    };
    MarkerDrag markerDrag;

    // In-flight loop / punch bracket drag. type identifies which endpoint
    // (or whole-bar) is being moved; origStart/origEnd are the bracket's
    // pre-drag bounds, captured at mouseDown so a "move whole bar" drag
    // can translate by delta without compounding rounding.
    struct BracketDrag
    {
        bool       active = false;
        BracketHit type   = BracketHit::None;
        juce::int64 mouseDownSample = 0;
        juce::int64 origStart = 0;
        juce::int64 origEnd   = 0;
    };
    BracketDrag bracketDrag;

    // Primary / anchor selection - the most-recently-clicked region.
    // Single-region operations (clipboard, paste, anchor for drag deltas)
    // act on this; group operations also include `additionalSelections`.
    // -1 / -1 means nothing selected. Cleared on undo/redo because the
    // action might have shifted indices.
    int selectedTrack    = -1;
    int selectedRegion   = -1;

    // Extra regions selected via Shift / Cmd-click on top of the
    // primary. The primary is NOT included in this vector - the full
    // selection set is `(selectedTrack, selectedRegion)` ∪ this.
    // Sorted-and-deduped to keep group ops (delete, nudge, gain) from
    // double-iterating the same region. Cleared whenever the primary
    // collapses to "nothing" (plain-click on empty space, undo, etc.).
    struct RegionId
    {
        int track;
        int regionIdx;
        bool operator== (const RegionId& other) const noexcept
        {
            return track == other.track && regionIdx == other.regionIdx;
        }
        bool operator< (const RegionId& other) const noexcept
        {
            return track < other.track
                || (track == other.track && regionIdx < other.regionIdx);
        }
    };
    std::vector<RegionId> additionalSelections;

    // True if (track, idx) is the primary OR appears in additional.
    bool isRegionSelected (int track, int idx) const noexcept;
    // Full selection list (primary first if set, then additional).
    std::vector<RegionId> allSelectedRegions() const;
    // Reset both primary and additional. Used by changeListenerCallback
    // (undo/redo may have shifted indices) and by plain-click on empty
    // timeline.
    void clearAllSelections() noexcept;
    // Add (or remove if already present) a region to the selection.
    // Used by Shift / Cmd-click to extend the selection without
    // collapsing back to a single anchor.
    void toggleRegionSelected (int track, int idx);

    // Hover state - set in mouseMove, cleared in mouseExit. Drives
    // affordance visibility (fade handles only paint on the hovered
    // or selected region, otherwise the timeline reads cluttered).
    int hoveredTrack  = -1;
    int hoveredRegion = -1;
    void mouseExit (const juce::MouseEvent&) override;

    // Rubber-band box-select state. Active during a Shift / Cmd +
    // drag from empty track-row space. Origin is captured on
    // mouseDown; current is updated each mouseDrag; on mouseUp every
    // audio region whose painted rect intersects the box gets added
    // to the multi-selection. MIDI regions are skipped (their
    // click-to-open-roll path is separate). Rectangle stored in
    // screen coords so the painter and intersection test share the
    // same frame of reference.
    bool                  rubberBandActive = false;
    juce::Rectangle<int>  rubberBand;
};
} // namespace focal
