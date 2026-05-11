#include <catch2/catch_test_macros.hpp>

#include "session/SnapHelpers.h"

namespace
{
constexpr double kSr = 48000.0;
}

TEST_CASE ("beatSamples: returns 0 when bpm is zero", "[SnapHelpers]")
{
    REQUIRE (focal::snap::beatSamples (kSr, 0.0f) == 0);
    REQUIRE (focal::snap::beatSamples (kSr, -10.0f) == 0);
}

TEST_CASE ("beatSamples: returns 0 when sample rate is zero", "[SnapHelpers]")
{
    REQUIRE (focal::snap::beatSamples (0.0, 120.0f) == 0);
    REQUIRE (focal::snap::beatSamples (-1.0, 120.0f) == 0);
}

TEST_CASE ("beatSamples: 120 BPM at 48 kHz = 24000 samples per beat", "[SnapHelpers]")
{
    REQUIRE (focal::snap::beatSamples (48000.0, 120.0f) == 24000);
    REQUIRE (focal::snap::beatSamples (48000.0, 60.0f)  == 48000);
    REQUIRE (focal::snap::beatSamples (44100.0, 120.0f) == 22050);
}

TEST_CASE ("beatOrSecondSamples: falls back to 1 second when bpm <= 0", "[SnapHelpers]")
{
    REQUIRE (focal::snap::beatOrSecondSamples (48000.0, 0.0f)   == 48000);
    REQUIRE (focal::snap::beatOrSecondSamples (44100.0, -1.0f)  == 44100);
    REQUIRE (focal::snap::beatOrSecondSamples (48000.0, 120.0f) == 24000);
}

TEST_CASE ("snapDelta: no-op when step <= 0", "[SnapHelpers]")
{
    REQUIRE (focal::snap::snapDelta ((juce::int64) 12345, 0)  == 12345);
    REQUIRE (focal::snap::snapDelta ((juce::int64) -500,  -1) == -500);
}

TEST_CASE ("snapDelta: rounds positive deltas with half-step bias", "[SnapHelpers]")
{
    constexpr juce::int64 step = 1000;
    // Exactly on a step boundary returns itself.
    REQUIRE (focal::snap::snapDelta ((juce::int64) 1000, step) == 1000);
    REQUIRE (focal::snap::snapDelta ((juce::int64) 0,    step) == 0);
    // Below half-step rounds down.
    REQUIRE (focal::snap::snapDelta ((juce::int64) 499,  step) == 0);
    // At/above half-step rounds up.
    REQUIRE (focal::snap::snapDelta ((juce::int64) 500,  step) == 1000);
    REQUIRE (focal::snap::snapDelta ((juce::int64) 1499, step) == 1000);
    REQUIRE (focal::snap::snapDelta ((juce::int64) 1500, step) == 2000);
}

TEST_CASE ("snapDelta: sign-aware rounding for negative deltas", "[SnapHelpers]")
{
    constexpr juce::int64 step = 1000;
    REQUIRE (focal::snap::snapDelta ((juce::int64) -499,  step) == 0);
    REQUIRE (focal::snap::snapDelta ((juce::int64) -500,  step) == -1000);
    REQUIRE (focal::snap::snapDelta ((juce::int64) -1499, step) == -1000);
    REQUIRE (focal::snap::snapDelta ((juce::int64) -1500, step) == -2000);
}

TEST_CASE ("snapAbsolute: no-op when step <= 0 or input negative", "[SnapHelpers]")
{
    REQUIRE (focal::snap::snapAbsolute ((juce::int64) 12345, 0)    == 12345);
    REQUIRE (focal::snap::snapAbsolute ((juce::int64) -100,  1000) == -100);
}

TEST_CASE ("snapAbsolute: rounds to nearest grid point", "[SnapHelpers]")
{
    constexpr juce::int64 step = 1000;
    REQUIRE (focal::snap::snapAbsolute ((juce::int64) 0,    step) == 0);
    REQUIRE (focal::snap::snapAbsolute ((juce::int64) 499,  step) == 0);
    REQUIRE (focal::snap::snapAbsolute ((juce::int64) 500,  step) == 1000);
    REQUIRE (focal::snap::snapAbsolute ((juce::int64) 1499, step) == 1000);
    REQUIRE (focal::snap::snapAbsolute ((juce::int64) 1500, step) == 2000);
}

TEST_CASE ("snap math agrees with the legacy TapeStrip inline expression",
          "[SnapHelpers]")
{
    // Verify byte-identical output vs the pre-refactor inline code at the
    // four TapeStrip snap sites. Reference expression (delta-form):
    //   d = (delta + (delta >= 0 ? step/2 : -step/2)) / step * step
    // and (absolute-form):
    //   s = (sample + step/2) / step * step
    auto legacyDelta = [] (juce::int64 d, juce::int64 step) -> juce::int64
    {
        if (step <= 0) return d;
        return ((d + (d >= 0 ? step / 2 : -step / 2)) / step) * step;
    };
    auto legacyAbs = [] (juce::int64 s, juce::int64 step) -> juce::int64
    {
        if (step <= 0 || s < 0) return s;
        return ((s + step / 2) / step) * step;
    };

    const juce::int64 step120 = focal::snap::beatSamples (48000.0, 120.0f);
    REQUIRE (step120 == 24000);

    for (juce::int64 d : { (juce::int64) -100000, (juce::int64) -24001,
                            (juce::int64) -12000,  (juce::int64) -11999,
                            (juce::int64) 0,        (juce::int64) 11999,
                            (juce::int64) 12000,    (juce::int64) 23999,
                            (juce::int64) 24000,    (juce::int64) 100000 })
    {
        REQUIRE (focal::snap::snapDelta (d, step120) == legacyDelta (d, step120));
    }

    for (juce::int64 s : { (juce::int64) 0, (juce::int64) 11999, (juce::int64) 12000,
                            (juce::int64) 23999, (juce::int64) 24000,
                            (juce::int64) 100000 })
    {
        REQUIRE (focal::snap::snapAbsolute (s, step120) == legacyAbs (s, step120));
    }
}
