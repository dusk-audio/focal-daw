#include "Session.h"

namespace focal
{
Session::Session()
{
    for (int i = 0; i < kNumTracks; ++i)
    {
        tracks[(size_t) i].name = juce::String (i + 1);
        tracks[(size_t) i].colour = juce::Colour::fromHSV (i / (float) kNumTracks, 0.45f, 0.75f, 1.0f);
    }

    static const char* defaultBusNames[] = { "BUS 1", "BUS 2", "BUS 3", "BUS 4" };
    for (int i = 0; i < kNumBuses; ++i)
    {
        buses[(size_t) i].name = defaultBusNames[i];
        buses[(size_t) i].colour = juce::Colour::fromHSV (0.55f + i * 0.07f, 0.35f, 0.72f, 1.0f);
    }

    static const char* defaultLaneNames[] = { "AUX 1", "AUX 2", "AUX 3", "AUX 4" };
    for (int i = 0; i < kNumAuxLanes; ++i)
    {
        auxLanes[(size_t) i].name = defaultLaneNames[i];
        // Different hue band so the AUX UI reads differently from the bus
        // strips and the track palette.
        auxLanes[(size_t) i].colour = juce::Colour::fromHSV (0.78f + i * 0.05f, 0.40f, 0.78f, 1.0f);
    }
}

bool Session::anyTrackSoloed() const noexcept
{
    return soloTrackCount.load (std::memory_order_relaxed) > 0;
}

bool Session::anyBusSoloed() const noexcept
{
    return soloBusCount.load (std::memory_order_relaxed) > 0;
}

bool Session::anyTrackArmed() const noexcept
{
    return armedTrackCount.load (std::memory_order_relaxed) > 0;
}

void Session::setTrackSoloed (int trackIndex, bool soloed) noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    auto& a = tracks[(size_t) trackIndex].strip.solo;
    const bool prev = a.exchange (soloed, std::memory_order_relaxed);
    if (prev != soloed)
        soloTrackCount.fetch_add (soloed ? 1 : -1, std::memory_order_relaxed);
}

void Session::setBusSoloed (int busIndex, bool soloed) noexcept
{
    if (busIndex < 0 || busIndex >= kNumBuses) return;
    auto& a = buses[(size_t) busIndex].strip.solo;
    const bool prev = a.exchange (soloed, std::memory_order_relaxed);
    if (prev != soloed)
        soloBusCount.fetch_add (soloed ? 1 : -1, std::memory_order_relaxed);
}

void Session::setTrackArmed (int trackIndex, bool armed) noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    auto& a = tracks[(size_t) trackIndex].recordArmed;
    const bool prev = a.exchange (armed, std::memory_order_relaxed);
    if (prev != armed)
        armedTrackCount.fetch_add (armed ? 1 : -1, std::memory_order_relaxed);
}

void Session::recomputeRtCounters() noexcept
{
    int s = 0, a = 0, ar = 0;
    for (auto& t : tracks)
    {
        if (t.strip.solo.load (std::memory_order_relaxed)) ++s;
        if (t.recordArmed.load (std::memory_order_relaxed)) ++ar;
    }
    for (auto& b : buses)
        if (b.strip.solo.load (std::memory_order_relaxed)) ++a;

    soloTrackCount .store (s,  std::memory_order_relaxed);
    soloBusCount   .store (a,  std::memory_order_relaxed);
    armedTrackCount.store (ar, std::memory_order_relaxed);
}

int Session::resolveInputForTrack (int trackIndex) const noexcept
{
    const int src = tracks[(size_t) trackIndex].inputSource.load (std::memory_order_relaxed);
    if (src == -2) return trackIndex;  // follow track index
    return src;                         // -1 = none, 0..N = explicit input
}

int Session::resolveInputRForTrack (int trackIndex) const noexcept
{
    const auto& t = tracks[(size_t) trackIndex];
    // R channel only valid in stereo mode.
    if (t.mode.load (std::memory_order_relaxed) != (int) Track::Mode::Stereo)
        return -1;
    const int rSrc = t.inputSourceR.load (std::memory_order_relaxed);
    if (rSrc == -2)
    {
        // "Follow" semantics for R - paired adjacent to the L source.
        const int lSrc = t.inputSource.load (std::memory_order_relaxed);
        const int lResolved = (lSrc == -2) ? trackIndex : lSrc;
        return (lResolved >= 0) ? lResolved + 1 : -1;
    }
    return rSrc;                        // -1 = none, 0..N = explicit input
}

void Session::setSessionDirectory (const juce::File& dir)
{
    sessionDir = dir;
    if (! sessionDir.exists())
        sessionDir.createDirectory();
    auto audioDir = getAudioDirectory();
    if (! audioDir.exists())
        audioDir.createDirectory();
}
} // namespace focal
