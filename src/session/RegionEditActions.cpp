#include "RegionEditActions.h"
#include "../engine/AudioEngine.h"
#include "../engine/PlaybackEngine.h"
#include "../engine/Transport.h"

namespace focal
{
namespace
{
void rebuildPlaybackIfStopped (AudioEngine& engine)
{
    if (engine.getTransport().getState() == Transport::State::Stopped)
        engine.getPlaybackEngine().preparePlayback();
}

bool indexValid (Session& s, int trackIdx, int regionIdx)
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    auto& regs = s.track (trackIdx).regions;
    return regionIdx >= 0 && regionIdx < (int) regs.size();
}
} // namespace

// ── RegionEditAction ──────────────────────────────────────────────────────

RegionEditAction::RegionEditAction (Session& s, AudioEngine& e,
                                      int t, int idx,
                                      const AudioRegion& b, const AudioRegion& a)
    : session (s), engine (e), trackIdx (t), regionIdx (idx),
      beforeState (b), afterState (a)
{}

bool RegionEditAction::perform()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    session.track (trackIdx).regions[(size_t) regionIdx] = afterState;
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool RegionEditAction::undo()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    session.track (trackIdx).regions[(size_t) regionIdx] = beforeState;
    rebuildPlaybackIfStopped (engine);
    return true;
}

// ── SplitRegionAction ─────────────────────────────────────────────────────

SplitRegionAction::SplitRegionAction (Session& s, AudioEngine& e,
                                        int t, int idx, juce::int64 atSample)
    : session (s), engine (e), trackIdx (t), regionIdx (idx), splitAt (atSample)
{}

bool SplitRegionAction::perform()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    auto& regs = session.track (trackIdx).regions;
    auto& orig = regs[(size_t) regionIdx];

    if (splitAt <= orig.timelineStart
        || splitAt >= orig.timelineStart + orig.lengthInSamples)
        return false;

    originalState = orig;

    AudioRegion right = orig;
    const auto leftLen = splitAt - orig.timelineStart;
    right.timelineStart   = splitAt;
    right.sourceOffset    = orig.sourceOffset + leftLen;
    right.lengthInSamples = orig.lengthInSamples - leftLen;

    orig.lengthInSamples  = leftLen;
    regs.insert (regs.begin() + regionIdx + 1, right);

    rebuildPlaybackIfStopped (engine);
    return true;
}

bool SplitRegionAction::undo()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    auto& regs = session.track (trackIdx).regions;

    // Remove the right half (idx+1) and restore the left to its full extent.
    if (regionIdx + 1 >= (int) regs.size()) return false;
    regs.erase (regs.begin() + regionIdx + 1);

    if (regionIdx >= (int) regs.size()) return false;
    regs[(size_t) regionIdx] = originalState;

    rebuildPlaybackIfStopped (engine);
    return true;
}

// ── PasteRegionAction ─────────────────────────────────────────────────────

PasteRegionAction::PasteRegionAction (Session& s, AudioEngine& e,
                                        int t, const AudioRegion& r)
    : session (s), engine (e), trackIdx (t), regionToInsert (r)
{}

bool PasteRegionAction::perform()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    auto& regs = session.track (trackIdx).regions;
    insertedAt = (int) regs.size();
    regs.push_back (regionToInsert);
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool PasteRegionAction::undo()
{
    if (insertedAt < 0) return false;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    auto& regs = session.track (trackIdx).regions;
    if (insertedAt >= (int) regs.size()) return false;
    regs.erase (regs.begin() + insertedAt);
    rebuildPlaybackIfStopped (engine);
    return true;
}

// ── DeleteRegionAction ────────────────────────────────────────────────────

DeleteRegionAction::DeleteRegionAction (Session& s, AudioEngine& e,
                                          int t, int idx)
    : session (s), engine (e), trackIdx (t), regionIdx (idx)
{}

bool DeleteRegionAction::perform()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    auto& regs = session.track (trackIdx).regions;
    removed = regs[(size_t) regionIdx];
    haveRemoved = true;
    regs.erase (regs.begin() + regionIdx);
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool DeleteRegionAction::undo()
{
    if (! haveRemoved) return false;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    auto& regs = session.track (trackIdx).regions;

    const int insertAt = juce::jmin (regionIdx, (int) regs.size());
    regs.insert (regs.begin() + insertAt, removed);
    rebuildPlaybackIfStopped (engine);
    return true;
}
} // namespace focal
