#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../session/Session.h"

namespace focal
{
// Piano-roll editor for one MidiRegion. Anchored on a (track, region) pair
// at construction; the component validates indices each paint so a
// concurrent record / delete that mutates the regions vector doesn't crash
// the UI - a stale view just shows nothing until the parent reopens it.
//
// Concurrency contract (matches the AudioRegion path used by TapeStrip):
//   • All mutation of session.track(t).midiRegions and the inner notes /
//     ccs vectors happens on the MESSAGE THREAD (this component's mouse
//     and keyboard handlers, plus RecordManager::stopRecording).
//   • The AUDIO THREAD reads those vectors lock-free during playback in
//     AudioEngine's per-block MIDI scheduler.
//   • A user edit while transport is rolling can race a concurrent
//     audio-thread iteration; vector reallocation on push_back would
//     invalidate the audio-thread's in-flight pointers. In practice
//     edits are infrequent enough (single user gesture per ~hundred ms)
//     and audio-thread iteration is short enough that the codebase
//     accepts this as the same "rare-but-possible" race that affects
//     AudioRegion edits. A future hardening pass would either (a) gate
//     mutations on transport-stopped, or (b) introduce a swap-load
//     atomic-pointer pattern. Not done yet.
//
// The roll is the ONE visible exception to Focal's "everything visible"
// rule (per Focal.md): a modal overlay on top of the tape-strip, dismissed
// with Esc or by clicking outside. The overlay-host is parent code; this
// component just paints the keyboard + note grid and handles edit input.
//
// Editing surface (lands across 4c-3):
//   • Click empty grid space → create a 1/4-note at that pitch+tick
//   • Click on a note → select; Backspace → delete
//   • Drag a selected note's body → move (snaps to grid)
//   • Drag a note's right edge → resize
//   • Mouse wheel → vertical scroll across the 128-key range
//   • Cmd/Ctrl + wheel → horizontal zoom (same as the main timeline)
class PianoRollComponent final : public juce::Component
{
public:
    PianoRollComponent (Session& session, int trackIndex, int regionIndex);
    ~PianoRollComponent() override;

    // Esc-to-close hook. The host (MainComponent) sets this so the user
    // can dismiss the overlay without reaching for the mouse. The roll
    // itself just calls it when it sees an Esc keypress.
    std::function<void()> onCloseRequested;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseWheelMove (const juce::MouseEvent&,
                          const juce::MouseWheelDetails&) override;

    // Visual constants exposed so the host overlay can size itself.
    static constexpr int kKeyboardWidth     = 56;
    static constexpr int kHeaderHeight      = 22;   // bar/beat ruler
    static constexpr int kNoteHeight        = 12;
    static constexpr int kNumKeys           = 128;
    static constexpr int kFullGridHeight    = kNumKeys * kNoteHeight;
    static constexpr int kVelocityStripH    = 56;   // bottom strip for vel bars
    static constexpr int kCcStripH          = 56;   // CC lane below velocity

private:
    Session& session;
    int trackIdx;
    int regionIdx;

    // Pixels per tick: drives horizontal scale. Default 24 px/quarter at
    // 480 ticks/quarter = 0.05 px/tick.
    float pixelsPerTick = 0.05f;

    // Snap resolution in ticks. 0 = no snap (free-positioning); positive =
    // round each create/move/resize tick to the nearest multiple. Default
    // 120 ticks = 1/16 note at 480 PPQN, which matches the most common
    // pop / rock entry grid. The user can change this with a number-key
    // shortcut later (Phase 5 keyboard pass).
    juce::int64 snapTicks = 120;

    // Vertical scroll. 0 = top of the 128-key range visible. Bounded so
    // we never scroll past either end. Mouse wheel adjusts.
    int scrollY = (kNumKeys - 24) * kNoteHeight / 2;  // centre near middle C

    // Horizontal scroll in pixels. 0 = region origin sits at the left of
    // the grid. Adjusted by Shift+wheel and by trackpad delta-X. Bounded
    // at zero on the left; right bound is open (a user editing past the
    // region's end is welcome - 4d's punch-stack will resize the region).
    int scrollX = 0;

    // Selected notes. Indices into region->notes; sorted ascending and
    // deduplicated. Operations that mutate notes (delete) iterate in
    // descending order so earlier indices stay valid. Empty = nothing
    // selected.
    std::vector<int> selectedNotes;

    // Anchor note for a multi-note drag - the note actually under the
    // cursor at mouseDown. Drag deltas are computed against this note;
    // every other selected note follows the same delta. Resize applies
    // only to the anchor (multi-resize is ambiguous).
    int dragAnchor = -1;

    // Per-selected-note snapshot taken at mouseDown so MoveNote can
    // apply a consistent delta to every selected note even after some
    // of them get clamped against the region boundary. Parallel to
    // selectedNotes (one entry per selected note in the same order).
    struct DragSnapshot
    {
        juce::int64 startTick     = 0;
        int         noteNumber    = 0;
        juce::int64 lengthInTicks = 0;
    };
    std::vector<DragSnapshot> dragSnapshots;

    // Active rubber-band rectangle in screen coords during BoxSelect.
    // Empty rect = no box-select in flight (paint skips the overlay).
    juce::Rectangle<int> rubberBand;

    enum class DragMode { None, MoveNote, ResizeNote, CreateNote, EditVelocity, BoxSelect, EditCcValue };
    DragMode dragMode = DragMode::None;

    // Active CC controller shown in the CC lane. Starts on CC 1 (mod
    // wheel) - the most-used continuous controller. 'L' cycles through
    // common ones (1, 7, 11, 64, 74). The lane only displays CCs whose
    // controller matches this; events on other controllers stay in the
    // region's ccs vector and pass through to the synth at playback.
    int activeCcController = 1;

    // Scale-highlight overlay. When `scale != Scale::Off`, rows whose
    // note isn't in the selected scale get a translucent dark wash so
    // in-scale notes pop visually. 'S' opens the picker popup.
    enum class Scale { Off, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian };
    Scale scale = Scale::Off;
    int   scaleRoot = 0;   // 0=C, 1=C#, ... 11=B
    // Index into region->ccs of the CC event currently being dragged in
    // the CC lane (set during EditCcValue, -1 otherwise). The vector
    // doesn't get mutated mid-drag, so the index stays stable.
    int draggedCcIdx = -1;

    // Note coloring. Pitch maps 12 semitones to 12 distinct hues (chord
    // shapes pop at a glance); Velocity is the legacy single-blue tinted
    // by 0..127; Channel is a 16-way hue cycle (useful for multi-channel
    // captures from a controller). 'C' cycles. Default Pitch matches
    // commercial DAWs.
    enum class ColorMode { Pitch, Velocity, Channel };
    ColorMode colorMode = ColorMode::Pitch;
    juce::int64 dragOriginTick    = 0;
    int         dragOriginNoteNum = 0;
    juce::int64 dragNoteStartTick = 0;
    juce::int64 dragNoteLenTicks  = 0;

    // Helpers - all return defensible values when the region pointer is
    // stale (regionIdx out of range), so paint code never branches on it.
    MidiRegion*       region();
    const MidiRegion* region() const;

    // Coordinate maps - keyboard column on the left is fixed, the note
    // grid fills the rest. Y wraps around the kNumKeys range with scrollY
    // applied; X anchors at the keyboard's right edge, ticks measured
    // from region.timelineStart (== region-local tick 0).
    int  yForNoteNumber (int noteNumber) const;
    int  noteNumberForY (int y) const;
    int  xForTick (juce::int64 tick) const;
    juce::int64 tickForX (int x) const;

    // Hit test - returns the index into region->notes hit by (x, y), or
    // -1 if the click was on empty grid space. Out-param `onRightEdge`
    // tells the caller the click was within the note's last few px (for
    // resize-vs-move drag intent).
    int hitTestNote (int x, int y, bool& onRightEdge) const;

    // Drawing helpers.
    void paintKeyboard      (juce::Graphics&, juce::Rectangle<int> area);
    void paintNoteGrid      (juce::Graphics&, juce::Rectangle<int> area);
    void paintBeatRuler     (juce::Graphics&, juce::Rectangle<int> area);
    void paintNotes         (juce::Graphics&, juce::Rectangle<int> area);
    void paintVelocityStrip (juce::Graphics&, juce::Rectangle<int> area);
    void paintCcStrip       (juce::Graphics&, juce::Rectangle<int> area);

    // Hit-test for CC bars in the CC lane. Returns the index into
    // region->ccs whose tick maps to a screen-x within kHitSlopPx
    // of the click, restricted to events on activeCcController.
    int hitTestCcBar (int x, juce::Rectangle<int> stripArea) const;

    // Hit-test helper for the velocity strip. Returns the index into
    // notes whose vertical bar contains x; -1 if no note matches.
    int hitTestVelocityBar (int x, juce::Rectangle<int> stripArea) const;

    // Resolve the fill colour for a note under the active colorMode.
    // Selection state is the caller's concern (paint applies the
    // selection highlight on top); this only returns the base fill.
    juce::Colour colourForNote (const MidiNote& n) const noexcept;

    // Selection helpers. Selection is stored sorted/deduped so set
    // operations (toggle / contains) are O(log n) on a vector of ints.
    bool isNoteSelected (int idx) const noexcept;
    void clearSelection();
    void selectOnly (int idx);
    void toggleSelected (int idx);
    void addToSelection (int idx);
    // Snapshot every selected note's start / pitch / length into
    // dragSnapshots so MoveNote can apply a consistent group delta.
    // Set dragAnchor to `anchorIdx` (the note under the cursor).
    void beginGroupDrag (int anchorIdx);
    // Shift every selected note by (deltaTicks, deltaPitch). Clamps
    // the delta against the group's bounding box so no note ends up
    // out of [0, lengthInTicks] or [0, 127].
    void applyGroupMove (juce::int64 deltaTicks, int deltaPitch);
    // Transpose every selected note by `semitones`, clamped per note
    // to [0, 127]. Used by Up/Down arrow handlers.
    void transposeSelected (int semitones);

    // Quantize selected notes (or all notes if nothing selected) to
    // `gridTicks`, blended at `strength` 0..1. strength=1.0 fully
    // snaps to grid; strength=0.5 moves halfway from original tick to
    // snapped tick (humanises without losing the original feel).
    void quantizeSelected (juce::int64 gridTicks, float strength);

    // Velocity helpers for selected notes (or all notes if nothing
    // selected, mirroring quantizeSelected's "no-selection = whole-
    // region" rule). humanizeVelocity adds a random offset in
    // [-rangePercent..+rangePercent] of full-scale (127) per note;
    // setVelocityFor clamps every note to a fixed value. Both clamp
    // the result to [1, 127].
    void humanizeVelocity (int rangePercent);
    void setVelocityFor (int value);
    void showVelocityPopup();

    // Glue every selected same-pitch + contiguous note into one. Two
    // notes are "contiguous" when the second's startTick falls at or
    // before the first's endTick. The lower-index note absorbs the
    // others; absorbed notes are erased in descending index order so
    // earlier indices stay valid. Selection is cleared on success
    // because the indices we held are now stale.
    void glueSelectedNotes();

    // Shift every selected note's startTick by deltaTicks, clamped so
    // no note ends up out of [0, lengthInTicks - lengthInTicks_of_note].
    // Used by Left/Right arrow nudge handlers.
    void nudgeSelectedTicks (juce::int64 deltaTicks);

    // True if `noteNumber` is in the active scale (or scale is Off).
    bool isInScale (int noteNumber) const noexcept;

    // 'Q' / 'S' popup launchers - show the menu and apply the chosen
    // action. Both run on the message thread inside keyPressed.
    void showQuantizePopup();
    void showScalePopup();
};
} // namespace focal
