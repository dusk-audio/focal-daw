#include "MainComponent.h"
#include "AudioSettingsPanel.h"
#include "AuxView.h"
#include "BounceDialog.h"
#include "DimOverlay.h"
#include "PianoRollComponent.h"
#include "TunerOverlay.h"
#include "../session/SessionTemplates.h"
#include "MasteringView.h"
#include "StartupDialog.h"
#include "SystemStatusBar.h"
#include "TapeStrip.h"
#include "TransportBar.h"
#include "../session/MarkerEditActions.h"
#include "../session/RecentSessions.h"
#include "../session/SessionSerializer.h"
#include "ConsoleView.h"  // (already included transitively, kept explicit)

namespace focal
{
MainComponent::MainComponent()
{
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // Default to a session under ~/Music/Focal/Untitled. The user can change
    // this later via a session-management UI; for the recorder MVP this is
    // enough to get WAVs on disk.
    auto musicDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    if (! musicDir.exists()) musicDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    session.setSessionDirectory (musicDir.getChildFile ("Focal").getChildFile ("Untitled"));

    // Top-of-window menu bar drives File / Settings actions. Replaces the
    // old row of TextButtons (Audio settings... / Save / Save As... / etc).
    menuBar.setModel (this);
    addAndMakeVisible (menuBar);

    // Stage selector - segmented look. The active stage gets a brighter
    // fill so the user always knows where they are.
    auto styleStageButton = [] (juce::TextButton& b, juce::Colour onColour)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (1001);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, onColour);
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    };
    styleStageButton (recordingStageBtn, juce::Colour (0xffd03030));   // red, like REC
    styleStageButton (mixingStageBtn,    juce::Colour (0xff5a8ad0));   // mix-desk blue
    styleStageButton (auxStageBtn,       juce::Colour (0xff9080c0));   // aux purple-grey
    styleStageButton (masteringStageBtn, juce::Colour (0xff8a5ad0));   // mastering purple
    recordingStageBtn.setConnectedEdges (juce::Button::ConnectedOnRight);
    mixingStageBtn   .setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    auxStageBtn      .setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    masteringStageBtn.setConnectedEdges (juce::Button::ConnectedOnLeft);
    recordingStageBtn.onClick = [this] { switchToStage (AudioEngine::Stage::Recording); };
    mixingStageBtn   .onClick = [this] { switchToStage (AudioEngine::Stage::Mixing); };
    auxStageBtn      .onClick = [this] { switchToStage (AudioEngine::Stage::Aux); };
    masteringStageBtn.onClick = [this] { switchToStage (AudioEngine::Stage::Mastering); };
    addAndMakeVisible (recordingStageBtn);
    addAndMakeVisible (mixingStageBtn);
    addAndMakeVisible (auxStageBtn);
    addAndMakeVisible (masteringStageBtn);
    mixingStageBtn.setToggleState (true, juce::dontSendNotification);  // default

    // Bank A/B buttons (visible only when the window is too narrow to fit
    // all 16 strips). Sit on a row directly below the stage selector so
    // the channel strips inside ConsoleView get the full body height.
    auto styleBankBtn = [] (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (1002);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd0a060));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colours::lightgrey);
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    };
    styleBankBtn (bankAButton);
    styleBankBtn (bankBButton);
    bankAButton.setToggleState (true, juce::dontSendNotification);
    bankAButton.onClick = [this]
    {
        if (consoleView != nullptr) consoleView->setBank (0);
    };
    bankBButton.onClick = [this]
    {
        if (consoleView != nullptr) consoleView->setBank (1);
    };
    addChildComponent (bankAButton);   // visibility set per-resize based on window width
    addChildComponent (bankBButton);

    // Save / Save As / Open / Mixdown / Bounce / Audio settings now live in
    // the menu bar - see getMenuForIndex() and menuItemSelected() below.

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setText (juce::CharPointer_UTF8 (
                             "Focal \xe2\x80\x94 Phase 1a (mixer, no recording, no plugin hosting). "
                             "Faders / pan / mute / solo / sends / HPF / EQ / Comp are live."),
                         juce::dontSendNotification);
    addAndMakeVisible (statusLabel);

    systemStatusBar = std::make_unique<SystemStatusBar> (engine);
    addAndMakeVisible (systemStatusBar.get());

    transportBar = std::make_unique<TransportBar> (engine);
    transportBar->onTunerToggle = [this] { toggleTuner(); };
    transportBar->onTapeStripToggle = [this] (bool expanded)
    {
        tapeStripExpanded = expanded;
        if (tapeStrip != nullptr) tapeStrip->setVisible (expanded);
        // Collapse each track strip's EQ + COMP into popup buttons while the
        // SUMMARY view is up - without this the fader and bus assigns get
        // pushed off the bottom of the strip and become unusable.
        if (consoleView != nullptr) consoleView->setStripsCompactMode (expanded);
        resized();
    };
    addAndMakeVisible (transportBar.get());

    tapeStrip = std::make_unique<TapeStrip> (session, engine);
    tapeStrip->setVisible (tapeStripExpanded);
    tapeStrip->onMidiRegionClicked = [this] (int t, int r) { openPianoRoll (t, r); };
    addAndMakeVisible (tapeStrip.get());

    // Sync the transport-bar TAPE toggle with the collapsed default.
    if (transportBar != nullptr)
        transportBar->setTapeStripExpanded (tapeStripExpanded);

    // Intentionally NO auto-load on startup. Standard DAW behavior is to
    // start with a fresh session and require the user to explicitly Load. The
    // previous best-effort auto-load of ~/Music/Focal/Untitled/session.json
    // was confusing - settings (master fader, mutes, etc.) silently persisted
    // across launches. Use the Load button to restore a saved session.
    // TODO: replace with a startup dialog (New / Open Recent / Browse...) when
    // we add proper session management UX.

    consoleView = std::make_unique<ConsoleView> (session, engine);
    addAndMakeVisible (consoleView.get());
    consoleView->setOnStripFocusRequested ([this] (int t)
    {
        if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
    });

    // Initial stage is Mixing (mixingStageBtn defaults toggled on) - sync the
    // strips so they show aux sends instead of input/IN/ARM/PRINT from the
    // first paint. switchToStage() handles subsequent changes.
    consoleView->setStripsMixingMode (engine.getStage() == AudioEngine::Stage::Mixing);

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

    // Dev affordance for screenshot / xdotool flows: set FOCAL_START_STAGE=mastering
    // to land in Mastering at startup. No-op in normal use.
    if (auto envStage = std::getenv ("FOCAL_START_STAGE"))
    {
        const juce::String s (envStage);
        if (s.equalsIgnoreCase ("mastering"))
            switchToStage (AudioEngine::Stage::Mastering);
        else if (s.equalsIgnoreCase ("recording"))
            switchToStage (AudioEngine::Stage::Recording);
    }

    // Receive keyboard input - needed for Ctrl+Z / Ctrl+Y. Children
    // (text editors, sliders) still grab focus when interacted with;
    // this just ensures clicks on empty canvas land here.
    setWantsKeyboardFocus (true);

    // Defer the startup dialog to the next message-loop tick so the main
    // window paints first - otherwise the dialog can pop up over a blank
    // canvas. SafePointer guards the case where the user closes the app
    // before the message loop catches up to the queued lambda.
    // FOCAL_SKIP_STARTUP_DIALOG=1 bypasses the picker for screenshot /
    // xdotool flows.
    if (std::getenv ("FOCAL_SKIP_STARTUP_DIALOG") == nullptr)
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr) safeThis->launchStartupDialog();
        });
    }

    // Autosave heartbeat. Writes session.json.autosave next to the canonical
    // session.json every kAutosaveIntervalMs (30 s) so a crash loses at most
    // ~30 s of work. The write is atomic (temp + rename), so even a kill
    // during the timer fire never leaves a half-written autosave.
    startTimer (kAutosaveIntervalMs);
}

MainComponent::~MainComponent()
{
    stopTimer();   // halt autosave before tearing down engine / session

    // Force-delete any audio settings dialog we launched. JUCE's
    // ModalComponentManager keeps modal dialogs alive on its own stack and
    // would clean them up at app exit via ScopedJuceInitialiser_GUI's
    // destructor - but that runs AFTER MainComponent destructs (and
    // therefore AFTER our AudioEngine + AudioDeviceManager are gone). The
    // dialog's AudioDeviceSelectorComponent listens to AudioDeviceManager
    // and would crash on listener-removal in that delayed teardown.
    // Deleting it here, while AudioEngine is still alive, is safe.
    // Embedded modal teardown: closes the panel + dim while AudioEngine
    // is still alive, so AudioDeviceSelectorComponent's listener removal
    // happens before the engine and its DeviceManager go away.
    audioSettingsModal.close();
    mixdownModal      .close();
    bounceModal       .close();

    // Intentionally NO auto-save here. Standard DAW behavior is to require
    // an explicit Save before exit. The previous auto-save on destruct
    // paired with auto-load on construct caused settings (master fader
    // position, mutes, etc.) to silently persist across launches. Use the
    // Save button when you want to keep state.

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0d0d0f));
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    auto& um = engine.getUndoManager();
    const auto mods    = key.getModifiers();
    const int  code    = key.getKeyCode();
    const bool cmd     = mods.isCommandDown();   // Ctrl on Linux/Windows, Cmd on macOS
    const bool shift   = mods.isShiftDown();
    const bool noMods  = ! cmd && ! shift && ! mods.isAltDown();

    // ── Edit: Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z, Ctrl/Cmd+Y ──
    if (code == 'Z' && cmd && ! shift) { um.undo(); return true; }
    if ((code == 'Z' && cmd && shift) || (code == 'Y' && cmd))
    {
        um.redo();
        return true;
    }

    // ── Region clipboard: Ctrl/Cmd+C, Ctrl/Cmd+X, Ctrl/Cmd+V; Delete ──
    // Each routes through TapeStrip, which owns the selection state. They
    // no-op when nothing's selected (or for paste, when the clipboard's
    // empty), letting the keypress fall through to default handling.
    if (tapeStrip != nullptr)
    {
        if (code == 'C' && cmd && ! shift)
        {
            if (tapeStrip->copySelectedRegion()) return true;
        }
        if (code == 'X' && cmd && ! shift)
        {
            if (tapeStrip->cutSelectedRegion()) return true;
        }
        if (code == 'V' && cmd && ! shift)
        {
            if (tapeStrip->pasteAtPlayhead()) return true;
        }
        if ((key == juce::KeyPress::deleteKey
             || key == juce::KeyPress::backspaceKey)
            && noMods)
        {
            if (tapeStrip->deleteSelectedRegion()) return true;
        }
    }

    // ── File: Ctrl/Cmd+S (save), Ctrl/Cmd+Shift+S (save as), Ctrl/Cmd+O (open) ──
    // Buttons are gone (replaced by the menu bar) - dispatch to menu IDs
    // directly so the keyboard path keeps working.
    if (code == 'S' && cmd && ! shift) { menuItemSelected (1003, 0); return true; }   // Save
    if (code == 'S' && cmd &&   shift) { menuItemSelected (1004, 0); return true; }   // Save as
    if (code == 'O' && cmd)            { menuItemSelected (1002, 0); return true; }   // Open

    // ── Bounce: Ctrl/Cmd+B (Logic-style; intuitive "B for Bounce") ──
    if (code == 'B' && cmd) { menuItemSelected (1011, 0); return true; }              // Bounce

    // ── Transport: Space toggles play/stop, R toggles record ──
    // Pro Tools / Reaper / Logic / Bitwig all use Space as the universal
    // play-stop toggle. R for record matches Pro Tools / Cubase. Both
    // require no modifiers so a focused button or text editor still owns
    // the key.
    if (key == juce::KeyPress::spaceKey && noMods)
    {
        auto& transport = engine.getTransport();
        if (transport.isStopped()) engine.play();
        else                       engine.stop();
        return true;
    }
    if (code == 'R' && noMods)
    {
        auto& transport = engine.getTransport();
        if (transport.isRecording()) engine.stop();
        else                         engine.record();
        return true;
    }

    // ── Mode toggles: L (loop), P (punch). Same vibe as REC/Loop in
    // Pro Tools - single-letter, no modifier. Skipped when modifiers are
    // held so they don't shadow a future Cmd+L / Cmd+P.
    if (code == 'L' && noMods)
    {
        auto& t = engine.getTransport();
        t.setLoopEnabled (! t.isLoopEnabled());
        return true;
    }
    if (code == 'P' && noMods)
    {
        auto& t = engine.getTransport();
        t.setPunchEnabled (! t.isPunchEnabled());
        return true;
    }

    // ── Navigation: Home → playhead to 0 (universal). End is intentionally
    // skipped because "end of timeline" isn't a fixed sample on a portastudio
    // - the timeline grows with the longest region.
    if (key == juce::KeyPress::homeKey)
    {
        engine.getTransport().setPlayhead (0);
        return true;
    }

    // ── Markers: 'M' (no modifiers) drops a marker at the current playhead.
    // Common DAW shortcut. Skips when typing - the noMods guard means this
    // only fires when no text editor has focus.
    if (code == 'M' && noMods)
    {
        const auto playhead = engine.getTransport().getPlayhead();
        um.beginNewTransaction ("Add marker");
        um.perform (new AddMarkerAction (session, playhead));
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }

    // ── Loop / punch: bracket keys set the current playhead as in/out;
    // L and P toggle the corresponding mode on/off. Shift+bracket switches
    // to punch boundaries; the unshifted form sets loop boundaries.
    auto& transport = engine.getTransport();
    if (code == '[' && ! cmd && ! mods.isAltDown())
    {
        const auto playhead = transport.getPlayhead();
        if (shift)
        {
            const auto end = transport.getPunchOut();
            transport.setPunchRange (playhead,
                                       end > playhead ? end : playhead);
        }
        else
        {
            const auto end = transport.getLoopEnd();
            transport.setLoopRange (playhead,
                                      end > playhead ? end : playhead);
        }
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if (code == ']' && ! cmd && ! mods.isAltDown())
    {
        const auto playhead = transport.getPlayhead();
        if (shift)
        {
            const auto start = transport.getPunchIn();
            transport.setPunchRange (start < playhead ? start : playhead,
                                       playhead);
        }
        else
        {
            const auto start = transport.getLoopStart();
            transport.setLoopRange (start < playhead ? start : playhead,
                                      playhead);
        }
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if (code == 'L' && noMods)
    {
        transport.setLoopEnabled (! transport.isLoopEnabled());
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if (code == 'P' && noMods)
    {
        transport.setPunchEnabled (! transport.isPunchEnabled());
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }

    // ── Metronome click toggle: 'C' (no modifiers). Matches the C/I
    // (count-in) abbreviation already used on the transport bar's button.
    // The TransportBar's clickToggle button polls the same atom on its
    // 30 Hz timer and re-syncs its visual state.
    if (code == 'C' && noMods)
    {
        auto& enabled = session.metronomeEnabled;
        enabled.store (! enabled.load (std::memory_order_relaxed),
                        std::memory_order_relaxed);
        return true;
    }

    // ── Track arm / solo / mute on the selected track. Selection state
    // lives on TapeStrip (the most-recently-clicked region's track); when
    // nothing's selected, the shortcuts no-op rather than guessing. The
    // ChannelStrip's existing 30 Hz timer picks up the atom changes and
    // refreshes its toggles.
    if (tapeStrip != nullptr)
    {
        const int sel = tapeStrip->getSelectedTrack();
        if (sel >= 0 && sel < Session::kNumTracks)
        {
            if (code == 'A' && noMods)
            {
                auto& a = session.track (sel).recordArmed;
                a.store (! a.load (std::memory_order_relaxed),
                          std::memory_order_relaxed);
                return true;
            }
            if (code == 'S' && noMods)
            {
                auto& s = session.track (sel).strip.solo;
                s.store (! s.load (std::memory_order_relaxed),
                          std::memory_order_relaxed);
                return true;
            }
            // 'X' = mute toggle. M is already taken by drop-marker; X is
            // mnemonic for "kill / cross out" and matches Reaper's mute
            // keybinding.
            if (code == 'X' && noMods)
            {
                auto& m = session.track (sel).strip.mute;
                m.store (! m.load (std::memory_order_relaxed),
                          std::memory_order_relaxed);
                return true;
            }
        }
    }

    // ── Window: F11 toggles fullscreen. Walks up to the parent
    // ResizableWindow because that's the layer that owns the OS window
    // state, not MainComponent itself.
    if (key == juce::KeyPress::F11Key)
    {
        if (auto* window = findParentComponentOfClass<juce::ResizableWindow>())
        {
            window->setFullScreen (! window->isFullScreen());
            return true;
        }
    }

    return false;
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (8);

    // Slim menu-bar row at the very top. The menu bar grows to fit its
    // top-level menu names (~80 px); status text + system meters share
    // the rest of the row to its right.
    auto top = area.removeFromTop (22);
    menuBar.setBounds (top.removeFromLeft (140));
    top.removeFromLeft (8);
    if (systemStatusBar != nullptr)
        systemStatusBar->setBounds (top.removeFromRight (300));
    top.removeFromRight (8);
    statusLabel.setBounds (top);
    area.removeFromTop (4);

    const auto curStage = engine.getStage();
    const bool inMastering = (curStage == AudioEngine::Stage::Mastering);
    const bool inAux       = (curStage == AudioEngine::Stage::Aux);
    const bool inFullscreenView = inMastering || inAux;

    // ── Combined transport / stage / bank row ──
    // Everything that used to live on three separate rows (stage selector,
    // bank buttons, transport bar) collapses into ONE row. The transport
    // bar paints the row's chrome + transport buttons + clock + right-edge
    // toggles; we overlay the stage selector centred on top of its hint
    // area, and bank buttons just to the left of the stage block. The
    // hint label is hidden so it doesn't render under the overlays.
    constexpr int kRowH = 44;
    juce::Rectangle<int> rowBounds;
    if (! inFullscreenView && transportBar != nullptr)
    {
        rowBounds = area.removeFromTop (kRowH);
        transportBar->setBounds (rowBounds);
        transportBar->setHintVisible (false);
    }
    else
    {
        // In mastering / aux the transport bar is hidden; the stage selector
        // still needs a row to live on. Reserve the same height so the stage
        // row doesn't jump positions when the user switches stages.
        rowBounds = area.removeFromTop (kRowH);
    }

    const int stageW = 110;
    constexpr int kStageBtnH = 28;
    const int stageBlockW = stageW * 4;
    const int stageY = rowBounds.getY() + (rowBounds.getHeight() - kStageBtnH) / 2;
    const int stageX = rowBounds.getX() + (rowBounds.getWidth() - stageBlockW) / 2;
    recordingStageBtn.setBounds (stageX,                stageY, stageW, kStageBtnH);
    mixingStageBtn   .setBounds (stageX + stageW,       stageY, stageW, kStageBtnH);
    auxStageBtn      .setBounds (stageX + 2 * stageW,   stageY, stageW, kStageBtnH);
    masteringStageBtn.setBounds (stageX + 3 * stageW,   stageY, stageW, kStageBtnH);
    // Children added before transportBar in the constructor render UNDER it
    // by default. Bring the overlay buttons to the front so the transport
    // bar's painted hint area doesn't bury them.
    recordingStageBtn.toFront (false);
    mixingStageBtn   .toFront (false);
    auxStageBtn      .toFront (false);
    masteringStageBtn.toFront (false);

    // Banking decision: console can't fit all 16 strips at reference width.
    // Hidden in mastering / aux (no console there). Banks slot in just to
    // the left of the centred stage block.
    const bool needsBanking = (consoleView != nullptr) && (! inFullscreenView)
                             && (area.getWidth() < consoleView->fixedWidthFor16Tracks());
    bankAButton.setVisible (needsBanking);
    bankBButton.setVisible (needsBanking);
    if (needsBanking)
    {
        constexpr int kBankBtnW = 130;
        constexpr int kBankBtnGap = 6;
        constexpr int kBankBtnH = 26;
        const int bankY = rowBounds.getY() + (rowBounds.getHeight() - kBankBtnH) / 2;
        const int bankBlockEndX = stageX - 16;        // 16 px gap from stage block
        const int bankX         = bankBlockEndX - (kBankBtnW * 2 + kBankBtnGap);
        bankAButton.setBounds (bankX, bankY, kBankBtnW, kBankBtnH);
        bankBButton.setBounds (bankX + kBankBtnW + kBankBtnGap, bankY,
                                kBankBtnW, kBankBtnH);
        bankAButton.toFront (false);
        bankBButton.toFront (false);
        // Sync toggle state with the console's current bank.
        const int b = consoleView->getBank();
        bankAButton.setToggleState (b == 0, juce::dontSendNotification);
        bankBButton.setToggleState (b == 1, juce::dontSendNotification);
    }
    area.removeFromTop (4);

    if (! inFullscreenView)
    {
        if (tapeStrip != nullptr && tapeStripExpanded)
        {
            tapeStrip->setBounds (area.removeFromTop (TapeStrip::naturalHeight()));
            area.removeFromTop (4);
        }

        if (consoleView != nullptr) consoleView->setBounds (area);
    }
    else if (inMastering)
    {
        if (masteringView != nullptr) masteringView->setBounds (area);
    }
    else if (inAux)
    {
        if (auxView != nullptr) auxView->setBounds (area);
    }

    // Re-centre the startup modal + its dim backdrop when the main window
    // resizes while the dialog is up. DimOverlay::parentSizeChanged handles
    // its own resize, but we still drive the centred dialog bounds.
    if (startupDialog != nullptr)
    {
        startupDialog->setBounds (getLocalBounds()
                                       .withSizeKeepingCentre (startupDialog->getWidth(),
                                                                  startupDialog->getHeight()));
    }
}

void MainComponent::openAudioSettings()
{
    // Embedded modal - identical UX to a window (Esc to dismiss, click
    // outside to close, body holds focus) but rendered over the main
    // canvas so we don't fragment across OS windows. The Modal helper
    // handles the dim overlay + body lifetime.
    if (audioSettingsModal.isOpen()) return;
    auto panel = std::make_unique<AudioSettingsPanel> (engine.getDeviceManager(),
                                                          engine, session);
    panel->setSize (600, 520);
    audioSettingsModal.show (*this, std::move (panel));
}

void MainComponent::switchToStage (AudioEngine::Stage s)
{
    if (engine.getStage() == s) return;

    engine.setStage (s);

    // Mixing/Recording share the console + tape strip; Mastering swaps to
    // MasteringView; Aux swaps to AuxView. Both heavy views are constructed
    // lazily so users who never visit them pay no startup cost.
    const bool wantMastering = (s == AudioEngine::Stage::Mastering);
    const bool wantAux       = (s == AudioEngine::Stage::Aux);
    const bool wantMixing    = (s == AudioEngine::Stage::Mixing);
    const bool wantFullscreenView = wantMastering || wantAux;

    if (consoleView   != nullptr) consoleView  ->setVisible (! wantFullscreenView);
    if (transportBar  != nullptr) transportBar ->setVisible (! wantFullscreenView);
    if (tapeStrip     != nullptr) tapeStrip    ->setVisible (! wantFullscreenView && tapeStripExpanded);

    // Channel strips swap their tracking-only block (input/IN/ARM/PRINT) for
    // a row of 4 AUX send knobs while in Mixing.
    if (consoleView != nullptr) consoleView->setStripsMixingMode (wantMixing);

    if (wantMastering)
    {
        if (masteringView == nullptr)
        {
            masteringView = std::make_unique<MasteringView> (session, engine);
            addAndMakeVisible (masteringView.get());
        }
        masteringView->setVisible (true);
    }
    else if (masteringView != nullptr)
    {
        masteringView->setVisible (false);
    }

    if (wantAux)
    {
        if (auxView == nullptr)
        {
            auxView = std::make_unique<AuxView> (session, engine);
            addAndMakeVisible (auxView.get());
        }
        auxView->setVisible (true);
    }
    else if (auxView != nullptr)
    {
        auxView->setVisible (false);
    }

    // Sync the segmented buttons (radio group means only one is on, but
    // explicitly setting the right one keeps a programmatic switch - like
    // doMixdown's auto-handoff - visually consistent).
    recordingStageBtn.setToggleState (s == AudioEngine::Stage::Recording, juce::dontSendNotification);
    mixingStageBtn   .setToggleState (s == AudioEngine::Stage::Mixing,    juce::dontSendNotification);
    auxStageBtn      .setToggleState (s == AudioEngine::Stage::Aux,       juce::dontSendNotification);
    masteringStageBtn.setToggleState (s == AudioEngine::Stage::Mastering, juce::dontSendNotification);

    resized();
}

void MainComponent::doMixdown()
{
    // One-shot bounce of the master mix to <sessionDir>/mixdown.wav, then
    // auto-switch to Mastering with that file loaded. No file dialog -
    // overwrites mixdown.wav each time. The "Bounce..." button retains
    // the explicit dialog flow for ad-hoc renders.
    auto target = session.getSessionDirectory().getChildFile ("mixdown.wav");

    auto panel = std::make_unique<BounceDialog> (engine, session,
                                                   engine.getDeviceManager(), target);
    panel->setSize (520, 200);

    // Hand off to Mastering once the bounce finishes successfully. The
    // dialog fires this on its message-thread "Close" path, well after the
    // worker has restored engine state, so the stage flip is safe.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    panel->onSuccessfulFinish = [safeThis] (juce::File rendered)
    {
        if (safeThis == nullptr) return;
        safeThis->mixdownModal.close();
        safeThis->switchToStage (AudioEngine::Stage::Mastering);
        if (safeThis->masteringView != nullptr)
            safeThis->masteringView->loadFile (rendered);
    };

    mixdownModal.show (*this, std::move (panel));
}

void MainComponent::launchStartupDialog()
{
    auto recents = RecentSessions::load();

    startupDialog = std::make_unique<StartupDialog> (recents);
    startupDialog->setSize (560, juce::jlimit (220, 520,
                                                 140 + (recents.isEmpty() ? 60 : recents.size() * 30)));

    startupDialog->onOpenRecent = [this] (juce::File dir)
    {
        loadSessionFromJson (dir.getChildFile ("session.json"));
    };
    startupDialog->onNewSession = [this] { newSessionPrompt(); };
    startupDialog->onOpenFile   = [this] { openFromFilePrompt(); };
    startupDialog->onSkip       = [] {};  // nothing - the bootstrap default dir stays
    // closeDialog calls onDismiss after each action; the host (us) tears
    // down the embedded dialog + its dim backdrop together.
    startupDialog->onDismiss    = [this] { dismissStartupDialog(); };

    // Dim backdrop covers the rest of the UI; clicking the dim is a
    // shortcut for "Continue blank" — the same as the dialog's own Skip
    // button — so it doesn't trap the user when they want to dismiss.
    startupDim = std::make_unique<DimOverlay>();
    startupDim->setBounds (getLocalBounds());
    startupDim->onClick = [this] { dismissStartupDialog(); };
    addAndMakeVisible (startupDim.get());

    // Centered on the main window. The dialog is plain dark — no native
    // title bar — to match the embedded-modal aesthetic shared with the
    // TapeMachine gear modal and the SUMMARY EQ/COMP popups.
    const auto bounds = getLocalBounds()
                            .withSizeKeepingCentre (startupDialog->getWidth(),
                                                       startupDialog->getHeight());
    startupDialog->setBounds (bounds);
    addAndMakeVisible (startupDialog.get());
}

void MainComponent::dismissStartupDialog()
{
    // Defer the actual delete by one message-loop tick — closeDialog is
    // typically called from inside one of the dialog's own button click
    // handlers, and tearing down the dialog from inside its own callback
    // chain is fragile. callAsync runs on the message thread after the
    // click handler returns, when nothing's still on the dialog's stack.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis == nullptr) return;
        safeThis->startupDialog.reset();
        safeThis->startupDim.reset();
    });
}

void MainComponent::newSessionPrompt()
{
    // Single-dialog "Save As" UX: filename text field + folder browser in
    // one step. The typed name becomes the session folder; the navigated
    // directory becomes its parent.
    auto startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                        .getChildFile ("Focal");
    if (! startDir.exists()) startDir.createDirectory();

    sessionFileChooser = std::make_unique<juce::FileChooser> (
        "Name your new session",
        startDir.getChildFile ("MySong"),
        juce::String());

    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting;

    sessionFileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        if (chosen == juce::File()) return;

        // Treat the chosen path as the new session folder. saveSessionTo
        // creates the dir + audio subdir and persists an initial session.json
        // so subsequent saves don't re-prompt.
        saveSessionTo (chosen);
    });
}

bool MainComponent::saveSessionTo (const juce::File& dir)
{
    if (dir == juce::File()) return false;

    // setSessionDirectory creates the dir + audio subdir if missing - safe
    // to call even when the user picked an existing session folder.
    session.setSessionDirectory (dir);

    // Snapshot live plugin slot state into the session model immediately
    // before serialising - the serializer only reads from Session, so
    // anything live in the engine that isn't atomically mirrored has to
    // be published first.
    engine.publishPluginStateForSave();
    engine.publishTransportStateForSave();

    const auto target = dir.getChildFile ("session.json");
    if (SessionSerializer::save (session, target))
    {
        RecentSessions::add (dir);
        // A successful manual save makes the autosave stale - drop it so the
        // recovery prompt doesn't fire on the next clean load.
        deleteAutosaveFor (dir);
        statusLabel.setText ("Saved: " + target.getFullPathName(), juce::dontSendNotification);
        return true;
    }
    statusLabel.setText ("Save failed at " + target.getFullPathName(), juce::dontSendNotification);
    return false;
}

juce::File MainComponent::getAutosaveFileFor (const juce::File& sessionDir) const
{
    return sessionDir.getChildFile ("session.json.autosave");
}

bool MainComponent::autosaveIsNewerThan (const juce::File& sessionJson) const
{
    const auto autosave = getAutosaveFileFor (sessionJson.getParentDirectory());
    if (! autosave.existsAsFile()) return false;
    if (! sessionJson.existsAsFile()) return true;   // autosave is the only state we have
    return autosave.getLastModificationTime() > sessionJson.getLastModificationTime();
}

void MainComponent::deleteAutosaveFor (const juce::File& sessionDir) const
{
    const auto autosave = getAutosaveFileFor (sessionDir);
    if (autosave.existsAsFile()) autosave.deleteFile();
}

void MainComponent::writeAutosave()
{
    const auto dir = session.getSessionDirectory();
    if (dir == juce::File()) return;

    // Ensure the directory exists before serialising. setSessionDirectory's
    // ctor path normally handles this, but a session that was constructed
    // via the default ~/Music/Focal/Untitled fallback may never have been
    // touched on disk yet.
    dir.createDirectory();

    // Same publish bookend as the manual save - the serializer reads only
    // from Session, not from the live engine.
    engine.publishPluginStateForSave();
    engine.publishTransportStateForSave();

    const auto target = getAutosaveFileFor (dir);
    if (! SessionSerializer::save (session, target))
        DBG ("MainComponent: autosave write failed at " << target.getFullPathName());
}

void MainComponent::timerCallback()
{
    // The audio thread is independent of the message thread, so a slow JSON
    // serialise here doesn't glitch playback. Still, we want autosave cheap -
    // SessionSerializer::save on a 16-track session should land well under
    // 30 ms.
    writeAutosave();
}

void MainComponent::saveSessionAndThen (std::function<void(bool)> onComplete)
{
    const auto dir = session.getSessionDirectory();
    if (dir.getChildFile ("session.json").existsAsFile())
    {
        // Sync save into the existing dir.
        const bool ok = saveSessionTo (dir);
        if (onComplete) onComplete (ok);
        return;
    }

    // No prior save - open a Save As dialog where the user types the session
    // name and picks a parent directory in one step. The chooser is in
    // saveMode | canSelectFiles so the OS shows a filename text field; we
    // then treat the resulting "file" path as the session folder we'll
    // create (saveSessionTo already creates the directory if missing).
    auto startDir = session.getSessionDirectory().getParentDirectory();
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                       .getChildFile ("Focal");
    if (! startDir.exists()) startDir.createDirectory();

    juce::String defaultName = session.getSessionDirectory().getFileName();
    if (defaultName.isEmpty() || defaultName == "Untitled") defaultName = "MySong";

    sessionFileChooser = std::make_unique<juce::FileChooser> (
        "Save session as...",
        startDir.getChildFile (defaultName),
        juce::String());

    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting;

    sessionFileChooser->launchAsync (chooserFlags,
        [this, onComplete = std::move (onComplete)] (const juce::FileChooser& fc)
        {
            const auto chosen = fc.getResult();
            if (chosen == juce::File())
            {
                if (onComplete) onComplete (false);
                return;
            }
            const bool ok = saveSessionTo (chosen);
            if (onComplete) onComplete (ok);
        });
}

void MainComponent::saveAsPrompt()
{
    // Single-dialog Save As: filename text field + folder browser in one
    // step. The typed name becomes the session folder; the navigated
    // directory becomes its parent. Replaces the old two-step modal-then-
    // chooser flow which only let the user browse, never type.
    auto startDir = session.getSessionDirectory().getParentDirectory();
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                       .getChildFile ("Focal");
    if (! startDir.exists()) startDir.createDirectory();

    juce::String defaultName = session.getSessionDirectory().getFileName();
    if (defaultName.isEmpty() || defaultName == "Untitled") defaultName = "MySong";

    sessionFileChooser = std::make_unique<juce::FileChooser> (
        "Save session as...",
        startDir.getChildFile (defaultName),
        juce::String());

    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting;

    sessionFileChooser->launchAsync (chooserFlags,
        [this] (const juce::FileChooser& fc)
        {
            const auto chosen = fc.getResult();
            if (chosen == juce::File()) return;
            saveSessionTo (chosen);
        });
}

bool MainComponent::loadSessionFromJson (const juce::File& sessionJson)
{
    if (! sessionJson.existsAsFile())
    {
        statusLabel.setText ("No session at " + sessionJson.getFullPathName(),
                             juce::dontSendNotification);
        return false;
    }

    const auto dir = sessionJson.getParentDirectory();

    // Recovery prompt path. The dialog is async so we hand it a continuation
    // that runs the actual load once the user has picked. The synchronous
    // bool return reflects only "we accepted the load attempt"; the load
    // itself completes after the dialog dismisses.
    if (autosaveIsNewerThan (sessionJson))
    {
        const auto autosave = getAutosaveFileFor (dir);
        juce::Component::SafePointer<MainComponent> safe (this);

        juce::AlertWindow::showAsync (
            juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                .withTitle ("Recover from autosave?")
                .withMessage (
                    "An autosave file is newer than the saved session at\n"
                    + dir.getFullPathName()
                    + "\n\nAutosave: "  + autosave  .getLastModificationTime().toString (true, true)
                    + "\nSaved:    "    + sessionJson.getLastModificationTime().toString (true, true)
                    + "\n\nFocal probably exited unexpectedly. "
                      "Recover the newer autosave, or load the saved session and discard it?")
                .withButton ("Recover autosave")
                .withButton ("Load saved session")
                .withButton ("Cancel"),
            [safe, sessionJson, dir, autosave] (int choice)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                // MessageBoxOptions returns 1 for the first button, 2 for second,
                // 3 for third. 0 means dismissed without a choice.
                if (choice == 0 || choice == 3) return;
                const auto src = (choice == 1) ? autosave : sessionJson;
                self->finishLoadingSessionFrom (src, dir);
            });
        return true;
    }

    return finishLoadingSessionFrom (sessionJson, dir);
}

bool MainComponent::finishLoadingSessionFrom (const juce::File& sourceJson,
                                                 const juce::File& dir)
{
    session.setSessionDirectory (dir);

    if (! SessionSerializer::load (session, sourceJson))
    {
        statusLabel.setText ("Load failed: " + sourceJson.getFullPathName(),
                             juce::dontSendNotification);
        return false;
    }

    // Autosave's job is done - clean up so the next load has a clean slate.
    // (Even when the user chose "load saved session" we drop the autosave, on
    // the assumption they've made a deliberate choice to discard it.)
    deleteAutosaveFor (dir);

    // After deserialisation, the Track::pluginDescriptionXml /
    // pluginStateBase64 fields are populated; ask the engine to
    // re-instantiate each track's plugin from those.
    engine.consumePluginStateAfterLoad();
    engine.consumeTransportStateAfterLoad();

    // Reconstruct the console so all controls reflect the freshly
    // loaded values (atomic state is in `session`, but UI widgets
    // captured initial values in their constructors).
    consoleView.reset();
    consoleView = std::make_unique<ConsoleView> (session, engine);
    addAndMakeVisible (consoleView.get());
    consoleView->setOnStripFocusRequested ([this] (int t)
    {
        if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
    });
    if (consoleView != nullptr && transportBar != nullptr)
        consoleView->setStripsCompactMode (tapeStripExpanded);
    resized();

    RecentSessions::add (dir);
    statusLabel.setText ("Loaded: " + sourceJson.getFullPathName(),
                         juce::dontSendNotification);
    return true;
}

void MainComponent::openFromFilePrompt()
{
    auto startDir = session.getSessionDirectory();
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    sessionFileChooser = std::make_unique<juce::FileChooser> (
        "Open session.json",
        startDir.getChildFile ("session.json"),
        "*.json");

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles;

    sessionFileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto chosen = fc.getResult();
        if (chosen == juce::File()) return;
        loadSessionFromJson (chosen);
    });
}

void MainComponent::openBounceDialog()
{
    auto defaultDir = session.getSessionDirectory();
    if (! defaultDir.isDirectory())
        defaultDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const auto defaultFile = defaultDir.getChildFile ("bounce.wav");

    bounceFileChooser = std::make_unique<juce::FileChooser> (
        "Bounce master mix to WAV",
        defaultFile,
        "*.wav");

    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting;

    bounceFileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto out = fc.getResult();
        if (out == juce::File()) return;  // user cancelled

        auto outFile = out;
        if (! outFile.hasFileExtension ("wav"))
            outFile = outFile.withFileExtension ("wav");

        auto panel = std::make_unique<BounceDialog> (engine, session,
                                                       engine.getDeviceManager(), outFile);
        panel->setSize (520, 200);
        bounceModal.show (*this, std::move (panel));
    });
}

// ── MenuBarModel ─────────────────────────────────────────────────────────
//
// Two top-level menus drive every header action that used to be a separate
// TextButton. Item IDs are namespaced per-menu so menuItemSelected can
// dispatch with a single switch, no need to also branch on the top-level
// menu index.
namespace
{
enum MenuItemId
{
    kMenuFileNew      = 1001,
    kMenuFileOpen     = 1002,
    kMenuFileSave     = 1003,
    kMenuFileSaveAs   = 1004,
    kMenuFileMixdown  = 1010,
    kMenuFileBounce   = 1011,
    kMenuFileCleanOut = 1012,
    kMenuFileQuit     = 1099,
    // Reserved range for template entries (one per SessionTemplate enum
    // value, indexed off this base). Stays well above the file-action IDs
    // so future additions don't collide.
    kMenuFileTemplateBase = 1200,
    kMenuSettingsAudio = 2001,
};
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Settings" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex,
                                                  const juce::String& /*name*/)
{
    juce::PopupMenu menu;
    if (topLevelMenuIndex == 0)   // File
    {
        menu.addItem (kMenuFileNew,    "New session...");

        // New From Template submenu - drops opinionated track names /
        // colours / modes onto the live session so the user is one
        // arm-click from recording. Iterates the SessionTemplate enum so
        // a new template appears here automatically.
        juce::PopupMenu templates;
        for (int i = 0; i < (int) SessionTemplate::kCount; ++i)
            templates.addItem (kMenuFileTemplateBase + i,
                                nameForTemplate ((SessionTemplate) i));
        menu.addSubMenu ("New from template", templates);

        menu.addItem (kMenuFileOpen,   "Open...");
        menu.addItem (kMenuFileSave,   "Save");
        menu.addItem (kMenuFileSaveAs, "Save as...");
        menu.addSeparator();
        menu.addItem (kMenuFileMixdown, "Mixdown");
        menu.addItem (kMenuFileBounce,  "Bounce...");
        menu.addSeparator();
        menu.addItem (kMenuFileCleanOut, "Clean out unreferenced files...");
        menu.addSeparator();
        menu.addItem (kMenuFileQuit, "Quit");
    }
    else if (topLevelMenuIndex == 1)   // Settings
    {
        menu.addItem (kMenuSettingsAudio, "Audio settings...");
    }
    return menu;
}

void MainComponent::menuItemSelected (int menuItemID, int /*topLevelMenuIndex*/)
{
    switch (menuItemID)
    {
        case kMenuFileNew:    newSessionPrompt();       break;
        case kMenuFileOpen:   openFromFilePrompt();     break;
        case kMenuFileSave:
        {
            // Mirror the Save button's smart behavior: redirect to Save As
            // if the session has never been saved (no session.json yet) so
            // the user picks a real destination instead of clobbering the
            // bootstrap "Untitled" dir.
            const auto dir = session.getSessionDirectory();
            if (! dir.getChildFile ("session.json").existsAsFile())
                saveAsPrompt();
            else
                saveSessionTo (dir);
            break;
        }
        case kMenuFileSaveAs: saveAsPrompt();           break;
        case kMenuFileMixdown: doMixdown();             break;
        case kMenuFileBounce:  openBounceDialog();      break;
        case kMenuFileCleanOut: cleanOutUnreferencedFiles(); break;
        case kMenuFileQuit:
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
            break;
        case kMenuSettingsAudio: openAudioSettings();   break;
        default:
            // Template menu items live in [kMenuFileTemplateBase, +kCount).
            if (menuItemID >= kMenuFileTemplateBase
                && menuItemID < kMenuFileTemplateBase + (int) SessionTemplate::kCount)
            {
                applyTemplate (session,
                                (SessionTemplate) (menuItemID - kMenuFileTemplateBase));
                // Rebuild the console view so the new track names / colours
                // / modes propagate into existing strip components - the
                // simplest way to pick up name + colour + mode in one shot.
                consoleView.reset();
                consoleView = std::make_unique<ConsoleView> (session, engine);
                addAndMakeVisible (consoleView.get());
                consoleView->setOnStripFocusRequested ([this] (int t)
                {
                    if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
                });
                consoleView->setStripsMixingMode (
                    engine.getStage() == AudioEngine::Stage::Mixing);
                if (tapeStrip != nullptr) tapeStrip->repaint();
                resized();
            }
            break;
    }
}

void MainComponent::cleanOutUnreferencedFiles()
{
    // Build the set of WAVs the session is currently using - both the
    // live region's `file` and every previousTakes entry. Anything in
    // the audio dir not in this set is fair game for deletion.
    auto audioDir = session.getAudioDirectory();
    if (! audioDir.isDirectory())
    {
        juce::AlertWindow::showAsync (
            juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::InfoIcon)
                .withTitle ("Clean out")
                .withMessage ("This session has no audio directory yet, "
                                "so there's nothing to clean.")
                .withButton ("OK"),
            nullptr);
        return;
    }

    juce::StringArray referenced;   // full paths
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        for (const auto& r : session.track (t).regions)
        {
            referenced.addIfNotAlreadyThere (r.file.getFullPathName());
            for (const auto& take : r.previousTakes)
                referenced.addIfNotAlreadyThere (take.file.getFullPathName());
        }
    }

    // Walk the audio directory for .wav files. Anything outside the
    // referenced set is a candidate. Subdirectories are intentionally
    // skipped so external WAVs the user dropped in by hand don't get
    // touched.
    juce::Array<juce::File> candidates;
    juce::int64 totalBytes = 0;
    for (const auto& f : audioDir.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        if (! referenced.contains (f.getFullPathName()))
        {
            candidates.add (f);
            totalBytes += f.getSize();
        }
    }

    if (candidates.isEmpty())
    {
        juce::AlertWindow::showAsync (
            juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::InfoIcon)
                .withTitle ("Clean out")
                .withMessage ("No unreferenced files found. The audio "
                                "directory is already clean.")
                .withButton ("OK"),
            nullptr);
        return;
    }

    const auto sizeMB = (double) totalBytes / (1024.0 * 1024.0);
    const auto msg = "Found " + juce::String (candidates.size())
                   + " unreferenced .wav file(s) totalling "
                   + juce::String (sizeMB, 1) + " MB.\n\n"
                   + "These were created by past record passes that no "
                     "longer have any region or take pointing at them. "
                     "Deleting cannot be undone.";

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle ("Clean out unreferenced files")
            .withMessage (msg)
            .withButton ("Delete")
            .withButton ("Cancel"),
        [safeThis = juce::Component::SafePointer<MainComponent> (this), candidates] (int buttonIdx)
        {
            if (buttonIdx != 1) return;   // 1 = first button = Delete
            int deleted = 0;
            for (const auto& f : candidates)
                if (f.deleteFile()) ++deleted;
            if (auto* self = safeThis.getComponent())
                self->statusLabel.setText ("Deleted " + juce::String (deleted)
                                            + " unreferenced file(s).",
                                            juce::dontSendNotification);
        });
}

void MainComponent::openPianoRoll (int trackIdx, int regionIdx)
{
    // Toggle vs swap. Clicking the SAME region while the roll is already
    // open dismisses it (a second click on a region is naturally read as
    // "I'm done"); clicking a DIFFERENT region tears the current roll
    // down and re-opens on the new target so the user doesn't have to
    // dismiss-then-click.
    if (pianoRoll != nullptr)
    {
        const bool sameRegion = (pianoRollTrackIdx == trackIdx
                                  && pianoRollRegionIdx == regionIdx);
        closePianoRoll();
        if (sameRegion) return;
    }

    pianoRoll = std::make_unique<PianoRollComponent> (session, trackIdx, regionIdx);
    pianoRollTrackIdx  = trackIdx;
    pianoRollRegionIdx = regionIdx;
    pianoRollDim = std::make_unique<DimOverlay>();

    // Sized as a centred panel that leaves a small inset on each side so
    // the dimmed backdrop is still visible (helps users see they're in a
    // modal). The inset shrinks on small windows so the roll always has
    // a workable surface even on a 1280-wide screen.
    const auto bounds = getLocalBounds();
    const int inset = juce::jmax (24, juce::jmin (bounds.getWidth(), bounds.getHeight()) / 16);
    const auto rollBounds = bounds.reduced (inset);

    pianoRollDim->setBounds (bounds);
    pianoRollDim->onClick = [this] { closePianoRoll(); };
    addAndMakeVisible (pianoRollDim.get());

    pianoRoll->setBounds (rollBounds);
    pianoRoll->onCloseRequested = [this] { closePianoRoll(); };
    addAndMakeVisible (pianoRoll.get());
    pianoRoll->grabKeyboardFocus();
}

void MainComponent::closePianoRoll()
{
    if (pianoRoll != nullptr) removeChildComponent (pianoRoll.get());
    if (pianoRollDim != nullptr) removeChildComponent (pianoRollDim.get());
    pianoRoll.reset();
    pianoRollDim.reset();
    pianoRollTrackIdx  = -1;
    pianoRollRegionIdx = -1;
    if (tapeStrip != nullptr) tapeStrip->repaint();
}

namespace
{
// Tiny Timer subclass used by the tuner overlay to poll the engine's
// pitch atoms at 30 Hz on the message thread. Public-internal because
// the unique_ptr in MainComponent.h holds it as juce::Timer*.
class TunerPoller final : public juce::Timer
{
public:
    TunerPoller (Session& s, TunerOverlay& o) : session (s), overlay (o)
    {
        startTimerHz (30);
    }
    void timerCallback() override
    {
        const float hz = session.tuneLatestHz   .load (std::memory_order_relaxed);
        const float lv = session.tuneLatestLevel.load (std::memory_order_relaxed);
        overlay.setDetected (hz, lv);
    }
private:
    Session& session;
    TunerOverlay& overlay;
};
} // namespace

void MainComponent::toggleTuner()
{
    if (tuner != nullptr) { closeTuner(); return; }

    // Pick the track to tune. Prefers the user's most-recent selection
    // (the same selectedTrack the keyboard shortcuts use); falls back
    // to track 0 when nothing's selected so the button always does
    // something rather than silently failing.
    int trackIdx = 0;
    if (tapeStrip != nullptr)
    {
        const int sel = tapeStrip->getSelectedTrack();
        if (sel >= 0 && sel < Session::kNumTracks) trackIdx = sel;
    }
    session.tuneTrackIndex.store (trackIdx, std::memory_order_relaxed);
    session.tuneLatestHz   .store (0.0f,    std::memory_order_relaxed);
    session.tuneLatestLevel.store (0.0f,    std::memory_order_relaxed);

    tuner = std::make_unique<TunerOverlay>();
    tuner->onDismiss = [this] { closeTuner(); };
    tunerDim = std::make_unique<DimOverlay>();
    tunerDim->setBounds (getLocalBounds());
    tunerDim->onClick = [this] { closeTuner(); };
    addAndMakeVisible (tunerDim.get());

    tuner->setBounds (getLocalBounds());
    addAndMakeVisible (tuner.get());

    tunerPoller = std::make_unique<TunerPoller> (session, *tuner);
}

void MainComponent::closeTuner()
{
    tunerPoller.reset();
    if (tuner    != nullptr) removeChildComponent (tuner.get());
    if (tunerDim != nullptr) removeChildComponent (tunerDim.get());
    tuner.reset();
    tunerDim.reset();
    session.tuneTrackIndex.store (-1, std::memory_order_relaxed);
}
} // namespace focal
