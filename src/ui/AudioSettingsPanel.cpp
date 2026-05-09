#include "AudioSettingsPanel.h"
#include "AppConfig.h"
#include "SelfTestPanel.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"
#if defined(__linux__)
 #include "../engine/alsa/AlsaAudioIODevice.h"
#endif

namespace focal
{
AudioSettingsPanel::AudioSettingsPanel (juce::AudioDeviceManager& dm,
                                          AudioEngine& e, Session& s)
    : deviceManager (dm), engine (e), session (s)
{
    selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        dm,
        /*minIn*/  0, /*maxIn*/  16,
        /*minOut*/ 2, /*maxOut*/ 2,
        /*showMidi*/ false, /*showMidiOut*/ false,
        /*stereoPairs*/ false, /*hideAdvanced*/ false);
    addAndMakeVisible (*selector);

#if defined(__linux__)
    addAndMakeVisible (periodsLabel);
    periodsLabel.setJustificationType (juce::Justification::centredRight);

    // Sensible USB-audio range. 2 is the minimum that gives the kernel any
    // slack at all; 4 is the JUCE default and what most DAWs use; 8/16 add
    // latency but give the kernel more headroom against scheduler jitter.
    for (int p : { 2, 3, 4, 8, 16 })
        periodsCombo.addItem (juce::String (p), p);
    {
        // getRequestedPeriods() is clamped to [2,16] but the combo only exposes
        // a discrete subset; fall back to 4 (JUCE default) for any value that
        // doesn't have a corresponding item, otherwise the combo renders blank.
        const int requested = AlsaAudioIODevice::getRequestedPeriods();
        const bool inSet = (requested == 2 || requested == 3 || requested == 4
                            || requested == 8 || requested == 16);
        periodsCombo.setSelectedId (inSet ? requested : 4, juce::dontSendNotification);
    }
    periodsCombo.setTooltip ("ALSA period count. Only applies to ALSA backend. "
                              "Increase if you hear xruns or distortion at low "
                              "buffer sizes; decrease for lower latency.");
    periodsCombo.onChange = [this] { applyPeriodsChange(); };
    addAndMakeVisible (periodsCombo);
#endif

    selfTestButton.onClick = [this] { openSelfTest(); };
    selfTestButton.setTooltip (juce::CharPointer_UTF8 (
        "Open the headless audio pipeline self-test panel "
        "- synthetic engine tests + backend cycle."));
    addAndMakeVisible (selfTestButton);

    rescanButton.onClick = [this] { applyRescan(); };
    rescanButton.setTooltip ("Re-enumerate audio backends and devices. "
                              "Use after plugging in or removing a USB / "
                              "Thunderbolt audio interface.");
    addAndMakeVisible (rescanButton);

    // Effect oversampling - global. ComboBox IDs are the literal factor (1,
    // 2, 4) so we read the value back without an extra mapping table.
    // CharPointer_UTF8 wrappers are required because the "×" multiplication
    // sign (U+00D7) is two-byte UTF-8; without the explicit ctor JUCE's
    // juce::String defaults to Latin-1 and renders mojibake ("Ã-").
    addAndMakeVisible (oversamplingLabel);
    oversamplingLabel.setJustificationType (juce::Justification::centredRight);
    oversamplingCombo.addItem (juce::CharPointer_UTF8 ("1× (native)"), 1);
    oversamplingCombo.addItem (juce::CharPointer_UTF8 ("2×"),          2);
    oversamplingCombo.addItem (juce::CharPointer_UTF8 ("4×"),          4);
    {
        const int current = juce::jlimit (1, 4, session.oversamplingFactor.load (std::memory_order_relaxed));
        oversamplingCombo.setSelectedId ((current == 2 || current == 4) ? current : 1,
                                          juce::dontSendNotification);
    }
    oversamplingCombo.setTooltip (juce::CharPointer_UTF8 (
        "Global effect oversampling. 1× is native rate "
        "(lowest CPU). 2× / 4× engage internal "
        "oversampling on the master + aux bus comps and "
        "the master tape saturation. Per-channel comp "
        "and EQ stay at native rate regardless."));
    oversamplingCombo.onChange = [this] { applyOversamplingChange(); };
    addAndMakeVisible (oversamplingCombo);

    // UI scale - user override on top of JUCE's per-display DPI. Lives
    // in app config (per-machine), separate from session state.
    uiScaleLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (uiScaleLabel);

    uiScaleSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    uiScaleSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    uiScaleSlider.setRange (appconfig::kUiScaleMin, appconfig::kUiScaleMax, 0.05);
    uiScaleSlider.setValue (appconfig::getUiScaleOverride(), juce::dontSendNotification);
    uiScaleSlider.setNumDecimalPlacesToDisplay (2);
    uiScaleSlider.setTextValueSuffix (juce::CharPointer_UTF8 ("×"));
    uiScaleSlider.setTooltip (juce::CharPointer_UTF8 (
        "Multiplier applied on top of the OS-reported "
        "display DPI. 1.00× = follow the OS. Range "
        "0.50× – 2.00×."));

    // Defer the actual setGlobalScaleFactor call until the user RELEASES
    // the slider (or commits a typed value). Applying mid-drag re-lays out
    // the slider itself, which makes the drag handle chase the mouse -
    // very jumpy. The slider's own textbox still updates live so the user
    // sees the target value during drag; only the world reflows on release.
    uiScaleSlider.onDragStart = [this] { uiScaleDragging = true; };
    uiScaleSlider.onDragEnd   = [this]
    {
        uiScaleDragging = false;
        applyUiScaleChange();
    };
    uiScaleSlider.onValueChange = [this]
    {
        if (! uiScaleDragging) applyUiScaleChange();
    };
    addAndMakeVisible (uiScaleSlider);

    uiScaleHint.setJustificationType (juce::Justification::centredLeft);
    uiScaleHint.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    uiScaleHint.setFont (juce::Font (juce::FontOptions (10.0f)));
    uiScaleHint.setText ("Saved per-machine; takes effect immediately.",
                          juce::dontSendNotification);
    addAndMakeVisible (uiScaleHint);
}

void AudioSettingsPanel::resized()
{
    auto area = getLocalBounds();

    // Bottom row: Periods + Oversampling + Rescan + Self-Test buttons.
    auto bottom = area.removeFromBottom (32);
#if defined(__linux__)
    periodsLabel.setBounds       (bottom.removeFromLeft (180).reduced (4, 4));
    periodsCombo.setBounds       (bottom.removeFromLeft (100).reduced (4, 4));
#endif
    oversamplingLabel.setBounds  (bottom.removeFromLeft (160).reduced (4, 4));
    oversamplingCombo.setBounds  (bottom.removeFromLeft (120).reduced (4, 4));
    selfTestButton.setBounds     (bottom.removeFromRight (160).reduced (4, 4));
    rescanButton.setBounds       (bottom.removeFromRight (140).reduced (4, 4));

    // Row above: UI scale slider + hint.
    auto scaleRow = area.removeFromBottom (28);
    uiScaleLabel.setBounds  (scaleRow.removeFromLeft (180).reduced (4, 2));
    uiScaleSlider.setBounds (scaleRow.removeFromLeft (260).reduced (4, 2));
    uiScaleHint.setBounds   (scaleRow.reduced (4, 2));

    selector->setBounds (area);
}

void AudioSettingsPanel::openSelfTest()
{
    auto* panel = new SelfTestPanel (engine, deviceManager, session);
    panel->setSize (760, 560);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (panel);
    opts.dialogTitle = "Audio Pipeline Self-Test";
    opts.dialogBackgroundColour = juce::Colour (0xff202024);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    if (auto* dw = opts.launchAsync()) dw->toFront (true);
}

void AudioSettingsPanel::applyUiScaleChange()
{
    const float scale = (float) uiScaleSlider.getValue();
    appconfig::setUiScaleOverride (scale);
    juce::Desktop::getInstance().setGlobalScaleFactor (scale);
}

void AudioSettingsPanel::applyRescan()
{
    // Re-enumerate every registered audio backend. AudioIODeviceType's
    // scanForDevices() repopulates the type's internal device list; the
    // listener notification path (fired by callDeviceChangeListeners() on
    // our ALSA type, and by JUCE's own backends on theirs) tells the
    // AudioDeviceSelectorComponent to re-query and rebuild its dropdowns.
    //
    // We iterate every type rather than only the current one so that
    // switching backend (e.g. ALSA -> JACK) after a hot-plug still sees
    // the freshly-enumerated devices on the new backend.
    const auto& types = deviceManager.getAvailableDeviceTypes();
    for (auto* type : types)
        if (type != nullptr)
            type->scanForDevices();

    // The selector subscribes to AudioDeviceManager change broadcasts. Most
    // backends' scanForDevices() will already fire callDeviceChangeListeners()
    // (which routes through audioDeviceListChanged() → sendChangeMessage()),
    // but some only broadcast on a real diff. Force a refresh either way.
    // Re-applying the same setup via setAudioDeviceSetup is a no-op when
    // newSetup == currentSetup (JUCE early-returns without notifying), so
    // poke the broadcaster directly.
    deviceManager.sendChangeMessage();

    // Same rescan also re-enumerates MIDI inputs so freshly plugged-in
    // controllers appear in the per-strip MIDI dropdowns. The engine
    // detaches its callbacks for the duration so audio briefly drops
    // out, which is acceptable for a user-triggered rescan; tracks
    // re-resolve their saved device identifiers to current indices on
    // completion.
    engine.refreshMidiInputs();
}

#if defined(__linux__)
void AudioSettingsPanel::applyPeriodsChange()
{
    const int p = periodsCombo.getSelectedId();
    if (p <= 0) return;

    AlsaAudioIODevice::setRequestedPeriods (p);

    // Re-open the device with the same setup so setParameters() runs and
    // picks up the new period count. Without this, the change only takes
    // effect on the next manual device switch.
    auto setup = deviceManager.getAudioDeviceSetup();
    deviceManager.setAudioDeviceSetup (setup, /*treatAsChosenDevice*/ true);
}
#endif

void AudioSettingsPanel::applyOversamplingChange()
{
    const int factor = oversamplingCombo.getSelectedId();
    if (factor != 1 && factor != 2 && factor != 4) return;

    session.oversamplingFactor.store (factor, std::memory_order_relaxed);

    // Re-prepare engine DSP so the new factor takes effect on the next
    // callback. Bouncing setAudioDeviceSetup forces audioDeviceAboutToStart
    // → AudioEngine::prepareForSelfTest → master/aux prepare(...,factor),
    // which is the cheapest way to apply the change without restarting.
    auto setup = deviceManager.getAudioDeviceSetup();
    deviceManager.setAudioDeviceSetup (setup, /*treatAsChosenDevice*/ true);
}
} // namespace focal
