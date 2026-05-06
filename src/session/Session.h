#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace focal
{
// Phase 1a-minimal channel strip parameters: fader, pan, mute, solo. The full
// strip (HPF, 4-band EQ, FET/Opto compressor, sends, bus assigns) lands in
// later chunks once the DSP cores are lifted out of the Dusk plugins repo.
struct ChannelStripParams
{
    std::atomic<float> faderDb { 0.0f };   // -inf via -100 sentinel, +12 dB headroom
    std::atomic<float> pan     { 0.0f };   // -1 = full L, 0 = center, +1 = full R
    std::atomic<bool>  mute    { false };
    std::atomic<bool>  solo    { false };
    std::atomic<bool>  phaseInvert { false };  // Ø - flips polarity at strip input

    // Per-channel bus assigns. Bit i is true => post-fader signal also sums
    // into BUS (i+1) at unity. Channel always sums to master regardless of
    // bus assigns. Knob-style FX sends (with level) will be a separate
    // surface added later.
    static constexpr int kNumBuses = 4;
    std::array<std::atomic<bool>, kNumBuses> busAssign {};

    // Per-channel AUX sends. Distinct from bus assigns - AUXes feed the
    // 4 aux strips' plugin chains (reverb / delay / etc.), each with a
    // continuous send level and a pre/post-fader tap point. -100 dB sentinel
    // = no send (audio thread can short-circuit). Defaults: -inf, post-fader
    // (matches the typical "send to reverb" workflow).
    static constexpr int   kNumAuxSends = 4;
    static constexpr float kAuxSendMinDb = -60.0f;
    static constexpr float kAuxSendMaxDb =   6.0f;
    static constexpr float kAuxSendOffDb = -100.0f;   // sentinel: knob fully CCW
    std::array<std::atomic<float>, kNumAuxSends> auxSendDb {
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb },
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb }
    };
    std::array<std::atomic<bool>, kNumAuxSends> auxSendPreFader {};

    // 4K EQ surface: HPF + 4-band parametric (LF/LM/HM/HF) + Brown/Black mode.
    // Maps into BritishEQProcessor::Parameters (multi-q) when read on the audio thread.
    std::atomic<bool>  hpfEnabled { false };
    std::atomic<float> hpfFreq    { 20.0f };
    std::atomic<float> lfGainDb   { 0.0f };
    std::atomic<float> lfFreq     { 100.0f };
    std::atomic<float> lmGainDb   { 0.0f };
    std::atomic<float> lmFreq     { 600.0f };
    std::atomic<float> lmQ        { 0.7f };
    std::atomic<float> hmGainDb   { 0.0f };
    std::atomic<float> hmFreq     { 2000.0f };
    std::atomic<float> hmQ        { 0.7f };
    std::atomic<float> hfGainDb   { 0.0f };
    std::atomic<float> hfFreq     { 8000.0f };
    std::atomic<bool>  eqBlackMode { false };  // false = Brown (E-series), true = Black (G-series)

    // Compressor surface: UniversalCompressor exposes a different parameter
    // set per mode (Opto/FET/VCA model real hardware), so each mode keeps its
    // own atomic state and the UI swaps which controls are visible.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<int>   compMode       { 2 };       // 0=Opto, 1=FET, 2=VCA

    // Opto (LA-2A style): peak-reduction + gain + optional limit mode.
    std::atomic<float> compOptoPeakRed { 30.0f };  // 0..100 %
    std::atomic<float> compOptoGain    { 50.0f };  // 0..100 % (50 = unity)
    std::atomic<bool>  compOptoLimit   { false };

    // FET (1176 style): input drives compression, ratio is a discrete choice.
    std::atomic<float> compFetInput   { 0.0f };    // -20..40 dB
    std::atomic<float> compFetOutput  { 0.0f };    // -20..20 dB
    std::atomic<float> compFetAttack  { 0.2f };    // 0.02..80 ms
    std::atomic<float> compFetRelease { 400.0f };  // 50..1100 ms
    std::atomic<int>   compFetRatio   { 0 };       // 0=4:1, 1=8:1, 2=12:1, 3=20:1, 4=All

    // VCA (classic): the textbook threshold/ratio/attack/release/output.
    std::atomic<float> compVcaThreshDb { 0.0f };    // -38..12 dB
    std::atomic<float> compVcaRatio    { 4.0f };    // 1..120
    std::atomic<float> compVcaAttack   { 1.0f };    // 0.1..50 ms
    std::atomic<float> compVcaRelease  { 100.0f };  // 10..5000 ms
    std::atomic<float> compVcaOutput   { 0.0f };    // -20..20 dB

    // Legacy unified threshold - still driven by the comp meter strip's drag
    // handle. Mirrors the *current mode's* primary compression knob so the
    // drag stays usable regardless of which mode is active. The audio thread
    // does NOT read this directly any more - see ChannelStrip::updateCompParameters.
    std::atomic<float> compThresholdDb { 0.0f };

    // Unified makeup-intent across modes. The comp editor's MAKEUP knob writes
    // here AS WELL AS to the appropriate per-mode atom; FET threshold reads
    // this back when recomputing compFetOutput so threshold and makeup compose
    // (without it, threshold's chain compensation overwrites the user's makeup).
    // VCA / Opto don't need this dance - their makeup parameter is independent
    // of threshold - but we still write here for consistency / save-state.
    std::atomic<float> compMakeupDb    { 0.0f };

    static constexpr float kFaderMinDb       = -100.0f;
    static constexpr float kFaderMaxDb       =  12.0f;
    static constexpr float kFaderInfThreshDb = -90.0f;  // below this we hard-mute

    // EQ control ranges
    static constexpr float kHpfMinHz   = 20.0f;
    static constexpr float kHpfMaxHz   = 300.0f;
    static constexpr float kHpfOffHz   = 20.0f;   // at min, HPF is effectively bypassed
    static constexpr float kLfFreqMin  = 20.0f,   kLfFreqMax = 400.0f;
    static constexpr float kLmFreqMin  = 100.0f,  kLmFreqMax = 4000.0f;
    static constexpr float kHmFreqMin  = 600.0f,  kHmFreqMax = 13000.0f;
    static constexpr float kHfFreqMin  = 1000.0f, kHfFreqMax = 20000.0f;
    static constexpr float kBandGainMin = -15.0f, kBandGainMax = 15.0f;
    static constexpr float kBandQMin = 0.4f, kBandQMax = 4.0f;  // mid-band Q range

    // Compressor parameter ranges
    static constexpr float kCompThreshMin =  -60.0f, kCompThreshMax =   0.0f;
    static constexpr float kCompRatioMin  =    1.0f, kCompRatioMax  =  20.0f;
    static constexpr float kCompAttackMin =    0.1f, kCompAttackMax = 200.0f;
    static constexpr float kCompReleaseMin=   10.0f, kCompReleaseMax= 2000.0f;
    static constexpr float kCompMakeupMin =  -12.0f, kCompMakeupMax =  24.0f;
};

// One slot in a region's take history. Stores everything needed to swap a
// previously-recorded version of this region's audio back into the live
// region: the WAV path on disk, where in that WAV the playable slice starts,
// and how many samples the slice runs for. Timeline position is NOT stored -
// rotating takes preserves the region's timelineStart so the user always
// hears the alternate take in the same spot in the song.
struct TakeRef
{
    juce::File file;
    juce::int64 sourceOffset    = 0;
    juce::int64 lengthInSamples = 0;
};

// Audio region - references a mono WAV file on disk. Multiple recordings
// over the same timeline range stack into `previousTakes`; the live take's
// fields (file/sourceOffset/lengthInSamples) plus that vector form a
// rotating history the user can cycle through via the badge UI.
struct AudioRegion
{
    juce::File file;                  // absolute path to the WAV
    juce::int64 timelineStart = 0;    // sample position on the timeline
    juce::int64 lengthInSamples = 0;
    juce::int64 sourceOffset = 0;     // future: trim from left

    // Older takes that occupied this region's timeline range, captured by
    // RecordManager::stopRecording when the new take's range fully contains
    // an existing region. Front of the vector = next to surface on a cycle.
    std::vector<TakeRef> previousTakes;
};

struct Track
{
    // Per-track signal mode. Selects how the strip records and plays back:
    //   Mono   - one audio input -> mono WAV -> mono playback (legacy default)
    //   Stereo - two audio inputs (L + R) -> 2-ch WAV -> stereo playback
    //   Midi   - one MIDI input -> .mid sequence -> drives the strip's
    //            hosted plugin which produces the audible audio
    // Phase 1 ships only the model + the UI selector. Stereo audio capture
    // and MIDI capture/playback land in subsequent phases.
    enum class Mode : int { Mono = 0, Stereo = 1, Midi = 2 };

    juce::String name;
    juce::Colour colour;
    ChannelStripParams strip;

    std::atomic<int> mode { (int) Mode::Mono };  // store as int for atomic-ness

    // Recording surface
    std::atomic<bool> recordArmed { false };
    std::atomic<bool> inputMonitor { false };  // off by default - engineer engages IN explicitly when monitoring
                                              // (track still records and meters when armed)
    std::atomic<bool> printEffects { false };  // when true, the recorded WAV captures the post-EQ/post-comp
                                              // signal - "print effects on the way in". Default off so the
                                              // user can re-EQ/re-comp at mix time.
    std::atomic<int>  inputSource { -2 };  // mono mode + L of stereo mode:
                                            // -2 = follow track index (default),
                                            // -1 = no input,
                                            // 0..N = audio device input N
    std::atomic<int>  inputSourceR { -2 }; // R of stereo mode (same encoding;
                                            // -2 = inputSource + 1, paired adjacent)
    std::atomic<int>  midiInputIndex { -1 }; // -1 = none; 0..N = AudioDeviceManager
                                              // MIDI input index. Only used in Midi mode.

    // Recorded regions for this track (mutated only on the message thread).
    std::vector<AudioRegion> regions;

    // Per-channel plugin slot persistence. Populated from the live PluginSlot
    // immediately before SessionSerializer::save (via AudioEngine::publishPluginStateForSave)
    // and consumed immediately after SessionSerializer::load (via consumePluginStateAfterLoad).
    // Empty when no plugin is loaded. Both are message-thread-only fields -
    // not std::atomic - because plugin load/save runs message-thread.
    juce::String pluginDescriptionXml;  // juce::PluginDescription serialised as XML
    juce::String pluginStateBase64;     // raw plugin state (getStateInformation), base64-encoded

    // Metering surface - written from the audio thread, polled by the UI.
    std::atomic<float> meterGrDb     { 0.0f };     // peak gain reduction in dB (≤ 0)
    std::atomic<float> meterInputDb  { -100.0f };  // peak of channel source (live or playback) in dBFS
    // R-channel input peak. Written only when track.mode == Stereo and an R
    // input is resolved; left at -100 for mono / midi tracks. The UI uses
    // this to render a 2nd LED bar alongside the fader for stereo tracks.
    std::atomic<float> meterInputRDb { -100.0f };
};

// Aux bus surface. Phase 1a: fader + mute + solo + 3-band EQ + bus comp + pan
// + meter. Bus EQ uses 3 of BritishEQProcessor's 4 bands (LF / MID-as-LM / HF)
// with fixed musical frequencies. Bus comp uses UniversalCompressor in Bus
// mode (mode index 3).
struct AuxBusParams
{
    std::atomic<float> faderDb { 0.0f };
    std::atomic<float> pan     { 0.0f };  // -1..1 (left..right balance)
    std::atomic<bool>  mute    { false };
    std::atomic<bool>  solo    { false };

    // 3-band EQ.
    std::atomic<bool>  eqEnabled  { false };
    std::atomic<float> eqLfGainDb { 0.0f };  // -15..+15
    std::atomic<float> eqMidGainDb{ 0.0f };
    std::atomic<float> eqHfGainDb { 0.0f };

    // Bus compressor.
    std::atomic<bool>  compEnabled   { false };
    std::atomic<float> compThreshDb  { 0.0f };    // -30..0
    std::atomic<float> compRatio     { 4.0f };    // 1..10
    std::atomic<float> compAttackMs  { 10.0f };
    std::atomic<float> compReleaseMs { 100.0f };
    std::atomic<float> compMakeupDb  { 0.0f };

    // Metering - written from the audio thread, read from UI. mutable for
    // const-DSP-pointer write semantics (same pattern as MasterBusParams).
    // Stereo L+R for the bus LED meter.
    mutable std::atomic<float> meterPostBusLDb { -100.0f };
    mutable std::atomic<float> meterPostBusRDb { -100.0f };
    mutable std::atomic<float> meterGrDb       { 0.0f };
};

struct AuxBus
{
    juce::String name;
    juce::Colour colour;
    AuxBusParams strip;

    // Send-FX plugin slot persistence. Same shape as Track: populated from
    // the live PluginSlot by AudioEngine::publishPluginStateForSave before
    // session save, consumed by consumePluginStateAfterLoad on load. Empty
    // when no plugin is loaded.
    juce::String pluginDescriptionXml;
    juce::String pluginStateBase64;
};

struct MasterBusParams
{
    std::atomic<float> faderDb     { 0.0f };

    // Tape on/off + HQ. Internal saturation drive / bias / formulation are
    // fixed in the DSP; the user toggles only the engagement and the HQ
    // (4x oversampling) flag. Mirror the TapeMachine plugin's preset values
    // requested in the spec.
    std::atomic<bool>  tapeEnabled { false };
    std::atomic<bool>  tapeHQ      { false };

    // Pultec-style Tube EQ ("Pultec" in the spec, exposed as "Tube" mode in
    // multi-q). Minimal control surface: LF boost, HF boost, tube drive,
    // output gain - the four most musical Pultec controls - plus enable.
    // Frequencies and bandwidth are set to musical defaults internally.
    std::atomic<bool>  eqEnabled       { false };
    std::atomic<float> eqLfBoost       { 0.0f };   // 0..10 (Pultec gain knob)
    std::atomic<float> eqHfBoost       { 0.0f };   // 0..10
    std::atomic<float> eqHfAtten       { 0.0f };   // 0..10 (HF cut shelf)
    std::atomic<float> eqTubeDrive     { 0.3f };   // 0..1 (saturation amount)
    std::atomic<float> eqOutputGainDb  { 0.0f };   // -12..+12 dB

    // Master bus compressor - UniversalCompressor in Bus mode (mode index 3).
    // Minimal control surface mirroring a stock SSL-style bus comp.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<float> compThreshDb   { 0.0f };    // -30..0
    std::atomic<float> compRatio      { 4.0f };    // 1..10
    std::atomic<float> compAttackMs   { 10.0f };   // 0.1..50
    std::atomic<float> compReleaseMs  { 100.0f };  // 50..1000
    std::atomic<float> compMakeupDb   { 0.0f };    // -10..+20

    // Metering - written from the audio thread each block, polled by UI.
    // `mutable` so DSP code holding a `const MasterBusParams*` can update
    // these even through a const surface (write-by-DSP semantics). Stereo
    // L+R so the master LED can render two channels side by side.
    mutable std::atomic<float> meterPostMasterLDb { -100.0f };
    mutable std::atomic<float> meterPostMasterRDb { -100.0f };
    mutable std::atomic<float> meterGrDb          { 0.0f };
};

// Mastering-stage chain parameters. Independent of MasterBusParams so the
// user can dial in different EQ / comp on the bounced mix vs. the live mix.
// Persisted to / from session.json. Source file is the WAV being mastered;
// the path is absolute so projects moved across machines need a relink.
struct MasteringParams
{
    juce::File sourceFile;  // empty when no mix has been loaded yet

    // Five-band digital EQ for the mastering stage. Band 0 is a low shelf,
    // bands 1-3 are peaking bells, band 4 is a high shelf. Defaults match
    // MasteringDigitalEq::prepare() so loading a fresh session matches
    // the DSP's idle state.
    static constexpr int kNumEqBands = 5;
    std::atomic<bool>  eqEnabled       { false };
    std::atomic<float> eqBandFreq[kNumEqBands]  { 80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f };
    std::atomic<float> eqBandGainDb[kNumEqBands]{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::atomic<float> eqBandQ[kNumEqBands]     { 0.7f, 1.0f, 1.0f, 1.0f, 0.7f };

    // Legacy Tube-EQ atomics - kept for session.json backward compatibility
    // (sessions saved before the digital-EQ swap). Not driven by the DSP
    // any longer.
    std::atomic<float> eqLfBoost       { 0.0f };
    std::atomic<float> eqHfBoost       { 0.0f };
    std::atomic<float> eqHfAtten       { 0.0f };
    std::atomic<float> eqTubeDrive     { 0.3f };
    std::atomic<float> eqOutputGainDb  { 0.0f };

    // Bus comp - same shape as MasterBusParams. Mastering tends to use
    // slower release + softer ratio; defaults reflect that.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<float> compThreshDb   { 0.0f };
    std::atomic<float> compRatio      { 2.0f };
    std::atomic<float> compAttackMs   { 30.0f };
    std::atomic<float> compReleaseMs  { 250.0f };
    std::atomic<float> compMakeupDb   { 0.0f };

    // Brickwall limiter (final stage). Default ceiling -0.3 dB matches
    // common streaming-platform headroom.
    std::atomic<bool>  limiterEnabled  { true };
    std::atomic<float> limiterDriveDb  { 0.0f };
    std::atomic<float> limiterCeilingDb{ -0.3f };
    std::atomic<float> limiterReleaseMs{ 100.0f };

    // Output meters - mutable for write-by-DSP semantics.
    mutable std::atomic<float> meterPostMasterLDb { -100.0f };
    mutable std::atomic<float> meterPostMasterRDb { -100.0f };
    mutable std::atomic<float> meterCompGrDb     { 0.0f };
    mutable std::atomic<float> meterLimiterGrDb  { 0.0f };

    // BS.1770 / EBU R128 loudness - written from the mastering chain's
    // post-limiter LoudnessMeter. M = momentary (400 ms), S = short-term
    // (3 s), I = integrated (program-wide, gated). True peak is in dBTP
    // (4× oversampled per ITU BS.1770 Annex 2).
    mutable std::atomic<float> meterMomentaryLufs   { -100.0f };
    mutable std::atomic<float> meterShortTermLufs   { -100.0f };
    mutable std::atomic<float> meterIntegratedLufs  { -100.0f };
    mutable std::atomic<float> meterTruePeakDb      { -100.0f };

    // Streaming-platform target preset. Drives the colour-coding of the
    // I-LUFS / true-peak cells in MasteringView. 0 = Off (neutral display);
    // see kMasteringTargets in RegionEditActions.cpp / MasteringView for
    // the canonical preset table. Persisted with the session.
    std::atomic<int> targetPresetIndex { 0 };
};

class Session
{
public:
    static constexpr int kNumTracks   = 16;
    static constexpr int kNumAuxBuses = 4;
    static constexpr int kBankSize    = 8;
    static constexpr int kNumBanks    = kNumTracks / kBankSize;

    Session();

    juce::File getSessionDirectory() const noexcept { return sessionDir; }
    juce::File getAudioDirectory()   const noexcept { return sessionDir.getChildFile ("audio"); }
    void setSessionDirectory (const juce::File& dir);

    Track& track (int i) noexcept             { jassert (i >= 0 && i < kNumTracks);   return tracks[(size_t) i]; }
    const Track& track (int i) const noexcept { jassert (i >= 0 && i < kNumTracks);   return tracks[(size_t) i]; }

    AuxBus& aux (int i) noexcept              { jassert (i >= 0 && i < kNumAuxBuses); return auxBuses[(size_t) i]; }
    const AuxBus& aux (int i) const noexcept  { jassert (i >= 0 && i < kNumAuxBuses); return auxBuses[(size_t) i]; }

    MasterBusParams& master() noexcept             { return masterParams; }
    const MasterBusParams& master() const noexcept { return masterParams; }

    MasteringParams&       mastering() noexcept       { return masteringParams; }
    const MasteringParams& mastering() const noexcept { return masteringParams; }

    // Audio-thread "any soloed / armed?" queries. O(1) - back-by atomic
    // counters that the UI bumps via the setters below. Bulk-write paths
    // (SessionSerializer / self-test) write the underlying atoms directly,
    // then call recomputeRtCounters() to resync. Without the counters these
    // queries scanned 16 / 4 / 16 atoms every callback before the per-track
    // loop even started.
    bool anyTrackSoloed() const noexcept;
    bool anyAuxSoloed()   const noexcept;
    bool anyTrackArmed()  const noexcept;

    // Counter-aware setters. Atomically toggle the bool AND adjust the
    // corresponding count, so anyXSoloed() stays correct without scans.
    // No-op when the value didn't actually change. Call from message thread.
    void setTrackSoloed (int trackIndex, bool soloed) noexcept;
    void setAuxSoloed   (int auxIndex,   bool soloed) noexcept;
    void setTrackArmed  (int trackIndex, bool armed)  noexcept;

    // Re-scan every track / aux atom and rebuild the counters. Call after a
    // bulk path that wrote the atoms directly (session load, self-test) so
    // the audio thread's any-X-soloed reads are correct on the next callback.
    void recomputeRtCounters() noexcept;

    // Persisted transport settings - mirrored to/from Transport's atomics
    // by AudioEngine::publishTransportStateForSave / consume so they round
    // trip through SessionSerializer. Plain (non-atomic) because they are
    // touched only on the message thread, between save/load and the
    // engine's bookend calls.
    juce::int64 savedLoopStart    = 0;
    juce::int64 savedLoopEnd      = 0;
    bool        savedLoopEnabled  = false;
    juce::int64 savedPunchIn      = 0;
    juce::int64 savedPunchOut     = 0;
    bool        savedPunchEnabled = false;

    // Region-edit snap. When true, region drags (move + trim) round to
    // beat boundaries (1 beat = 60 / tempoBpm seconds) when tempoBpm > 0,
    // or to 1-second boundaries otherwise. Touched only on the message
    // thread.
    bool snapToGrid = false;

    // Global oversampling factor applied to effects that support it (master
    // tape sat, master/aux bus comp). Per-channel comp + EQ run at native
    // rate regardless. 1 = native (default - lowest CPU), 2 = 2× ox, 4 =
    // 4× ox. Read by MasterBus and AuxBusStrip in prepare(); changing it
    // requires a re-prepare (the AudioSettingsPanel triggers one via
    // setAudioDeviceSetup). Stored as an atomic<int> so future audio-thread
    // reads (e.g. for per-block reactive ox) are lock-free, but today only
    // the message-thread prepare path consults it.
    std::atomic<int> oversamplingFactor { 1 };

    // Tempo + transport-grid metadata. Persisted with the session.
    //   tempoBpm        - beats per minute. 0 disables beat-grid behavior
    //                      (metronome stays silent, snap falls back to seconds).
    //   beatsPerBar     - numerator of the time signature (e.g. 4 in 4/4).
    //   beatUnit        - denominator (4 = quarter, 8 = eighth, etc.).
    //   metronomeEnabled - click on/off.
    //   metronomeVolDb  - click level relative to master output (negative).
    // BPM and metronomeEnabled are atomics so the audio thread can pick up
    // tempo changes without a lock.
    std::atomic<float> tempoBpm          { 120.0f };
    std::atomic<int>   beatsPerBar       { 4 };
    std::atomic<int>   beatUnit          { 4 };
    std::atomic<bool>  metronomeEnabled  { false };
    std::atomic<float> metronomeVolDb    { -12.0f };

    // Count-in (pre-roll). When true, hitting Record shifts the playhead
    // backwards by one bar so the metronome ticks for one bar before
    // capture begins. The captured WAV's first sample still maps to the
    // playhead position at Record-press, not the negative pre-roll start.
    std::atomic<bool>  countInEnabled    { false };

    // Resolve the audio device input channel that this track should read from.
    // -2 (default) means "follow the track index", -1 means "no input".
    int resolveInputForTrack (int trackIndex) const noexcept;
    // R-channel input resolver - mirrors resolveInputForTrack but reads
    // inputSourceR. Returns -1 when no R input is configured, or in Mono /
    // Midi mode (where the second channel is meaningless).
    int resolveInputRForTrack (int trackIndex) const noexcept;

private:
    std::array<Track, kNumTracks> tracks;
    std::array<AuxBus, kNumAuxBuses> auxBuses;
    MasterBusParams masterParams;
    MasteringParams masteringParams;
    juce::File sessionDir;

    // RT-perf: counter-backed any-X-soloed / -armed queries. Each is
    // incremented when a flag goes false->true and decremented true->false
    // by the matching setter; the audio thread does a single relaxed load
    // per callback instead of scanning all 16 / 4 atoms.
    std::atomic<int> soloTrackCount { 0 };
    std::atomic<int> soloAuxCount   { 0 };
    std::atomic<int> armedTrackCount { 0 };
};
} // namespace focal
