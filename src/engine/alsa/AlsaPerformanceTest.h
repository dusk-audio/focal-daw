#pragma once

#include <juce_core/juce_core.h>

namespace focal
{
// Performance + stability test harness for the Focal-owned ALSA backend.
// Tier 1: drives an AlsaAudioIODevice directly with a measuring callback,
// no JUCE AudioDeviceManager involvement, no audible content, no special
// hardware setup beyond a real ALSA hw: device.
//
// Reports per-buffer-size:
//   - xrun count over a fixed-duration silent run
//   - per-callback wall-clock time: mean, p95, p99, max
//   - "verdict" against the period's audio-time budget
//
// Plus open/close cycle stress (catches resource leaks) and start/stop
// race (catches deadlock or inconsistent state).
//
// Hooked via FOCAL_RUN_ALSA_PERF=1; see runHeadlessAlsaPerfTest in
// FocalApp.cpp for the env var surface.
class AlsaPerformanceTest
{
public:
    struct Result
    {
        juce::String testName;
        juce::String configuration;     // "buf=512 rate=48000"
        bool         passed = false;

        int    xruns           = 0;
        double budgetMs        = 0.0;   // (period / sampleRate) * 1000
        double meanCallbackMs  = 0.0;
        double p95CallbackMs   = 0.0;
        double p99CallbackMs   = 0.0;
        double maxCallbackMs   = 0.0;
        int    callbackCount   = 0;

        juce::String verdict;           // "SAFE" / "MARGINAL" / "UNSAFE" / "FAIL"
        juce::String details;
    };

    struct Options
    {
        juce::String     deviceId        = "hw:0,0";
        unsigned int     sampleRate      = 48000;
        int              durationMs      = 5000;     // per buffer-size step
        int              fakeDspLoadUs   = 0;        // synthetic CPU work in callback
        int              openCloseCycles = 50;
        int              startStopCycles = 20;
        bool             runLoopback     = false;    // require user-supplied loopback path
        juce::Array<int> bufferSizes;                // empty -> use default ladder
    };

    // Loopback round-trip measurement. Only meaningful when there is a
    // physical loopback cable plugged from the device's first output to its
    // first input, OR snd-aloop is loaded and the deviceId points at the
    // loopback card. Without loopback the burst goes nowhere and signalDetected
    // stays false.
    struct LoopbackResult
    {
        bool         signalDetected = false;
        int          latencySamples = -1;
        double       latencyMs      = 0.0;
        int          burstStartSample = -1;
        int          firstSignalSample = -1;
        juce::String details;
    };

    // Run the full Tier 1 suite (buffer sweep + open/close + start/stop)
    // and return a markdown-style report. If opts.runLoopback is true,
    // also runs the loopback probe and includes results.
    static juce::String runAll (const Options& opts);

    // Individual entry points - useful for targeted reruns.
    static juce::Array<Result> runBufferSweep    (const Options& opts);
    static Result              runOpenCloseStress (const Options& opts);
    static Result              runStartStopRace   (const Options& opts);
    static LoopbackResult      runLoopbackProbe   (const Options& opts);
};
} // namespace focal
