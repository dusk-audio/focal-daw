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
class TapeStrip final : public juce::Component, private juce::Timer
{
public:
    TapeStrip (Session& sessionRef, AudioEngine& engineRef);
    ~TapeStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int kTrackLabelW = 44;
    static constexpr int kRulerH      = 20;
    static constexpr int kTrackRowH   = 14;
    static constexpr int kRowGap      = 1;

    // Total pixel height of the strip when sized to the natural row count.
    static int naturalHeight();

private:
    void timerCallback() override;

    juce::Rectangle<int> labelColumnBounds() const noexcept;
    juce::Rectangle<int> rulerBounds() const noexcept;
    juce::Rectangle<int> tracksColumnBounds() const noexcept;
    juce::Rectangle<int> rowBounds (int trackIdx) const noexcept;

    double pixelsPerSecond() const noexcept;
    juce::int64 sampleAtX (int x) const noexcept;
    int xForSample (juce::int64 s) const noexcept;

    Session& session;
    AudioEngine& engine;

    juce::int64 lastPlayhead = -1;
};
} // namespace adhdaw
