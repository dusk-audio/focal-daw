#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>

namespace focal
{
// Classic analog VU meter - cream face, swept needle, red overload zone past
// 0 VU. Reads one or two std::atomic<float> linear-RMS sources (audio-thread
// integrated at 300 ms tau, matching IEC 60268-17 and the TapeMachine
// plugin's internal VU integrator). The UI thread converts to VU dB
// (20·log10(rms) - referenceDbFs) and runs the mechanical ballistics
// (spring + RC) on the needle position.
//
// Reference level (dBFS that maps to 0 VU) is configurable; default -18 dBFS
// matches the common DAW convention. At 0 dBFS the needle pegs hard right
// (well past +3) which visually communicates clipping the same way an analog
// meter would slam its endstop. Two atoms = stereo (two needles overlaid on
// one face); one atom = single-channel (e.g. master bus summed RMS, or a
// caller that already collapses L/R upstream).
class AnalogVuMeter final : public juce::Component, private juce::Timer
{
public:
    // leftRmsAtom must be non-null; rightRmsAtom may be null for mono. Atoms
    // hold linear RMS amplitude (not dB) so the UI does one log10 + position
    // map per refresh tick rather than per sample. Both pointed-to atoms
    // must outlive this component; in Focal these live on Session params
    // (BusParams / MasterBusParams), constructed before any UI and destructed
    // after, so the lifetime contract holds implicitly.
    AnalogVuMeter (const std::atomic<float>* leftRmsAtom,
                    const std::atomic<float>* rightRmsAtom = nullptr);
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

    const std::atomic<float>* leftRms  = nullptr;
    const std::atomic<float>* rightRms = nullptr;
    float referenceDbFs = -18.0f;

    // Needle position per channel as a 0..1 fraction of full deflection
    // (0 = -20 VU, 1 = +3 VU). Driven on the UI Timer with IEC 60268-17
    // ballistics + a damped-spring overshoot, matching the TapeMachine
    // plugin's meter so a single signal reads identically on both.
    float needlePosL = 0.0f;
    float needlePosR = 0.0f;
    float needleVelL = 0.0f;
    float needleVelR = 0.0f;

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
