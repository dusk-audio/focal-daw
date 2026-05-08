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

// ── CloneTrackAction ─────────────────────────────────────────────────────

// POD snapshot of every per-track field the clone should duplicate.
// Atomics are loaded into plain values; vectors are copied by value.
// Plugin state is captured as the (descriptionXml, stateBase64) pair the
// session-restore path already understands.
struct CloneTrackAction::Impl
{
    juce::String name;
    juce::Colour colour;
    int mode = 0;

    // ChannelStripParams.
    float faderDb = 0.0f, pan = 0.0f;
    bool  mute = false, solo = false, phaseInvert = false;
    std::array<bool, ChannelStripParams::kNumBuses> busAssign {};
    std::array<float, ChannelStripParams::kNumAuxSends> auxSendDb {};
    std::array<bool,  ChannelStripParams::kNumAuxSends> auxSendPreFader {};

    bool  hpfEnabled = false; float hpfFreq = 20.0f;
    float lfGainDb = 0.0f, lfFreq = 100.0f;
    float lmGainDb = 0.0f, lmFreq = 600.0f, lmQ = 0.7f;
    float hmGainDb = 0.0f, hmFreq = 2000.0f, hmQ = 0.7f;
    float hfGainDb = 0.0f, hfFreq = 8000.0f;
    bool  eqBlackMode = false;

    bool  compEnabled = false;
    int   compMode = 2;
    float compThresholdDb = 0.0f;
    float compOptoPeakRed = 30.0f, compOptoGain = 50.0f;
    bool  compOptoLimit = false;
    float compFetInput = 0.0f, compFetOutput = 0.0f, compFetAttack = 0.2f, compFetRelease = 400.0f;
    int   compFetRatio = 0;
    float compVcaThreshDb = 0.0f, compVcaRatio = 4.0f, compVcaAttack = 1.0f, compVcaRelease = 100.0f, compVcaOutput = 0.0f;

    // Recording surface.
    bool  recordArmed = false;
    bool  inputMonitor = false;
    bool  printEffects = false;
    int   inputSource = -2, inputSourceR = -2;
    int   midiInputIndex = -1, midiChannel = 0;

    // Plugin slot - description + state captured live from the engine.
    juce::String pluginDescXml;
    juce::String pluginStateB64;

    // Region / MIDI region content.
    std::vector<AudioRegion> regions;
    std::vector<MidiRegion>  midiRegions;
};

namespace
{
CloneTrackAction::Impl captureTrack (Track& t, AudioEngine& engine, int idx)
{
    CloneTrackAction::Impl s;
    s.name        = t.name;
    s.colour      = t.colour;
    s.mode        = t.mode.load (std::memory_order_relaxed);

    s.faderDb     = t.strip.faderDb.load (std::memory_order_relaxed);
    s.pan         = t.strip.pan.load     (std::memory_order_relaxed);
    s.mute        = t.strip.mute.load    (std::memory_order_relaxed);
    s.solo        = t.strip.solo.load    (std::memory_order_relaxed);
    s.phaseInvert = t.strip.phaseInvert.load (std::memory_order_relaxed);
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        s.busAssign[(size_t) i] = t.strip.busAssign[(size_t) i].load (std::memory_order_relaxed);
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        s.auxSendDb      [(size_t) i] = t.strip.auxSendDb[(size_t) i].load (std::memory_order_relaxed);
        s.auxSendPreFader[(size_t) i] = t.strip.auxSendPreFader[(size_t) i].load (std::memory_order_relaxed);
    }

    s.hpfEnabled = t.strip.hpfEnabled.load (std::memory_order_relaxed);
    s.hpfFreq    = t.strip.hpfFreq.load    (std::memory_order_relaxed);
    s.lfGainDb   = t.strip.lfGainDb.load   (std::memory_order_relaxed);
    s.lfFreq     = t.strip.lfFreq.load     (std::memory_order_relaxed);
    s.lmGainDb   = t.strip.lmGainDb.load   (std::memory_order_relaxed);
    s.lmFreq     = t.strip.lmFreq.load     (std::memory_order_relaxed);
    s.lmQ        = t.strip.lmQ.load        (std::memory_order_relaxed);
    s.hmGainDb   = t.strip.hmGainDb.load   (std::memory_order_relaxed);
    s.hmFreq     = t.strip.hmFreq.load     (std::memory_order_relaxed);
    s.hmQ        = t.strip.hmQ.load        (std::memory_order_relaxed);
    s.hfGainDb   = t.strip.hfGainDb.load   (std::memory_order_relaxed);
    s.hfFreq     = t.strip.hfFreq.load     (std::memory_order_relaxed);
    s.eqBlackMode = t.strip.eqBlackMode.load (std::memory_order_relaxed);

    s.compEnabled    = t.strip.compEnabled.load    (std::memory_order_relaxed);
    s.compMode       = t.strip.compMode.load       (std::memory_order_relaxed);
    s.compThresholdDb = t.strip.compThresholdDb.load (std::memory_order_relaxed);
    s.compOptoPeakRed = t.strip.compOptoPeakRed.load (std::memory_order_relaxed);
    s.compOptoGain    = t.strip.compOptoGain.load    (std::memory_order_relaxed);
    s.compOptoLimit   = t.strip.compOptoLimit.load   (std::memory_order_relaxed);
    s.compFetInput    = t.strip.compFetInput.load    (std::memory_order_relaxed);
    s.compFetOutput   = t.strip.compFetOutput.load   (std::memory_order_relaxed);
    s.compFetAttack   = t.strip.compFetAttack.load   (std::memory_order_relaxed);
    s.compFetRelease  = t.strip.compFetRelease.load  (std::memory_order_relaxed);
    s.compFetRatio    = t.strip.compFetRatio.load    (std::memory_order_relaxed);
    s.compVcaThreshDb = t.strip.compVcaThreshDb.load (std::memory_order_relaxed);
    s.compVcaRatio    = t.strip.compVcaRatio.load    (std::memory_order_relaxed);
    s.compVcaAttack   = t.strip.compVcaAttack.load   (std::memory_order_relaxed);
    s.compVcaRelease  = t.strip.compVcaRelease.load  (std::memory_order_relaxed);
    s.compVcaOutput   = t.strip.compVcaOutput.load   (std::memory_order_relaxed);

    s.recordArmed    = t.recordArmed.load    (std::memory_order_relaxed);
    s.inputMonitor   = t.inputMonitor.load   (std::memory_order_relaxed);
    s.printEffects   = t.printEffects.load   (std::memory_order_relaxed);
    s.inputSource    = t.inputSource.load    (std::memory_order_relaxed);
    s.inputSourceR   = t.inputSourceR.load   (std::memory_order_relaxed);
    s.midiInputIndex = t.midiInputIndex.load (std::memory_order_relaxed);
    s.midiChannel    = t.midiChannel.load    (std::memory_order_relaxed);

    // Plugin: pull from the live slot, not the (potentially stale)
    // session.json fields. Those are only kept fresh during save.
    auto& slot = engine.getStrip (idx).getPluginSlot();
    s.pluginDescXml  = slot.getDescriptionXmlForSave();
    s.pluginStateB64 = slot.getStateBase64ForSave();

    s.regions     = t.regions;
    s.midiRegions = t.midiRegions;
    return s;
}

void applyTrack (Track& t, AudioEngine& engine, int idx,
                  const CloneTrackAction::Impl& s)
{
    t.name   = s.name;
    t.colour = s.colour;
    t.mode.store (s.mode, std::memory_order_relaxed);

    t.strip.faderDb.store     (s.faderDb,     std::memory_order_relaxed);
    t.strip.pan.store         (s.pan,         std::memory_order_relaxed);
    t.strip.mute.store        (s.mute,        std::memory_order_relaxed);
    t.strip.solo.store        (s.solo,        std::memory_order_relaxed);
    t.strip.phaseInvert.store (s.phaseInvert, std::memory_order_relaxed);
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        t.strip.busAssign[(size_t) i].store (s.busAssign[(size_t) i], std::memory_order_relaxed);
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        t.strip.auxSendDb      [(size_t) i].store (s.auxSendDb[(size_t) i],      std::memory_order_relaxed);
        t.strip.auxSendPreFader[(size_t) i].store (s.auxSendPreFader[(size_t) i], std::memory_order_relaxed);
    }

    t.strip.hpfEnabled.store (s.hpfEnabled, std::memory_order_relaxed);
    t.strip.hpfFreq.store    (s.hpfFreq,    std::memory_order_relaxed);
    t.strip.lfGainDb.store   (s.lfGainDb,   std::memory_order_relaxed);
    t.strip.lfFreq.store     (s.lfFreq,     std::memory_order_relaxed);
    t.strip.lmGainDb.store   (s.lmGainDb,   std::memory_order_relaxed);
    t.strip.lmFreq.store     (s.lmFreq,     std::memory_order_relaxed);
    t.strip.lmQ.store        (s.lmQ,        std::memory_order_relaxed);
    t.strip.hmGainDb.store   (s.hmGainDb,   std::memory_order_relaxed);
    t.strip.hmFreq.store     (s.hmFreq,     std::memory_order_relaxed);
    t.strip.hmQ.store        (s.hmQ,        std::memory_order_relaxed);
    t.strip.hfGainDb.store   (s.hfGainDb,   std::memory_order_relaxed);
    t.strip.hfFreq.store     (s.hfFreq,     std::memory_order_relaxed);
    t.strip.eqBlackMode.store (s.eqBlackMode, std::memory_order_relaxed);

    t.strip.compEnabled.store    (s.compEnabled,    std::memory_order_relaxed);
    t.strip.compMode.store       (s.compMode,       std::memory_order_relaxed);
    t.strip.compThresholdDb.store (s.compThresholdDb, std::memory_order_relaxed);
    t.strip.compOptoPeakRed.store (s.compOptoPeakRed, std::memory_order_relaxed);
    t.strip.compOptoGain.store    (s.compOptoGain,    std::memory_order_relaxed);
    t.strip.compOptoLimit.store   (s.compOptoLimit,   std::memory_order_relaxed);
    t.strip.compFetInput.store    (s.compFetInput,    std::memory_order_relaxed);
    t.strip.compFetOutput.store   (s.compFetOutput,   std::memory_order_relaxed);
    t.strip.compFetAttack.store   (s.compFetAttack,   std::memory_order_relaxed);
    t.strip.compFetRelease.store  (s.compFetRelease,  std::memory_order_relaxed);
    t.strip.compFetRatio.store    (s.compFetRatio,    std::memory_order_relaxed);
    t.strip.compVcaThreshDb.store (s.compVcaThreshDb, std::memory_order_relaxed);
    t.strip.compVcaRatio.store    (s.compVcaRatio,    std::memory_order_relaxed);
    t.strip.compVcaAttack.store   (s.compVcaAttack,   std::memory_order_relaxed);
    t.strip.compVcaRelease.store  (s.compVcaRelease,  std::memory_order_relaxed);
    t.strip.compVcaOutput.store   (s.compVcaOutput,   std::memory_order_relaxed);

    t.recordArmed.store    (s.recordArmed,    std::memory_order_relaxed);
    t.inputMonitor.store   (s.inputMonitor,   std::memory_order_relaxed);
    t.printEffects.store   (s.printEffects,   std::memory_order_relaxed);
    t.inputSource.store    (s.inputSource,    std::memory_order_relaxed);
    t.inputSourceR.store   (s.inputSourceR,   std::memory_order_relaxed);
    t.midiInputIndex.store (s.midiInputIndex, std::memory_order_relaxed);
    t.midiChannel.store    (s.midiChannel,    std::memory_order_relaxed);

    // Plugin: replay through the live slot. Empty descriptionXml
    // legitimately means "no plugin" - restoreFromSavedState handles
    // that as the unloaded steady state.
    juce::String err;
    engine.getStrip (idx).getPluginSlot().restoreFromSavedState (
        s.pluginDescXml, s.pluginStateB64, err);
    if (err.isNotEmpty())
        DBG ("CloneTrackAction: plugin restore failed on strip " << idx
              << " (" << s.pluginDescXml << "): " << err);

    t.regions     = s.regions;
    t.midiRegions = s.midiRegions;

    // Persist the post-restore plugin state on Session so a save right
    // after a clone (with no manual edits in between) round-trips
    // correctly. publishPluginStateForSave does this for ALL slots; for
    // a single track we mirror by hand.
    t.pluginDescriptionXml = s.pluginDescXml;
    t.pluginStateBase64    = s.pluginStateB64;
}
} // namespace

CloneTrackAction::CloneTrackAction (Session& s, AudioEngine& e,
                                      int srcTrack, int dstTrack)
    : session (s), engine (e), srcIdx (srcTrack), dstIdx (dstTrack)
{}

CloneTrackAction::~CloneTrackAction() = default;

bool CloneTrackAction::perform()
{
    if (srcIdx < 0 || srcIdx >= Session::kNumTracks) return false;
    if (dstIdx < 0 || dstIdx >= Session::kNumTracks) return false;
    if (srcIdx == dstIdx) return false;

    // First perform: capture both before-state of the destination
    // (for undo) and after-state from the source (for redo). Subsequent
    // perform()s (i.e. redo) just re-apply the captured after-state.
    if (beforeState == nullptr)
    {
        beforeState = std::make_unique<Impl> (
            captureTrack (session.track (dstIdx), engine, dstIdx));
        afterState = std::make_unique<Impl> (
            captureTrack (session.track (srcIdx), engine, srcIdx));
        // Tag the cloned name so the user can tell duplicates apart.
        afterState->name = afterState->name + " (copy)";
    }

    applyTrack (session.track (dstIdx), engine, dstIdx, *afterState);
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool CloneTrackAction::undo()
{
    if (beforeState == nullptr) return false;
    if (dstIdx < 0 || dstIdx >= Session::kNumTracks) return false;
    applyTrack (session.track (dstIdx), engine, dstIdx, *beforeState);
    rebuildPlaybackIfStopped (engine);
    return true;
}
} // namespace focal
