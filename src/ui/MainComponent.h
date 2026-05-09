#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "EmbeddedModal.h"
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

    // App-quit gate. Called from the window's close button. If there are
    // unsaved changes since the last manual save (autosave file is newer
    // than session.json), shows a Focal-styled modal with Save / Don't
    // Save / Cancel; otherwise quits immediately. Industry-standard
    // dirty-only prompt - matches Logic / Pro Tools / Bitwig.
    void requestQuit();

    // Staged shutdown that quiesces the engine and tears down native
    // window peers in an order the host windowing system can keep up
    // with. Without this sequencing the destroy-notify storm of a
    // hard quit can race the compositor / window manager and on
    // Linux/Wayland (Mutter) has been observed to take down the
    // whole desktop session.
    //
    // Order: stop autosave timer, stop transport (drains in-flight
    // record buffers + commits regions), detach the audio callback
    // (audio thread stops calling processBlock on plugins about to
    // be torn down), drop every plugin editor window, sync the
    // windowing system, hide the main window, sync again, then
    // post the quit message.
    void beginSafeShutdown();

private:
    void openAudioSettings();
    void openBounceDialog();
    void cleanOutUnreferencedFiles();
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
    EmbeddedModal audioSettingsModal;
    EmbeddedModal mixdownModal;
    EmbeddedModal bounceModal;
    EmbeddedModal quitModal;
    EmbeddedModal virtualKeyboardModal;
    void toggleVirtualKeyboard();

    // True once the audio callback has been removed in preparation for
    // shutdown. Used by saveSessionTo / beginSafeShutdown to make the
    // detach call idempotent and to signal publishPluginStateForSave
    // that the atomic-park sleeps can be skipped (no audio thread to
    // race). Reset is unnecessary - the only path that sets this also
    // ends in systemRequestedQuit.
    bool engineDetached = false;
    juce::Label statusLabel;
    std::unique_ptr<ConsoleView> consoleView;
    std::unique_ptr<class TransportBar>     transportBar;
    std::unique_ptr<class TapeStrip>        tapeStrip;
    std::unique_ptr<class SystemStatusBar>  systemStatusBar;
    bool tapeStripExpanded = false;  // collapsed by default; user expands when arranging

    // Startup dialog now lives as an embedded modal (no separate window) so
    // it appears centered on the main UI with a dim backdrop. Both pieces
    // are owned here and torn down together via dismissStartupDialog.
    std::unique_ptr<class StartupDialog> startupDialog;
    std::unique_ptr<class DimOverlay>    startupDim;
    void dismissStartupDialog();

    // Piano-roll overlay - constructed on demand when the user clicks a
    // MidiRegion in the tape strip; dismissed by clicking the dim backdrop
    // or pressing Esc. The roll is the single visible exception to "no
    // tabs / no hidden panels" per Focal.md (the spec calls it out).
    // Tracks which region is currently open so the tape-strip click handler
    // can toggle (same region) vs swap (different region).
    std::unique_ptr<class DimOverlay>          pianoRollDim;
    std::unique_ptr<class PianoRollComponent>  pianoRoll;
    int pianoRollTrackIdx  = -1;
    int pianoRollRegionIdx = -1;
    void openPianoRoll  (int trackIdx, int regionIdx);
    void closePianoRoll();

    // Tuner overlay - same modal pattern as the piano roll. While open,
    // a 30 Hz timer polls Session::tuneLatestHz / tuneLatestLevel into
    // the overlay's setDetected(). The overlay's onDismiss closes the
    // modal AND clears Session::tuneTrackIndex so the audio thread stops
    // running the detector when nobody's looking.
    std::unique_ptr<class DimOverlay>    tunerDim;
    std::unique_ptr<class TunerOverlay>  tuner;
    std::unique_ptr<class juce::Timer>   tunerPoller;
    void toggleTuner();
    void closeTuner();
};
} // namespace focal
