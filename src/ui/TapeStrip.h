#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace adhdaw
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

    void mouseDown  (const juce::MouseEvent&) override;
    void mouseDrag  (const juce::MouseEvent&) override;
    void mouseUp    (const juce::MouseEvent&) override;
    void mouseMove  (const juce::MouseEvent&) override;

    static constexpr int kTrackLabelW = 44;
    static constexpr int kRulerH      = 20;
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
    // right edge); kEdgeHitPx controls how wide the resize gutter is.
    enum class RegionOp { None, Move, TrimStart, TrimEnd };
    static constexpr int kEdgeHitPx = 6;
    struct RegionHit
    {
        int track    = -1;
        int regionIdx = -1;
        RegionOp op  = RegionOp::None;
    };
    RegionHit hitTestRegion (int x, int y) const noexcept;
    void rebuildPlaybackIfStopped();
    void showRegionContextMenu (const RegionHit&, juce::Point<int> screenPos);

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
    };
    ActiveDrag drag;

    // The most recently clicked region - selection target for keyboard
    // copy/cut/paste/delete. -1 / -1 means nothing selected. Cleared on
    // undo/redo because the action might have shifted indices.
    int selectedTrack    = -1;
    int selectedRegion   = -1;
};
} // namespace adhdaw
