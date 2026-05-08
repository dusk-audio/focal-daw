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

    // Currently-selected note, indexed into the region's notes vector.
    // -1 = nothing selected.
    int selectedNote = -1;

    enum class DragMode { None, MoveNote, ResizeNote, CreateNote, EditVelocity };
    DragMode dragMode = DragMode::None;
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

    // Hit-test helper for the velocity strip. Returns the index into
    // notes whose vertical bar contains x; -1 if no note matches.
    int hitTestVelocityBar (int x, juce::Rectangle<int> stripArea) const;
};
} // namespace focal
