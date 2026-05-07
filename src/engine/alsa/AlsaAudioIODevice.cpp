#include "AlsaAudioIODevice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sys/resource.h>

namespace focal
{
namespace
{
// ----- Format priority -------------------------------------------------------
//
// Try-order is by reliability across the long tail of audio interfaces, NOT
// by float-vs-int convenience. S32_LE has zero alignment ambiguity (4-byte
// container, 4-byte aligned, no packing). S16_LE is universally supported.
// S24_LE (24-in-32) is the next-most-aligned 24-bit option. S24_3LE (packed
// 3-byte) goes near the end because some USB-class-compliant devices
// advertise it but glitch on the actual byte alignment. FLOAT_LE is last
// because it's the format most likely to be falsely advertised by drivers
// that secretly convert from int.
struct AlsaFormat
{
    snd_pcm_format_t alsa;
    int              bytesPerSample;
    int              bitDepth;
    bool             isFloat;
    const char*      label;
};

constexpr AlsaFormat kFormats[] = {
    { SND_PCM_FORMAT_S32_LE,  4, 32, false, "S32_LE"  },
    { SND_PCM_FORMAT_S16_LE,  2, 16, false, "S16_LE"  },
    { SND_PCM_FORMAT_S24_LE,  4, 24, false, "S24_LE"  },  // 24-in-32 LSB-aligned
    { SND_PCM_FORMAT_S24_3LE, 3, 24, false, "S24_3LE" },  // packed 3-byte
    { SND_PCM_FORMAT_FLOAT_LE,4, 32, true,  "FLOAT_LE"},
};
constexpr int kNumFormats = (int) (sizeof (kFormats) / sizeof (kFormats[0]));

const AlsaFormat* selectFormat (snd_pcm_t* handle, snd_pcm_hw_params_t* hwParams)
{
    for (int i = 0; i < kNumFormats; ++i)
        if (snd_pcm_hw_params_set_format (handle, hwParams, kFormats[i].alsa) >= 0)
            return &kFormats[i];
    return nullptr;
}

// ----- Per-format float<->int conversion -------------------------------------
//
// Per-period dispatch. The branch on format happens ONCE outside the per-frame
// loop. Endianness is host-LE (we ship for x86_64 Linux); the LE formats are
// memcpy-able directly. For sample scaling we use the standard
// (max_int_value) factor to map float [-1, 1] onto the full int range.
//
// Inactive channels are written as zero into the interleaved frame; the
// caller arranges the active-channel-index mapping.

constexpr float kInt16Scale = 32767.0f;
constexpr float kInt24Scale = 8388607.0f;
constexpr float kInt32Scale = 2147483647.0f;

inline float clampUnit (float v) noexcept
{
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

void writeInterleavedS32 (const juce::AudioBuffer<float>& src, void* dest,
                           int numFrames, int numDeviceChannels,
                           const int* activeIdx, int numActive)
{
    auto* out = static_cast<int32_t*> (dest);
    std::memset (out, 0, sizeof (int32_t) * (size_t) (numFrames * numDeviceChannels));
    for (int a = 0; a < numActive; ++a)
    {
        const auto* srcCh = src.getReadPointer (a);
        const int   col   = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
            out[f * numDeviceChannels + col] = (int32_t) std::lrint (clampUnit (srcCh[f]) * kInt32Scale);
    }
}

void writeInterleavedS16 (const juce::AudioBuffer<float>& src, void* dest,
                           int numFrames, int numDeviceChannels,
                           const int* activeIdx, int numActive)
{
    auto* out = static_cast<int16_t*> (dest);
    std::memset (out, 0, sizeof (int16_t) * (size_t) (numFrames * numDeviceChannels));
    for (int a = 0; a < numActive; ++a)
    {
        const auto* srcCh = src.getReadPointer (a);
        const int   col   = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
            out[f * numDeviceChannels + col] = (int16_t) std::lrint (clampUnit (srcCh[f]) * kInt16Scale);
    }
}

void writeInterleavedS24in32 (const juce::AudioBuffer<float>& src, void* dest,
                               int numFrames, int numDeviceChannels,
                               const int* activeIdx, int numActive)
{
    // 24-bit signed sample stored in the LOWER 24 bits of a 32-bit container,
    // sign-extended through bits 24..31.
    auto* out = static_cast<int32_t*> (dest);
    std::memset (out, 0, sizeof (int32_t) * (size_t) (numFrames * numDeviceChannels));
    for (int a = 0; a < numActive; ++a)
    {
        const auto* srcCh = src.getReadPointer (a);
        const int   col   = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
        {
            const int32_t v = (int32_t) std::lrint (clampUnit (srcCh[f]) * kInt24Scale);
            // The driver sees a 32-bit container; sign-extension via shift
            // ensures negative values keep their high bits set.
            out[f * numDeviceChannels + col] = (v << 8) >> 8;
        }
    }
}

void writeInterleavedS24Packed (const juce::AudioBuffer<float>& src, void* dest,
                                  int numFrames, int numDeviceChannels,
                                  const int* activeIdx, int numActive)
{
    auto* out = static_cast<uint8_t*> (dest);
    std::memset (out, 0, (size_t) (3 * numFrames * numDeviceChannels));
    for (int a = 0; a < numActive; ++a)
    {
        const auto* srcCh = src.getReadPointer (a);
        const int   col   = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
        {
            const int32_t v = (int32_t) std::lrint (clampUnit (srcCh[f]) * kInt24Scale);
            uint8_t* p = out + 3 * (f * numDeviceChannels + col);
            p[0] = (uint8_t) (v        & 0xff);
            p[1] = (uint8_t) ((v >> 8 ) & 0xff);
            p[2] = (uint8_t) ((v >> 16) & 0xff);
        }
    }
}

void writeInterleavedFloat (const juce::AudioBuffer<float>& src, void* dest,
                              int numFrames, int numDeviceChannels,
                              const int* activeIdx, int numActive)
{
    auto* out = static_cast<float*> (dest);
    std::memset (out, 0, sizeof (float) * (size_t) (numFrames * numDeviceChannels));
    for (int a = 0; a < numActive; ++a)
    {
        const auto* srcCh = src.getReadPointer (a);
        const int   col   = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
            out[f * numDeviceChannels + col] = srcCh[f];
    }
}

// Capture direction: deinterleave from device frame into the float buffer.
// Only the active subset of device channels is materialised into floats; the
// rest of the device's interleaved data is ignored.

void readInterleavedS32 (const void* src, juce::AudioBuffer<float>& dest,
                          int numFrames, int numDeviceChannels,
                          const int* activeIdx, int numActive)
{
    const auto* in = static_cast<const int32_t*> (src);
    constexpr float invScale = 1.0f / kInt32Scale;
    for (int a = 0; a < numActive; ++a)
    {
        auto*     destCh = dest.getWritePointer (a);
        const int col    = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
            destCh[f] = (float) in[f * numDeviceChannels + col] * invScale;
    }
}

void readInterleavedS16 (const void* src, juce::AudioBuffer<float>& dest,
                          int numFrames, int numDeviceChannels,
                          const int* activeIdx, int numActive)
{
    const auto* in = static_cast<const int16_t*> (src);
    constexpr float invScale = 1.0f / kInt16Scale;
    for (int a = 0; a < numActive; ++a)
    {
        auto*     destCh = dest.getWritePointer (a);
        const int col    = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
            destCh[f] = (float) in[f * numDeviceChannels + col] * invScale;
    }
}

void readInterleavedS24in32 (const void* src, juce::AudioBuffer<float>& dest,
                              int numFrames, int numDeviceChannels,
                              const int* activeIdx, int numActive)
{
    const auto* in = static_cast<const int32_t*> (src);
    constexpr float invScale = 1.0f / kInt24Scale;
    for (int a = 0; a < numActive; ++a)
    {
        auto*     destCh = dest.getWritePointer (a);
        const int col    = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
        {
            // Sign-extend the lower 24 bits before scaling.
            const int32_t v = (in[f * numDeviceChannels + col] << 8) >> 8;
            destCh[f] = (float) v * invScale;
        }
    }
}

void readInterleavedS24Packed (const void* src, juce::AudioBuffer<float>& dest,
                                 int numFrames, int numDeviceChannels,
                                 const int* activeIdx, int numActive)
{
    const auto* in = static_cast<const uint8_t*> (src);
    constexpr float invScale = 1.0f / kInt24Scale;
    for (int a = 0; a < numActive; ++a)
    {
        auto*     destCh = dest.getWritePointer (a);
        const int col    = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
        {
            const uint8_t* p = in + 3 * (f * numDeviceChannels + col);
            int32_t v = (int32_t) p[0]
                       | ((int32_t) p[1] << 8)
                       | ((int32_t) p[2] << 16);
            // Sign-extend a 24-bit value held in 32 bits.
            if (v & 0x00800000) v |= 0xff000000;
            destCh[f] = (float) v * invScale;
        }
    }
}

void readInterleavedFloat (const void* src, juce::AudioBuffer<float>& dest,
                             int numFrames, int numDeviceChannels,
                             const int* activeIdx, int numActive)
{
    const auto* in = static_cast<const float*> (src);
    for (int a = 0; a < numActive; ++a)
    {
        auto*     destCh = dest.getWritePointer (a);
        const int col    = activeIdx[a];
        for (int f = 0; f < numFrames; ++f)
            destCh[f] = in[f * numDeviceChannels + col];
    }
}

// ----- Periods knob ----------------------------------------------------------
//
// 2 is the Ardour default and the lowest-latency setting that gives the kernel
// any slack. Range [2, 16]. UI lives in AudioSettingsPanel and updates this
// before triggering a re-open of the device.
std::atomic<int> gRequestedPeriods { 2 };

// ----- Sample rate / buffer size candidates ----------------------------------
//
// The device's actual support is checked by ALSA at open(); these are just
// the values we surface to JUCE's UI. Standard pro-audio rates and sizes.
const double kCandidateRates[] = {
    44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
};
const int kCandidateBufferSizes[] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096
};
} // namespace

void AlsaAudioIODevice::setRequestedPeriods (int p) noexcept
{
    gRequestedPeriods.store (juce::jlimit (2, 16, p), std::memory_order_relaxed);
}
int AlsaAudioIODevice::getRequestedPeriods() noexcept
{
    return gRequestedPeriods.load (std::memory_order_relaxed);
}

// ============================================================================
AlsaAudioIODevice::AlsaAudioIODevice (const juce::String& name,
                                        const juce::String& inId,
                                        const juce::String& outId)
    : juce::AudioIODevice (name, "ALSA (Focal)"),
      juce::Thread ("Focal-ALSA-IO"),
      inputId (inId),
      outputId (outId),
      displayName (name)
{
}

AlsaAudioIODevice::~AlsaAudioIODevice()
{
    close();
}

// ----- Capability queries ----------------------------------------------------
void AlsaAudioIODevice::probeIfNeeded()
{
    if (hasProbed)
        return;

    auto probe = [] (const juce::String& id, bool isCapture,
                      unsigned int& maxChans,
                      juce::Array<double>& rates,
                      juce::Array<int>& bufSizes)
    {
        if (id.isEmpty())
            return;

        snd_pcm_t* handle = nullptr;
        if (snd_pcm_open (&handle,
                           id.toRawUTF8(),
                           isCapture ? SND_PCM_STREAM_CAPTURE
                                     : SND_PCM_STREAM_PLAYBACK,
                           SND_PCM_NONBLOCK) < 0)
            return;

        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_alloca (&hw);
        if (snd_pcm_hw_params_any (handle, hw) >= 0)
        {
            unsigned int minCh = 0, maxCh = 0;
            snd_pcm_hw_params_get_channels_min (hw, &minCh);
            snd_pcm_hw_params_get_channels_max (hw, &maxCh);
            maxChans = std::max (maxChans, maxCh);

            for (double r : kCandidateRates)
                if (snd_pcm_hw_params_test_rate (handle, hw, (unsigned int) r, 0) == 0)
                    rates.addIfNotAlreadyThere (r);

            for (int b : kCandidateBufferSizes)
            {
                snd_pcm_uframes_t f = (snd_pcm_uframes_t) b;
                if (snd_pcm_hw_params_test_period_size (handle, hw, f, 0) == 0)
                    bufSizes.addIfNotAlreadyThere (b);
            }
        }

        snd_pcm_close (handle);
    };

    probe (outputId, false, deviceMaxOutChannels, supportedSampleRates, supportedBufferSizes);
    probe (inputId,  true,  deviceMaxInChannels,  supportedSampleRates, supportedBufferSizes);

    if (supportedSampleRates.isEmpty())
        for (double r : kCandidateRates) supportedSampleRates.add (r);
    if (supportedBufferSizes.isEmpty())
        for (int b : kCandidateBufferSizes) supportedBufferSizes.add (b);

    std::sort (supportedSampleRates.begin(), supportedSampleRates.end());
    std::sort (supportedBufferSizes.begin(), supportedBufferSizes.end());

    hasProbed = true;
}

juce::StringArray AlsaAudioIODevice::getOutputChannelNames()
{
    probeIfNeeded();
    juce::StringArray names;
    for (unsigned int i = 0; i < deviceMaxOutChannels; ++i)
        names.add ("channel " + juce::String ((int) i + 1));
    return names;
}

juce::StringArray AlsaAudioIODevice::getInputChannelNames()
{
    probeIfNeeded();
    juce::StringArray names;
    for (unsigned int i = 0; i < deviceMaxInChannels; ++i)
        names.add ("channel " + juce::String ((int) i + 1));
    return names;
}

juce::Array<double> AlsaAudioIODevice::getAvailableSampleRates()
{
    probeIfNeeded();
    return supportedSampleRates;
}

juce::Array<int> AlsaAudioIODevice::getAvailableBufferSizes()
{
    probeIfNeeded();
    return supportedBufferSizes;
}

int AlsaAudioIODevice::getDefaultBufferSize()
{
    return 1024;  // Conservative default; small enough for studio use, large
                  // enough to be stable on USB-class devices out of the box.
}

// ----- Open / close ----------------------------------------------------------
bool AlsaAudioIODevice::openOneHandle (const juce::String& id, bool isCapture, snd_pcm_t*& handle)
{
    handle = nullptr;
    const int err = snd_pcm_open (&handle,
                                    id.toRawUTF8(),
                                    isCapture ? SND_PCM_STREAM_CAPTURE
                                              : SND_PCM_STREAM_PLAYBACK,
                                    SND_PCM_NONBLOCK);
    if (err < 0)
    {
        lastError = juce::String ("snd_pcm_open(") + id + ", "
                  + (isCapture ? "capture" : "playback") + "): "
                  + snd_strerror (err);
        handle = nullptr;
        return false;
    }
    return true;
}

bool AlsaAudioIODevice::configurePcm (snd_pcm_t* handle, bool isCapture,
                                        unsigned int& sampleRate,
                                        unsigned int& numChannels,
                                        snd_pcm_uframes_t& period,
                                        unsigned int& periods,
                                        int& bytesPerSample, bool& isFloat)
{
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca (&hw);

    if (snd_pcm_hw_params_any (handle, hw) < 0)
    {
        lastError = "snd_pcm_hw_params_any failed";
        return false;
    }

    if (snd_pcm_hw_params_set_access (handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
    {
        lastError = "device does not support RW_INTERLEAVED access";
        return false;
    }

    const AlsaFormat* fmt = selectFormat (handle, hw);
    if (fmt == nullptr)
    {
        lastError = "no compatible PCM format";
        return false;
    }
    bytesPerSample = fmt->bytesPerSample;
    isFloat        = fmt->isFloat;
    openedBitDepth = fmt->bitDepth;

    if (snd_pcm_hw_params_set_rate_near (handle, hw, &sampleRate, nullptr) < 0)
    {
        lastError = "device rejected sample rate " + juce::String ((int) sampleRate);
        return false;
    }

    {
        unsigned int minCh = 0, maxCh = 0;
        snd_pcm_hw_params_get_channels_min (hw, &minCh);
        snd_pcm_hw_params_get_channels_max (hw, &maxCh);
        numChannels = juce::jlimit (minCh, maxCh, numChannels);
        if (snd_pcm_hw_params_set_channels (handle, hw, numChannels) < 0)
        {
            lastError = "device rejected channel count " + juce::String ((int) numChannels);
            return false;
        }
    }

    snd_pcm_hw_params_set_periods_integer (handle, hw);  // Ardour does this; harmless if it fails

    if (snd_pcm_hw_params_set_period_size_near (handle, hw, &period, nullptr) < 0)
    {
        lastError = "device rejected period size";
        return false;
    }
    if (snd_pcm_hw_params_set_periods_near (handle, hw, &periods, nullptr) < 0)
    {
        lastError = "device rejected period count";
        return false;
    }

    if (snd_pcm_hw_params (handle, hw) < 0)
    {
        lastError = "snd_pcm_hw_params commit failed";
        return false;
    }

    // Software params - conservative and small. start_threshold = period_size
    // so the device starts after one period is buffered. stop_threshold =
    // buffer_size means underruns DO stop the stream and trigger recovery
    // (audible click on a glitch, but no smearing - the right tradeoff for
    // pro audio). No silence_size override.
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca (&sw);
    if (snd_pcm_sw_params_current (handle, sw) < 0
        || snd_pcm_sw_params_set_start_threshold (handle, sw, isCapture ? 0 : period) < 0
        || snd_pcm_sw_params_set_stop_threshold (handle, sw, period * periods) < 0
        || snd_pcm_sw_params_set_avail_min (handle, sw, period) < 0
        || snd_pcm_sw_params (handle, sw) < 0)
    {
        lastError = "snd_pcm_sw_params commit failed";
        return false;
    }

    return true;
}

juce::String AlsaAudioIODevice::open (const juce::BigInteger& inputChannels,
                                        const juce::BigInteger& outputChannels,
                                        double sampleRate, int bufferSizeSamples)
{
    if (isDeviceOpen.load (std::memory_order_acquire))
        close();

    probeIfNeeded();

    currentInputChannels  = inputChannels;
    currentOutputChannels = outputChannels;

    const bool wantOutput = outputChannels.getHighestBit() >= 0 && outputId.isNotEmpty();
    const bool wantInput  = inputChannels.getHighestBit()  >= 0 && inputId.isNotEmpty();

    if (! wantOutput && ! wantInput)
    {
        lastError = "no input or output channels selected";
        return lastError;
    }

    snd_pcm_uframes_t period = (snd_pcm_uframes_t) juce::jmax (32, bufferSizeSamples);
    unsigned int     periods = (unsigned int) gRequestedPeriods.load (std::memory_order_relaxed);
    unsigned int     rate    = (unsigned int) sampleRate;

    // Open PLAYBACK first - some interfaces sequence better that way (the
    // capture clock derives from the playback master in many class-compliant
    // designs). Either order works for most.
    if (wantOutput)
    {
        if (! openOneHandle (outputId, false, outHandle))
            return lastError;

        outNumChannels = (unsigned int) deviceMaxOutChannels;
        if (! configurePcm (outHandle, false, rate, outNumChannels,
                              period, periods,
                              bytesPerOutSample, openedIsFloat))
        {
            close();
            return lastError;
        }
    }

    if (wantInput)
    {
        if (! openOneHandle (inputId, true, inHandle))
        {
            close();
            return lastError;
        }

        inNumChannels = (unsigned int) deviceMaxInChannels;
        unsigned int inRate = rate;  // require capture to match playback exactly
        bool         inFloat = false;
        int          inBytes = 0;
        snd_pcm_uframes_t inPeriod  = period;
        unsigned int     inPeriods = periods;
        if (! configurePcm (inHandle, true, inRate, inNumChannels,
                              inPeriod, inPeriods, inBytes, inFloat))
        {
            close();
            return lastError;
        }
        bytesPerInSample = inBytes;
        if (inRate != rate)
        {
            lastError = "capture rate " + juce::String ((int) inRate)
                       + " != playback rate " + juce::String ((int) rate);
            close();
            return lastError;
        }
    }

    // snd_pcm_link gives both directions a shared start trigger and shared
    // hardware clock. Without this, capture and playback can drift (audible
    // as a periodic phase ripple) and recovery on one direction leaves the
    // other in an unrecovered state.
    if (outHandle != nullptr && inHandle != nullptr)
        snd_pcm_link (outHandle, inHandle);

    openedSampleRate = rate;
    periodSize       = (int) period;
    periodsCount     = (int) periods;
    outputLatency    = wantOutput ? (int) (period * (periods - 1)) : 0;
    inputLatency     = wantInput  ? (int) period : 0;

    // Active-channel index tables. Callback channel `a` corresponds to device
    // channel `activeXXXDeviceChannelIndex[a]`. Mask is the user's UI ticks.
    activeOutDeviceChannelIndex.clearQuick();
    activeInDeviceChannelIndex.clearQuick();
    if (wantOutput)
        for (int i = 0; i <= outputChannels.getHighestBit(); ++i)
            if (outputChannels[i])
                activeOutDeviceChannelIndex.add (i);
    if (wantInput)
        for (int i = 0; i <= inputChannels.getHighestBit(); ++i)
            if (inputChannels[i])
                activeInDeviceChannelIndex.add (i);

    // Pre-size scratch. Allocations on the audio thread are forbidden; the
    // sizing here is the size the I/O loop relies on.
    const int activeOut = activeOutDeviceChannelIndex.size();
    const int activeIn  = activeInDeviceChannelIndex.size();

    callbackOutFloats.setSize (juce::jmax (1, activeOut), periodSize, false, true, false);
    callbackInFloats .setSize (juce::jmax (1, activeIn),  periodSize, false, true, false);
    callbackOutPointers.resize (activeOut);
    callbackInPointers .resize (activeIn);
    for (int i = 0; i < activeOut; ++i) callbackOutPointers.set (i, callbackOutFloats.getWritePointer (i));
    for (int i = 0; i < activeIn;  ++i) callbackInPointers .set (i, callbackInFloats .getReadPointer  (i));

    if (wantOutput)
        interleavedOutBytes.allocate ((size_t) (bytesPerOutSample * (int) outNumChannels * periodSize), true);
    if (wantInput)
        interleavedInBytes .allocate ((size_t) (bytesPerInSample  * (int) inNumChannels  * periodSize), true);

    if (outHandle != nullptr) snd_pcm_prepare (outHandle);
    if (inHandle  != nullptr) snd_pcm_prepare (inHandle);

    isDeviceOpen.store (true, std::memory_order_release);
    lastError.clear();

    std::fprintf (stderr,
                  "[Focal/ALSA] opened \"%s\" rate=%u period=%d periods=%d bits=%d %s "
                  "out=%uch in=%uch (active out=%d in=%d)\n",
                  displayName.toRawUTF8(),
                  (unsigned) rate, (int) period, (int) periods, openedBitDepth,
                  openedIsFloat ? "float" : "int",
                  outNumChannels, inNumChannels, activeOut, activeIn);

    return {};
}

void AlsaAudioIODevice::close()
{
    stop();

    if (outHandle != nullptr) { snd_pcm_close (outHandle); outHandle = nullptr; }
    if (inHandle  != nullptr) { snd_pcm_close (inHandle);  inHandle  = nullptr; }

    interleavedOutBytes.free();
    interleavedInBytes .free();
    callbackOutFloats.setSize (1, 1);
    callbackInFloats .setSize (1, 1);

    isDeviceOpen.store (false, std::memory_order_release);
}

// ----- Start / stop ----------------------------------------------------------
void AlsaAudioIODevice::start (juce::AudioIODeviceCallback* newCallback)
{
    if (! isDeviceOpen.load (std::memory_order_acquire) || newCallback == nullptr)
        return;
    if (isStarted.load (std::memory_order_acquire))
        return;

    if (newCallback != nullptr)
        newCallback->audioDeviceAboutToStart (this);

    {
        const juce::ScopedLock sl (callbackLock);
        callback = newCallback;
    }

    xrunCount.store (0, std::memory_order_relaxed);

    // Linked-pair start ritual. With snd_pcm_link, capture and playback share
    // a start trigger - whoever crosses its start_threshold first starts the
    // other. Capture's threshold is 0, so the first readi would trigger
    // start - but with playback's buffer empty, that's an instant underrun
    // and the device halts before any callback-produced audio reaches the
    // DAC (which is exactly the "meters move, no sound" symptom).
    //
    // Fix: pre-fill the playback ring buffer with `period * periods` frames
    // of silence BEFORE the I/O thread runs. This crosses playback's
    // start_threshold during the writei itself, the linked capture starts
    // along with us, and the loop's first readi gets real data. After the
    // loop's first writei, the buffer level oscillates between
    // (periods-1)*period and periods*period frames - normal steady state.
    //
    // For input-only or output-only opens we just snd_pcm_start the open
    // direction; with no link, capture's threshold-of-0 also makes the
    // first readi start the stream cleanly, but explicit start is the
    // belt-and-braces option.
    if (outHandle != nullptr)
    {
        const int frameBytes = bytesPerOutSample * (int) outNumChannels;
        const auto preFillFrames = (size_t) (periodSize * periodsCount);
        juce::HeapBlock<char> silence (preFillFrames * (size_t) frameBytes, true);

        snd_pcm_sframes_t wrote = snd_pcm_writei (outHandle, silence.getData(),
                                                    (snd_pcm_uframes_t) preFillFrames);
        if (wrote < 0)
        {
            // EPIPE here means the device went into XRUN state during prepare
            // (rare, but seen on some USB drivers). Recover and retry once.
            recoverFromXrun (outHandle, (int) wrote);
            snd_pcm_writei (outHandle, silence.getData(),
                            (snd_pcm_uframes_t) preFillFrames);
        }
    }

    // Explicit start - idempotent if start_threshold has already fired the
    // device via the pre-fill writei above. With snd_pcm_link, calling on
    // either handle starts both.
    snd_pcm_t* startHandle = (outHandle != nullptr) ? outHandle : inHandle;
    if (startHandle != nullptr
        && snd_pcm_state (startHandle) != SND_PCM_STATE_RUNNING)
    {
        snd_pcm_start (startHandle);
    }

    // Pick a SCHED_RR priority safely below RLIMIT_RTPRIO so pthread_create
    // doesn't EPERM. Same logic as JUCE's patched ALSA backend.
    int juceRtPrio = 8;
    struct rlimit rl{};
    if (getrlimit (RLIMIT_RTPRIO, &rl) == 0 && rl.rlim_cur > 1)
    {
        const long capped     = (long) rl.rlim_cur - 4;
        const long juceScale  = (capped * 10 + 49) / 99;
        juceRtPrio = (int) juce::jlimit ((long) 0, (long) 10, juceScale);
    }

    const bool gotRT = startRealtimeThread (juce::Thread::RealtimeOptions{}.withPriority (juceRtPrio));
    if (! gotRT)
        startThread (juce::Thread::Priority::high);

    std::fprintf (stderr, "[Focal/ALSA] thread started: %s (juce-prio=%d, RLIMIT_RTPRIO=%d)\n",
                  gotRT ? "SCHED_RR" : "Priority::high",
                  juceRtPrio, (int) rl.rlim_cur);

    isStarted.store (true, std::memory_order_release);
}

void AlsaAudioIODevice::stop()
{
    if (! isStarted.load (std::memory_order_acquire))
        return;

    isStarted.store (false, std::memory_order_release);
    signalThreadShouldExit();
    stopThread (2000);

    juce::AudioIODeviceCallback* cb = nullptr;
    {
        const juce::ScopedLock sl (callbackLock);
        std::swap (cb, callback);
    }
    if (cb != nullptr)
        cb->audioDeviceStopped();

    if (outHandle != nullptr) snd_pcm_drop (outHandle);
    if (inHandle  != nullptr) snd_pcm_drop (inHandle);
    if (outHandle != nullptr) snd_pcm_prepare (outHandle);
    if (inHandle  != nullptr) snd_pcm_prepare (inHandle);
}

// ----- Recovery --------------------------------------------------------------
int AlsaAudioIODevice::recoverFromXrun (snd_pcm_t* handle, int err)
{
    xrunCount.fetch_add (1, std::memory_order_relaxed);
    const int recovered = snd_pcm_recover (handle, err, /*silent*/ 1);
    return recovered;
}

// ----- Interleave / deinterleave dispatch ------------------------------------
void AlsaAudioIODevice::interleavePlaybackBlock (const juce::AudioBuffer<float>& src,
                                                   void* dest, int numFrames) const
{
    const int n      = (int) outNumChannels;
    const int active = activeOutDeviceChannelIndex.size();
    const int* idx   = activeOutDeviceChannelIndex.getRawDataPointer();

    if (openedIsFloat)
        writeInterleavedFloat (src, dest, numFrames, n, idx, active);
    else if (bytesPerOutSample == 4 && openedBitDepth == 32)
        writeInterleavedS32 (src, dest, numFrames, n, idx, active);
    else if (bytesPerOutSample == 4 && openedBitDepth == 24)
        writeInterleavedS24in32 (src, dest, numFrames, n, idx, active);
    else if (bytesPerOutSample == 3)
        writeInterleavedS24Packed (src, dest, numFrames, n, idx, active);
    else if (bytesPerOutSample == 2)
        writeInterleavedS16 (src, dest, numFrames, n, idx, active);
}

void AlsaAudioIODevice::deinterleaveCaptureBlock (const void* src,
                                                    juce::AudioBuffer<float>& dest, int numFrames) const
{
    const int n      = (int) inNumChannels;
    const int active = activeInDeviceChannelIndex.size();
    const int* idx   = activeInDeviceChannelIndex.getRawDataPointer();

    if (openedIsFloat)
        readInterleavedFloat (src, dest, numFrames, n, idx, active);
    else if (bytesPerInSample == 4 && openedBitDepth == 32)
        readInterleavedS32 (src, dest, numFrames, n, idx, active);
    else if (bytesPerInSample == 4 && openedBitDepth == 24)
        readInterleavedS24in32 (src, dest, numFrames, n, idx, active);
    else if (bytesPerInSample == 3)
        readInterleavedS24Packed (src, dest, numFrames, n, idx, active);
    else if (bytesPerInSample == 2)
        readInterleavedS16 (src, dest, numFrames, n, idx, active);
}

// ----- I/O thread ------------------------------------------------------------
void AlsaAudioIODevice::run()
{
    while (! threadShouldExit())
    {
        // Capture path: read one period (blocking with a long timeout). If
        // there's no input device, the playback path drives the loop on its
        // own via writei, which blocks until the kernel has space.
        if (inHandle != nullptr)
        {
            const int avail = snd_pcm_wait (inHandle, 1000);
            if (avail < 0)
            {
                if (recoverFromXrun (inHandle, avail) < 0) break;
                continue;
            }
            if (threadShouldExit()) break;

            snd_pcm_sframes_t got = snd_pcm_readi (inHandle,
                                                     interleavedInBytes.getData(),
                                                     (snd_pcm_uframes_t) periodSize);
            if (got < 0)
            {
                if (recoverFromXrun (inHandle, (int) got) < 0) break;
                continue;
            }
            if (got < periodSize)
            {
                // Partial read: zero the rest and proceed; better than a stall.
                std::memset ((char*) interleavedInBytes.getData() + got * bytesPerInSample * (int) inNumChannels,
                              0,
                              (size_t) ((periodSize - got) * bytesPerInSample * (int) inNumChannels));
            }

            deinterleaveCaptureBlock (interleavedInBytes.getData(),
                                        callbackInFloats, periodSize);
        }

        callbackOutFloats.clear();

        {
            const juce::ScopedLock sl (callbackLock);
            if (callback != nullptr)
            {
                callback->audioDeviceIOCallbackWithContext (
                    callbackInPointers.getRawDataPointer(),
                    callbackInPointers.size(),
                    callbackOutPointers.getRawDataPointer(),
                    callbackOutPointers.size(),
                    periodSize,
                    {});
            }
        }

        if (outHandle != nullptr)
        {
            // Output-only path: nothing waited for us above, so wait for
            // playback ring-buffer space here. With duplex (snd_pcm_link), the
            // capture wait already returned exactly when playback had room
            // for one period, so this is a fast no-op.
            if (inHandle == nullptr)
            {
                const int avail = snd_pcm_wait (outHandle, 1000);
                if (avail < 0)
                {
                    if (recoverFromXrun (outHandle, avail) < 0) break;
                    continue;
                }
                if (threadShouldExit()) break;
            }

            interleavePlaybackBlock (callbackOutFloats,
                                       interleavedOutBytes.getData(), periodSize);

            int    framesRemaining = periodSize;
            char*  cursor          = (char*) interleavedOutBytes.getData();
            const int frameBytes   = bytesPerOutSample * (int) outNumChannels;

            while (framesRemaining > 0 && ! threadShouldExit())
            {
                snd_pcm_sframes_t wrote = snd_pcm_writei (outHandle, cursor,
                                                            (snd_pcm_uframes_t) framesRemaining);
                if (wrote < 0)
                {
                    if (recoverFromXrun (outHandle, (int) wrote) < 0) goto loopExit;
                    continue;
                }
                cursor          += wrote * frameBytes;
                framesRemaining -= (int) wrote;
            }
        }
    }
loopExit: ;
}
} // namespace focal
