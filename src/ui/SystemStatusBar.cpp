#include "SystemStatusBar.h"

namespace adhdaw
{
SystemStatusBar::SystemStatusBar (AudioEngine& e) : engine (e)
{
    setOpaque (true);
    startTimerHz (4);
    timerCallback();
}

SystemStatusBar::~SystemStatusBar() = default;

void SystemStatusBar::timerCallback()
{
    const double sr = engine.getCurrentSampleRate();
    const int    bs = engine.getCurrentBlockSize();
    const double cpu = engine.getDeviceManager().getCpuUsage();
    const int    engineXruns  = engine.getXRunCount();
    const int    backendXruns = engine.getBackendXRunCount();

    if (sr > 0.0 && bs > 0)
    {
        const double latencyMs = 1000.0 * (double) bs / sr;
        const double srKhz = sr / 1000.0;
        // 44.1 kHz / 88.2 kHz get a decimal; 48/96 stay as integers
        const auto srStr = (std::abs (srKhz - std::round (srKhz)) < 0.05)
                            ? juce::String ((int) std::round (srKhz))
                            : juce::String (srKhz, 1);
        audioInfo = "Audio: " + srStr + " kHz  " + juce::String (latencyMs, 1) + " ms";
    }
    else
    {
        audioInfo = "Audio: -";
    }

    // Override with a loud warning when the open device has 0 outputs -
    // engine is silent in that state, and we'd rather the user see this than
    // chase a "broken" engine. Replaces the rate/latency string entirely so
    // the warning isn't lost next to normal-looking telemetry.
    if (! engine.hasUsableOutputs() && sr > 0.0)
        audioInfo = "Audio: NO OUTPUTS - pick another device";

    // Two xrun counts: engine-self-detected (callback overrun) / backend
    // (driver EPIPE on ALSA, JACK xrun callback). They have different fixes
    // - surfacing both lets a glitchy session be diagnosed at a glance.
    dspInfo = "DSP: " + juce::String ((int) std::round (cpu * 100.0)) + "%"
            + " (" + juce::String (engineXruns) + "/" + juce::String (backendXruns) + ")";

    repaint();
}

void SystemStatusBar::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff141418));
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawRect (getLocalBounds(), 1);

    auto bounds = getLocalBounds().reduced (8, 0);
    g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                12.0f, juce::Font::plain)));

    // Color the DSP segment red when CPU is high or either xrun counter
    // moved off zero (engine-side OR backend-side).
    const double cpu   = engine.getDeviceManager().getCpuUsage();
    const int    xruns = engine.getXRunCount() + engine.getBackendXRunCount();
    const bool   warn  = cpu > 0.85 || xruns > 0;

    // DSP info now reads "DSP: 12% (3/0)" at worst - engine/backend xrun
    // pair widens the right column slightly.
    auto dspBounds = bounds.removeFromRight (140);
    bounds.removeFromRight (8);  // small gap

    // Audio segment goes red when the device opened with no outputs -
    // matches the "NO OUTPUTS" text override applied in timerCallback.
    const bool audioWarn = ! engine.hasUsableOutputs() && engine.getCurrentSampleRate() > 0.0;
    g.setColour (audioWarn ? juce::Colour (0xffe05050) : juce::Colour (0xffb0b0b8));
    g.drawText (audioInfo, bounds, juce::Justification::centredLeft, false);

    g.setColour (warn ? juce::Colour (0xffe05050) : juce::Colour (0xffb0b0b8));
    g.drawText (dspInfo, dspBounds, juce::Justification::centredRight, false);
}
} // namespace adhdaw
