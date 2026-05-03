#include "MainComponent.h"

namespace adhdaw
{
MainComponent::MainComponent()
{
    addAndMakeVisible (audioSettingsButton);
    audioSettingsButton.onClick = [this] { openAudioSettings(); };

    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setText ("ADH DAW — Phase 1a bootstrap. Open audio settings to route input/output.",
                         juce::dontSendNotification);
    addAndMakeVisible (statusLabel);

    setSize (1280, 720);
}

MainComponent::~MainComponent() = default;

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (16);
    audioSettingsButton.setBounds (area.removeFromTop (32).removeFromLeft (200));
    area.removeFromTop (8);
    statusLabel.setBounds (area.removeFromTop (24));
}

void MainComponent::openAudioSettings()
{
    auto& dm = engine.getDeviceManager();
    auto* selector = new juce::AudioDeviceSelectorComponent (
        dm,
        /*minIn*/  0, /*maxIn*/  16,
        /*minOut*/ 2, /*maxOut*/ 2,
        /*showMidi*/ false, /*showMidiOut*/ false,
        /*stereoPairs*/ false, /*hideAdvanced*/ false);
    selector->setSize (600, 480);

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (selector);
    opts.dialogTitle = "Audio device";
    opts.dialogBackgroundColour = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
}
} // namespace adhdaw
