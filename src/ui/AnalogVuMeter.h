#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>

namespace focal
{
// Classic analog VU meter - cream face, swept needle, red overload zone past
// 0 VU. Reads from one or two std::atomic<float> dB sources (peak post-DSP);
// the slow VU ballistics smooth peak input into something close to RMS.
//
// Reference level (dBFS that maps to 0 VU) is configurable; default -18 dBFS
// matches the common DAW convention. At 0 dBFS the needle pegs hard right
// (well past +3) which visually communicates clipping the same way an analog
// meter would slam its endstop. Two atoms = stereo (two needles overlaid on
// one face); one atom = single-channel (e.g. master bus summed peak, or a
// caller that already collapses L/R upstream).
class AnalogVuMeter final : public juce::Component, private juce::Timer
{
public:
    // leftDbAtom must be non-null; rightDbAtom may be null for mono. Both
    // pointed-to atoms must outlive this component. In Focal these are owned
    // by Session params (Track / MasterBusParams etc.), which are constructed
    // before any UI and destructed after, so the lifetime contract holds
    // implicitly. Don't pass atoms owned by transient UI state.
    AnalogVuMeter (const std::atomic<float>* leftDbAtom,
                    const std::atomic<float>* rightDbAtom = nullptr);
    ~AnalogVuMeter() override;

    // dBFS that maps to 0 VU on the scale. Common values: -18 (broadcast /
    // DAW), -20 (cinema), -14 (loudness-normalised streaming). The face is
    // re-rendered when this changes so the labels remain consistent.
    void setReferenceLevelDb (float refDb);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void rebuildCachedFace();

    const std::atomic<float>* leftDb  = nullptr;
    const std::atomic<float>* rightDb = nullptr;
    float referenceDbFs = -18.0f;

    // Smoothed needle position per channel. Drives display only - the
    // input atoms are peak-of-block from the audio thread and these are
    // updated on the UI Timer with VU ballistics (1-pole LPF, ~65 ms
    // time constant -> ~300 ms rise to 99 %, matching the IEC VU spec).
    float displayedDbL = -60.0f;
    float displayedDbR = -60.0f;

    // Pre-rendered face. Rebuilt on resize/reference-change; the per-frame
    // paint just blits this and overlays the needle(s). Cheaper than
    // re-rasterising the arcs and labels at 30 Hz.
    juce::Image cachedFace;

    // Needle pivot is below the visible face (off-screen) so the needle
    // sweeps a shallow arc across the top of the meter, matching the
    // mechanics of a real analog galvanometer. Cached at resize.
    juce::Point<float> pivot;
    float arcRadius   = 0.0f;
    float halfArcDeg  = 45.0f;   // each side from straight-up
};
} // namespace focal
