#include "AlsaPerformanceTest.h"
#include "AlsaAudioIODevice.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace focal
{
namespace
{
// Default buffer ladder if Options::bufferSizes is empty. Standard pro-audio
// powers-of-two from 32 to 2048 - covers the latency range users actually
// pick from. 4096+ is rare for live work and stable everywhere; not worth
// the test time.
const int kDefaultBufferLadder[] = { 32, 64, 128, 256, 512, 1024, 2048 };

// Count this many callbacks before we start storing timings - lets the
// audio thread's first-period spikes (page-fault cost, RT priority promotion
// scheduler dance) settle before we take measurements. ~50ms warmup at
// 48k/512 is two periods; we extend to 8 to be safe at very small buffers.
constexpr int kWarmupCallbacks = 8;

// Per-callback timings: pre-allocated ring buffer, lock-free writes from
// the audio thread, read after stop. Sized for worst-case 5s at 32 frames
// per callback at 192 kHz = 30000 callbacks; 64K entries gives plenty of
// headroom and is still trivially small at 8 bytes each (~512 KB).
constexpr int kSampleBufferSize = 65536;

// Verdict thresholds. UNSAFE = any xruns OR p99 within 90% of budget.
// MARGINAL = p99 within 70% of budget. Otherwise SAFE.
constexpr double kP99WarningRatio = 0.70;
constexpr double kP99FailureRatio = 0.90;

class MeasuringCallback final : public juce::AudioIODeviceCallback
{
public:
    explicit MeasuringCallback (int fakeLoadMicroseconds)
        : fakeLoadUs (fakeLoadMicroseconds)
    {
        samples.resize (kSampleBufferSize, 0.0);
        const auto tps = juce::Time::getHighResolutionTicksPerSecond();
        secondsPerTick = (tps > 0) ? 1.0 / (double) tps : 0.0;
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* dev) override
    {
        const auto sr = dev->getCurrentSampleRate();
        const auto bs = dev->getCurrentBufferSizeSamples();
        budgetMs = (sr > 0.0) ? 1000.0 * (double) bs / sr : 0.0;
        callbackCount.store (0, std::memory_order_relaxed);
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const* /*inputChannelData*/,
                                            int /*numInputChannels*/,
                                            float* const* outputChannelData,
                                            int numOutputChannels,
                                            int numSamples,
                                            const juce::AudioIODeviceCallbackContext&) override
    {
        const auto t0 = juce::Time::getHighResolutionTicks();

        // Synthetic DSP load: busy-wait so the audio thread actually consumes
        // wall-clock time. Sleep would yield to the scheduler and produce
        // misleading "free" headroom.
        if (fakeLoadUs > 0)
        {
            const auto budgetTicks = (juce::int64) (((double) fakeLoadUs * 1.0e-6)
                                                       / juce::jmax (1.0e-12, secondsPerTick));
            const auto deadline = t0 + budgetTicks;
            while (juce::Time::getHighResolutionTicks() < deadline) { /* spin */ }
        }

        // Output silence. Tier 1 measures stability, not signal correctness;
        // real-signal validation is Tier 3 and needs loopback hardware.
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                std::memset (outputChannelData[ch], 0,
                              sizeof (float) * (size_t) numSamples);

        const auto t1 = juce::Time::getHighResolutionTicks();
        const double elapsedMs = (double) (t1 - t0) * secondsPerTick * 1000.0;

        const int idx = callbackCount.fetch_add (1, std::memory_order_relaxed);
        if (idx >= kWarmupCallbacks && (idx - kWarmupCallbacks) < kSampleBufferSize)
            samples[(size_t) (idx - kWarmupCallbacks)] = elapsedMs;
    }

    // After stop(): collect stats from the measured-callback samples.
    struct Stats { double mean = 0, p95 = 0, p99 = 0, max = 0; int n = 0; };

    Stats computeStats() const
    {
        const int total = callbackCount.load (std::memory_order_acquire);
        const int measured = juce::jmax (0, total - kWarmupCallbacks);
        const int n = juce::jmin (measured, kSampleBufferSize);
        Stats s;
        s.n = n;
        if (n == 0)
            return s;

        std::vector<double> sorted (samples.begin(), samples.begin() + n);
        std::sort (sorted.begin(), sorted.end());

        double sum = 0.0;
        for (double v : sorted) sum += v;
        s.mean = sum / (double) n;
        s.max  = sorted.back();
        // Linear-interpolation percentiles. For our sample sizes (hundreds
        // to tens of thousands), exact-rank is fine.
        auto pct = [&] (double p)
        {
            const double idxF = p * (double) (n - 1);
            const int lo = (int) std::floor (idxF);
            const int hi = (int) std::ceil  (idxF);
            const double frac = idxF - (double) lo;
            return sorted[(size_t) lo] * (1.0 - frac) + sorted[(size_t) hi] * frac;
        };
        s.p95 = pct (0.95);
        s.p99 = pct (0.99);
        return s;
    }

    int totalCallbacks() const { return callbackCount.load (std::memory_order_acquire); }
    double getBudgetMs() const { return budgetMs; }

private:
    std::vector<double> samples;
    std::atomic<int>    callbackCount { 0 };
    int                 fakeLoadUs    = 0;
    double              budgetMs      = 0.0;
    double              secondsPerTick = 0.0;
};

// Open + start an AlsaAudioIODevice for an output-only run. Returns nullptr
// (and sets errorOut) on failure. Caller owns the device, must stop+close.
std::unique_ptr<AlsaAudioIODevice> openOutputOnly (const juce::String& deviceId,
                                                     unsigned int sampleRate,
                                                     int          bufferSize,
                                                     juce::AudioIODeviceCallback* callback,
                                                     juce::String& errorOut)
{
    // Display name is just for the device's "name" field; the open itself
    // resolves through outputId. Channel masks: 2 stereo outs, no inputs.
    auto dev = std::make_unique<AlsaAudioIODevice> (deviceId, /*inId*/ "", /*outId*/ deviceId);
    juce::BigInteger outMask, inMask;
    outMask.setRange (0, 2, true);  // bits 0 + 1
    // inMask stays empty

    const auto err = dev->open (inMask, outMask, (double) sampleRate, bufferSize);
    if (err.isNotEmpty())
    {
        errorOut = err;
        return nullptr;
    }
    dev->start (callback);
    return dev;
}

juce::String formatVerdict (int xruns, double p99, double budget)
{
    if (xruns > 0)                                  return "UNSAFE (xruns)";
    if (budget <= 0.0 || p99 <= 0.0)                return "?";
    if (p99 >= budget * kP99FailureRatio)           return "UNSAFE (p99 near budget)";
    if (p99 >= budget * kP99WarningRatio)           return "MARGINAL";
    return "SAFE";
}

juce::String formatTableRow (const AlsaPerformanceTest::Result& r)
{
    auto fmt = [] (double v)
    {
        if (v <= 0.0) return juce::String ("    -  ");
        return juce::String::formatted ("%6.3f ms", v);
    };
    auto bufFromCfg = [] (const juce::String& cfg)
    {
        return cfg.fromFirstOccurrenceOf ("buf=", false, false)
                  .upToFirstOccurrenceOf (" ", false, false);
    };

    return juce::String::formatted (
        " %5s |  %s | %s | %s | %s | %s |    %4d   | %s",
        bufFromCfg (r.configuration).toRawUTF8(),
        fmt (r.budgetMs).toRawUTF8(),
        fmt (r.meanCallbackMs).toRawUTF8(),
        fmt (r.p95CallbackMs).toRawUTF8(),
        fmt (r.p99CallbackMs).toRawUTF8(),
        fmt (r.maxCallbackMs).toRawUTF8(),
        r.xruns,
        r.verdict.toRawUTF8());
}
} // namespace

juce::Array<AlsaPerformanceTest::Result>
AlsaPerformanceTest::runBufferSweep (const Options& opts)
{
    juce::Array<int> sizes = opts.bufferSizes;
    if (sizes.isEmpty())
        for (int b : kDefaultBufferLadder) sizes.add (b);

    juce::Array<Result> results;
    for (int bs : sizes)
    {
        Result r;
        r.testName      = "buffer sweep";
        r.configuration = juce::String::formatted ("buf=%d rate=%u", bs, opts.sampleRate);

        MeasuringCallback cb (opts.fakeDspLoadUs);
        juce::String openErr;
        auto dev = openOutputOnly (opts.deviceId, opts.sampleRate, bs, &cb, openErr);
        if (dev == nullptr)
        {
            r.passed   = false;
            r.verdict  = "FAIL (open)";
            r.details  = openErr;
            results.add (r);
            continue;
        }

        juce::Thread::sleep (opts.durationMs);

        dev->stop();
        const int xruns = dev->getXRunCount();
        const auto stats = cb.computeStats();
        dev->close();

        r.xruns           = xruns;
        r.budgetMs        = cb.getBudgetMs();
        r.meanCallbackMs  = stats.mean;
        r.p95CallbackMs   = stats.p95;
        r.p99CallbackMs   = stats.p99;
        r.maxCallbackMs   = stats.max;
        r.callbackCount   = stats.n;
        r.verdict         = formatVerdict (xruns, stats.p99, r.budgetMs);
        r.passed          = r.verdict == "SAFE" || r.verdict == "MARGINAL";
        results.add (r);
    }

    return results;
}

AlsaPerformanceTest::Result
AlsaPerformanceTest::runOpenCloseStress (const Options& opts)
{
    Result r;
    r.testName = "open/close cycle stress";
    r.configuration = juce::String::formatted ("cycles=%d", opts.openCloseCycles);

    int failures = 0;
    juce::String firstFailure;

    for (int i = 0; i < opts.openCloseCycles; ++i)
    {
        AlsaAudioIODevice dev (opts.deviceId, /*inId*/ "", /*outId*/ opts.deviceId);
        juce::BigInteger outMask, inMask;
        outMask.setRange (0, 2, true);

        const auto err = dev.open (inMask, outMask, (double) opts.sampleRate, 1024);
        if (err.isNotEmpty())
        {
            ++failures;
            if (firstFailure.isEmpty())
                firstFailure = juce::String::formatted ("cycle %d: ", i) + err;
            continue;
        }
        // No start/stop - just immediate close. Tests the unstart-then-close
        // path which is hit by "user changed their mind" UI flows.
        dev.close();
    }

    r.passed  = failures == 0;
    r.verdict = failures == 0 ? "SAFE" : juce::String::formatted ("UNSAFE (%d/%d failed)",
                                                                     failures, opts.openCloseCycles);
    r.details = firstFailure;
    return r;
}

AlsaPerformanceTest::Result
AlsaPerformanceTest::runStartStopRace (const Options& opts)
{
    Result r;
    r.testName = "start/stop race";
    r.configuration = juce::String::formatted ("cycles=%d", opts.startStopCycles);

    AlsaAudioIODevice dev (opts.deviceId, /*inId*/ "", /*outId*/ opts.deviceId);
    juce::BigInteger outMask, inMask;
    outMask.setRange (0, 2, true);

    const auto err = dev.open (inMask, outMask, (double) opts.sampleRate, 1024);
    if (err.isNotEmpty())
    {
        r.passed  = false;
        r.verdict = "FAIL (open)";
        r.details = err;
        return r;
    }

    MeasuringCallback cb (0);
    int failures = 0;

    for (int i = 0; i < opts.startStopCycles; ++i)
    {
        dev.start (&cb);
        // Brief moment so start() actually fires the device before stop kicks
        // in - 50ms = a few periods at 1024/48k. Without this, the test
        // measures only the queueing layer and misses real start_threshold
        // behavior.
        juce::Thread::sleep (50);
        dev.stop();
        if (! dev.isOpen())
        {
            ++failures;
            break;
        }
    }

    dev.close();

    r.xruns   = dev.getXRunCount();
    r.passed  = failures == 0;
    r.verdict = failures == 0 ? "SAFE"
                                : juce::String::formatted ("UNSAFE (failed at cycle %d)", failures);
    r.details = juce::String::formatted ("xruns over all cycles: %d", r.xruns);
    return r;
}

juce::String AlsaPerformanceTest::runAll (const Options& opts)
{
    juce::StringArray out;
    out.add ("=== ALSA Backend Performance Test (Tier 1) ===");
    out.add (juce::String::formatted ("Time:     %s",
                                         juce::Time::getCurrentTime().toString (true, true).toRawUTF8()));
    out.add (juce::String::formatted ("Device:   %s", opts.deviceId.toRawUTF8()));
    out.add (juce::String::formatted ("Rate:     %u Hz", opts.sampleRate));
    out.add (juce::String::formatted ("Duration: %d ms per buffer", opts.durationMs));
    out.add (juce::String::formatted ("DSP load: %d us / callback (synthetic)", opts.fakeDspLoadUs));
    out.add ("");

    out.add ("--- Buffer-size sweep (output-only, silent) ---");
    out.add (" Buffer | Budget    | Mean      | P95       | P99       | Max       | XRuns/run | Verdict");
    out.add ("--------|-----------|-----------|-----------|-----------|-----------|-----------|---------");

    const auto sweep = runBufferSweep (opts);
    int recommendedMin = -1;
    for (const auto& r : sweep)
    {
        out.add (formatTableRow (r));
        if (recommendedMin < 0 && (r.verdict == "SAFE" || r.verdict == "MARGINAL"))
        {
            const auto bufStr = r.configuration.fromFirstOccurrenceOf ("buf=", false, false)
                                                .upToFirstOccurrenceOf (" ", false, false);
            recommendedMin = bufStr.getIntValue();
        }
        if (r.details.isNotEmpty())
            out.add (juce::String ("        ") + r.details);
    }
    out.add ("");

    if (recommendedMin > 0)
    {
        out.add (juce::String::formatted ("Recommended minimum buffer for this hardware: %d frames", recommendedMin));
        out.add (juce::String::formatted ("Safer choice with 50%% headroom:               %d frames", recommendedMin * 2));
    }
    else
    {
        out.add ("No buffer size in the swept range was stable. Check the open errors above.");
    }
    out.add ("");

    out.add ("--- Open/close cycle stress ---");
    {
        const auto r = runOpenCloseStress (opts);
        out.add (juce::String::formatted ("  %d cycles  %s", opts.openCloseCycles, r.verdict.toRawUTF8()));
        if (r.details.isNotEmpty()) out.add (juce::String ("        ") + r.details);
    }
    out.add ("");

    out.add ("--- Start/stop race ---");
    {
        const auto r = runStartStopRace (opts);
        out.add (juce::String::formatted ("  %d cycles  %s", opts.startStopCycles, r.verdict.toRawUTF8()));
        if (r.details.isNotEmpty()) out.add (juce::String ("        ") + r.details);
    }
    out.add ("");

    out.add ("=== End of Performance Test ===");
    return out.joinIntoString ("\n");
}

} // namespace focal
