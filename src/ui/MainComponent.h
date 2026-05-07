#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "FocalLookAndFeel.h"
#include "ConsoleView.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace focal
{
class MainComponent final : public juce::Component,
                             public juce::MenuBarModel,
                             private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // MenuBarModel overrides - drive the File / Settings menus at the top
    // of the window. Replaces the previous row of large TextButtons.
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex (int topLevelMenuIndex,
                                         const juce::String& menuName) override;
    void              menuItemSelected (int menuItemID,
                                          int topLevelMenuIndex) override;

    // Public entry-point used by MainWindow's close-confirm dialog. Saves
    // to the current sessionDir if a session.json already exists there;
    // otherwise opens the Save As file chooser. The optional `onComplete`
    // callback runs on the message thread once the save is committed -
    // synchronously when no chooser is needed, asynchronously after the
    // chooser dismisses. The bool argument is true on success.
    void saveSessionAndThen (std::function<void(bool)> onComplete);

private:
    void openAudioSettings();
    void openBounceDialog();
    void launchStartupDialog();
    void switchToStage (AudioEngine::Stage);
    void doMixdown();

    // Session-management helpers shared by the header buttons and the
    // startup dialog. All run on the message thread.
    bool saveSessionTo (const juce::File& sessionDir);   // writes session.json, returns true on success
    void saveAsPrompt();                                 // 2-step: name + parent dir
    bool loadSessionFromJson (const juce::File& sessionJson);
    // Synchronous tail-half of loadSessionFromJson. Called either directly
    // (no autosave present) or from the autosave recovery prompt's callback
    // once the user has picked which file to load from.
    bool finishLoadingSessionFrom (const juce::File& sessionJson,
                                    const juce::File& sessionDir);
    void openFromFilePrompt();                           // file picker for session.json
    void newSessionPrompt();                             // dir picker + setSessionDirectory + immediate save

    // Autosave: a juce::Timer fires every 30s and writes a session.json.autosave
    // sibling using the same atomic temp+rename pattern as the manual save. On
    // session load (loadSessionFromJson) we check whether the autosave is newer
    // than session.json - if so, prompt the user to recover. Manual saves
    // delete the autosave so the prompt doesn't fire on the next clean load.
    void timerCallback() override;
    void writeAutosave();
    juce::File getAutosaveFileFor (const juce::File& sessionDir) const;
    // Returns true if session.json.autosave exists and is newer than session.json.
    // Caller drives the user-facing prompt.
    bool autosaveIsNewerThan (const juce::File& sessionJson) const;
    void deleteAutosaveFor   (const juce::File& sessionDir) const;
    static constexpr int kAutosaveIntervalMs = 30000;

    Session session;
    AudioEngine engine { session };

    FocalLookAndFeel lookAndFeel;

    // Menu bar at the very top. Replaces the prior row of TextButtons
    // (Audio settings... / Save / Save As... / Open... / Mixdown / Bounce...)
    // - the menu bar is much slimmer and reads as a normal app menu.
    juce::MenuBarComponent menuBar;

    // Stage selector - segmented control. Drives both engine.setStage() and
    // which view the body shows.
    juce::TextButton recordingStageBtn { "RECORDING" };
    juce::TextButton mixingStageBtn    { "MIXING" };
    juce::TextButton auxStageBtn       { "AUX" };
    juce::TextButton masteringStageBtn { "MASTERING" };

    // Bank A / B buttons. Used when the window is too narrow to show all
    // 16 channel strips at once - we show 8 at a time and the user toggles
    // between bank A (1-8) and bank B (9-16). Lives here in MainComponent
    // (rather than inside ConsoleView) so the row sits directly below the
    // stage selector and the freed vertical space inside the console all
    // goes to the channel strips' fader sections.
    juce::TextButton bankAButton { "BANK A  (1-8)"  };
    juce::TextButton bankBButton { "BANK B  (9-16)" };

    std::unique_ptr<juce::FileChooser> bounceFileChooser;
    std::unique_ptr<juce::FileChooser> sessionFileChooser;
    std::unique_ptr<class MasteringView> masteringView;
    std::unique_ptr<class AuxView>       auxView;

    // Track the audio settings DialogWindow so we can explicitly delete it
    // in our destructor BEFORE AudioEngine destructs. Required because the
    // dialog hosts an AudioDeviceSelectorComponent that's a change-listener
    // on engine.deviceManager - if we let JUCE's ModalComponentManager clean
    // it up at app exit (via ScopedJuceInitialiser_GUI's destructor, which
    // runs AFTER us), the listener removal would dereference a freed
    // AudioDeviceManager → SIGSEGV.
    juce::Component::SafePointer<juce::DialogWindow> activeAudioDialog;
    juce::Label statusLabel;
    std::unique_ptr<ConsoleView> consoleView;
    std::unique_ptr<class TransportBar>     transportBar;
    std::unique_ptr<class TapeStrip>        tapeStrip;
    std::unique_ptr<class SystemStatusBar>  systemStatusBar;
    bool tapeStripExpanded = false;  // collapsed by default; user expands when arranging
};
} // namespace focal
