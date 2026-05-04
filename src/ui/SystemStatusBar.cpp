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
    const int    xruns = engine.getXRunCount();

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
        audioInfo = "Audio: —";
    }

    dspInfo = "DSP: " + juce::String ((int) std::round (cpu * 100.0)) + "%"
            + " (" + juce::String (xruns) + ")";

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

    // Color the DSP segment red when CPU is high or xruns have happened.
    const double cpu   = engine.getDeviceManager().getCpuUsage();
    const int    xruns = engine.getXRunCount();
    const bool   warn  = cpu > 0.85 || xruns > 0;

    // DSP info is short ("DSP: 100% (99)" worst case ≈ 100 px) — give it
    // exactly what it needs on the right and let audioInfo use the rest of
    // the bar so "ms" doesn't get clipped at typical status-bar widths.
    auto dspBounds = bounds.removeFromRight (110);
    bounds.removeFromRight (8);  // small gap

    g.setColour (juce::Colour (0xffb0b0b8));
    g.drawText (audioInfo, bounds, juce::Justification::centredLeft, false);

    g.setColour (warn ? juce::Colour (0xffe05050) : juce::Colour (0xffb0b0b8));
    g.drawText (dspInfo, dspBounds, juce::Justification::centredRight, false);
}
} // namespace adhdaw
