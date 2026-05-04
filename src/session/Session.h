#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace adhdaw
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
    std::atomic<bool>  phaseInvert { false };  // Ø — flips polarity at strip input

    // Per-channel bus assigns. Bit i is true => post-fader signal also sums
    // into BUS (i+1) at unity. Channel always sums to master regardless of
    // bus assigns. Knob-style FX sends (with level) will be a separate
    // surface added later.
    static constexpr int kNumBuses = 4;
    std::array<std::atomic<bool>, kNumBuses> busAssign {};

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

    // Legacy unified threshold — still driven by the comp meter strip's drag
    // handle. Mirrors the *current mode's* primary compression knob so the
    // drag stays usable regardless of which mode is active. The audio thread
    // does NOT read this directly any more — see ChannelStrip::updateCompParameters.
    std::atomic<float> compThresholdDb { 0.0f };

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

// Audio region — references a mono WAV file on disk. Phase 2 minimum: a
// region is a single recorded take; later phases add take history, trim,
// copy/paste etc. per the spec.
struct AudioRegion
{
    juce::File file;                  // absolute path to the WAV
    juce::int64 timelineStart = 0;    // sample position on the timeline
    juce::int64 lengthInSamples = 0;
    juce::int64 sourceOffset = 0;     // future: trim from left
};

struct Track
{
    juce::String name;
    juce::Colour colour;
    ChannelStripParams strip;

    // Recording surface
    std::atomic<bool> recordArmed { false };
    std::atomic<bool> inputMonitor { false };  // off by default — engineer engages IN explicitly when monitoring
                                              // (track still records and meters when armed)
    std::atomic<bool> printEffects { false };  // when true, the recorded WAV captures the post-EQ/post-comp
                                              // signal — "print effects on the way in". Default off so the
                                              // user can re-EQ/re-comp at mix time.
    std::atomic<int>  inputSource { -2 };  // -2 = follow track index (default),
                                            // -1 = no input,
                                            // 0..N = audio device input N

    // Recorded regions for this track (mutated only on the message thread).
    std::vector<AudioRegion> regions;

    // Metering surface — written from the audio thread, polled by the UI.
    std::atomic<float> meterGrDb     { 0.0f };     // peak gain reduction in dB (≤ 0)
    std::atomic<float> meterInputDb  { -100.0f };  // peak of channel source (live or playback) in dBFS
};

// Aux bus surface. Phase 1a wires fader + mute + solo only; the EQ + bus
// compressor described in the spec land when the corresponding DSP cores ship.
struct AuxBusParams
{
    std::atomic<float> faderDb { 0.0f };
    std::atomic<bool>  mute    { false };
    std::atomic<bool>  solo    { false };
};

struct AuxBus
{
    juce::String name;
    juce::Colour colour;
    AuxBusParams strip;
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

    bool anyTrackSoloed() const noexcept;
    bool anyAuxSoloed()   const noexcept;
    bool anyTrackArmed()  const noexcept;

    // Resolve the audio device input channel that this track should read from.
    // -2 (default) means "follow the track index", -1 means "no input".
    int resolveInputForTrack (int trackIndex) const noexcept;

private:
    std::array<Track, kNumTracks> tracks;
    std::array<AuxBus, kNumAuxBuses> auxBuses;
    MasterBusParams masterParams;
    juce::File sessionDir;
};
} // namespace adhdaw
