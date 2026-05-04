#include "MainComponent.h"
#include "SystemStatusBar.h"
#include "TapeStrip.h"
#include "TransportBar.h"
#include "../session/SessionSerializer.h"

namespace adhdaw
{
MainComponent::MainComponent()
{
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // Default to a session under ~/Music/ADHDaw/Untitled. The user can change
    // this later via a session-management UI; for the recorder MVP this is
    // enough to get WAVs on disk.
    auto musicDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    if (! musicDir.exists()) musicDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    session.setSessionDirectory (musicDir.getChildFile ("ADHDaw").getChildFile ("Untitled"));

    addAndMakeVisible (audioSettingsButton);
    audioSettingsButton.onClick = [this] { openAudioSettings(); };

    auto styleHeaderButton = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff202024));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d0));
    };
    styleHeaderButton (saveButton);
    styleHeaderButton (loadButton);
    saveButton.onClick = [this]
    {
        const auto target = session.getSessionDirectory().getChildFile ("session.json");
        if (SessionSerializer::save (session, target))
            statusLabel.setText ("Saved: " + target.getFullPathName(), juce::dontSendNotification);
        else
            statusLabel.setText ("Save failed at " + target.getFullPathName(), juce::dontSendNotification);
    };
    loadButton.onClick = [this]
    {
        const auto src = session.getSessionDirectory().getChildFile ("session.json");
        if (SessionSerializer::load (session, src))
        {
            // Reconstruct the console so all controls reflect the freshly
            // loaded values (atomic state is in `session`, but UI widgets
            // captured initial values in their constructors).
            consoleView.reset();
            consoleView = std::make_unique<ConsoleView> (session);
            addAndMakeVisible (consoleView.get());
            resized();
            statusLabel.setText ("Loaded: " + src.getFullPathName(), juce::dontSendNotification);
        }
        else
        {
            statusLabel.setText ("No session at " + src.getFullPathName(), juce::dontSendNotification);
        }
    };
    addAndMakeVisible (saveButton);
    addAndMakeVisible (loadButton);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setText (juce::CharPointer_UTF8 (
                             "ADH DAW \xe2\x80\x94 Phase 1a (mixer, no recording, no plugin hosting). "
                             "Faders / pan / mute / solo / sends / HPF / EQ / Comp are live."),
                         juce::dontSendNotification);
    addAndMakeVisible (statusLabel);

    systemStatusBar = std::make_unique<SystemStatusBar> (engine);
    addAndMakeVisible (systemStatusBar.get());

    transportBar = std::make_unique<TransportBar> (engine);
    transportBar->onTapeStripToggle = [this] (bool expanded)
    {
        tapeStripExpanded = expanded;
        if (tapeStrip != nullptr) tapeStrip->setVisible (expanded);
        resized();
    };
    addAndMakeVisible (transportBar.get());

    tapeStrip = std::make_unique<TapeStrip> (session, engine);
    tapeStrip->setVisible (tapeStripExpanded);
    addAndMakeVisible (tapeStrip.get());

    // Sync the transport-bar TAPE toggle with the collapsed default.
    if (transportBar != nullptr)
        transportBar->setTapeStripExpanded (tapeStripExpanded);

    // Best-effort auto-load: if there's a session.json in the default session
    // directory from a previous run, restore everything before we build the UI.
    {
        const auto target = session.getSessionDirectory().getChildFile ("session.json");
        SessionSerializer::load (session, target);
    }

    consoleView = std::make_unique<ConsoleView> (session);
    addAndMakeVisible (consoleView.get());

    // Auto-size to screen: prefer the reference size (1440x1280) but shrink to
    // fit smaller displays. The ConsoleView itself reflows responsively.
    const int kPreferredW = 1440;
    const int kPreferredH = 1280;
    const int kTopBarH    = 32 + 8 + 16;  // settings button + status row + outer padding

    int w = kPreferredW;
    int h = kPreferredH;
    if (auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto userArea = primary->userArea;
        w = juce::jmin (kPreferredW, userArea.getWidth()  - 24);
        h = juce::jmin (kPreferredH, userArea.getHeight() - 24);
    }

    const int minContentW = ConsoleView::minimumContentWidth() + 16;  // + outer padding
    const int minContentH = 480 + kTopBarH;
    w = juce::jmax (w, minContentW);
    h = juce::jmax (h, minContentH);

    setSize (w, h);
}

MainComponent::~MainComponent()
{
    // Auto-save on close so the next launch picks up where we left off.
    SessionSerializer::save (session,
                              session.getSessionDirectory().getChildFile ("session.json"));
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0d0d0f));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (8);
    auto top = area.removeFromTop (28);
    audioSettingsButton.setBounds (top.removeFromLeft (160));
    top.removeFromLeft (8);
    saveButton.setBounds (top.removeFromLeft (60));
    top.removeFromLeft (4);
    loadButton.setBounds (top.removeFromLeft (60));
    top.removeFromLeft (12);
    if (systemStatusBar != nullptr)
        systemStatusBar->setBounds (top.removeFromRight (300));
    top.removeFromRight (8);
    statusLabel.setBounds (top);
    area.removeFromTop (4);

    if (transportBar != nullptr)
        transportBar->setBounds (area.removeFromTop (44));
    area.removeFromTop (4);

    if (tapeStrip != nullptr && tapeStripExpanded)
    {
        tapeStrip->setBounds (area.removeFromTop (TapeStrip::naturalHeight()));
        area.removeFromTop (4);
    }

    consoleView->setBounds (area);
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
    opts.dialogBackgroundColour = juce::Colour (0xff202024);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
}
} // namespace adhdaw
