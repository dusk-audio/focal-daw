#include "Session.h"

namespace adhdaw
{
Session::Session()
{
    for (int i = 0; i < kNumTracks; ++i)
    {
        tracks[(size_t) i].name = juce::String (i + 1);
        tracks[(size_t) i].colour = juce::Colour::fromHSV (i / (float) kNumTracks, 0.45f, 0.75f, 1.0f);
    }

    static const char* defaultBusNames[] = { "BUS 1", "BUS 2", "BUS 3", "BUS 4" };
    for (int i = 0; i < kNumAuxBuses; ++i)
    {
        auxBuses[(size_t) i].name = defaultBusNames[i];
        auxBuses[(size_t) i].colour = juce::Colour::fromHSV (0.55f + i * 0.07f, 0.35f, 0.72f, 1.0f);
    }
}

bool Session::anyTrackSoloed() const noexcept
{
    for (auto& t : tracks)
        if (t.strip.solo.load (std::memory_order_relaxed))
            return true;
    return false;
}

bool Session::anyAuxSoloed() const noexcept
{
    for (auto& a : auxBuses)
        if (a.strip.solo.load (std::memory_order_relaxed))
            return true;
    return false;
}

bool Session::anyTrackArmed() const noexcept
{
    for (auto& t : tracks)
        if (t.recordArmed.load (std::memory_order_relaxed))
            return true;
    return false;
}

int Session::resolveInputForTrack (int trackIndex) const noexcept
{
    const int src = tracks[(size_t) trackIndex].inputSource.load (std::memory_order_relaxed);
    if (src == -2) return trackIndex;  // follow track index
    return src;                         // -1 = none, 0..N = explicit input
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
} // namespace adhdaw
