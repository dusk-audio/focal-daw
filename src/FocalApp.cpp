#include "FocalApp.h"
#include "ui/AppConfig.h"
#include "ui/ConsoleView.h"
#include "ui/MainComponent.h"
#include "ui/WindowState.h"
#include "engine/AudioEngine.h"
#include "engine/AudioPipelineSelfTest.h"
#if defined(__linux__)
 #include "engine/alsa/AlsaAudioIODeviceType.h"
 #include "engine/alsa/AlsaPerformanceTest.h"
#endif
#include "session/Session.h"

#include <cstdio>
#include <cstdlib>

#if JUCE_LINUX
 #include <sys/mman.h>
 #include <sys/resource.h>
#endif

namespace focal
{
class FocalApp::MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (juce::String name)
        : DocumentWindow (std::move (name),
                          juce::Desktop::getInstance().getDefaultLookAndFeel()
                              .findColour (juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent(), true);
        setResizable (true, true);  // resizable + corner resizer
        // Min height keeps the console usable; the tape strip is collapsible
        // so we don't need to budget for it in the floor.
        setResizeLimits (ConsoleView::minimumContentWidth() + 24, 750, 32768, 32768);

        // Restore prior session's window geometry. JUCE's
        // restoreWindowStateFromString rebuilds bounds + fullscreen state
        // from the same string getWindowStateAsString() produced. We then
        // sanity-check the restored bounds against connected displays -
        // if the saved monitor is gone, undo the restore and centre at
        // MainComponent's default size.
        bool restored = false;
        const auto savedState = WindowState::load();
        if (savedState.isNotEmpty() && restoreWindowStateFromString (savedState))
        {
            // Validate using the WINDOWED bounds (so a fullscreen-on-an-
            // unplugged-monitor case still falls back gracefully).
            const auto checkRect = isFullScreen()
                ? juce::Rectangle<int> (0, 0,
                                          juce::jmax (getWidth(), 800),
                                          juce::jmax (getHeight(), 600))
                : getBounds();
            if (WindowState::rectIsUsable (checkRect))
                restored = true;
        }

        if (! restored)
        {
            // Some tiling/Wayland WMs auto-maximize new windows. Explicitly
            // opt out of full-screen so we open at the size MainComponent
            // requested.
            setFullScreen (false);
            centreWithSize (getWidth(), getHeight());
        }

        setVisible (true);
    }

    void closeButtonPressed() override
    {
        // Always confirm on close, Ardour-style. Three options: cancel,
        // discard, save+quit. We don't try to track a dirty flag - every
        // close goes through the prompt so the user can never silently lose
        // unsaved work.
        auto* main = dynamic_cast<MainComponent*> (getContentComponent());

        juce::MessageBoxOptions opts;
        opts = opts.withIconType (juce::MessageBoxIconType::QuestionIcon)
                   .withTitle ("Quit Focal?")
                   .withMessage ("Any unsaved changes will be lost.\n\n"
                                  "What do you want to do?")
                   .withButton ("Don't quit")
                   .withButton ("Just quit")
                   .withButton ("Save and quit");

        juce::AlertWindow::showAsync (opts,
            [main] (int picked)
            {
                // showAsync's callback receives JUCE's button result code,
                // NOT a 0-based index. For three buttons added in order
                // [Don't quit, Just quit, Save and quit] the codes are:
                //   button[0] (Don't quit)    → 1
                //   button[1] (Just quit)     → 2
                //   button[2] (Save and quit) → 0
                // (See juce::AlertWindow::showAsync docs.)
                // Anything else (Esc, window close on the alert): cancel.
                if (picked == 1)
                {
                    // Don't quit - do nothing, leave the window open.
                    return;
                }
                if (picked == 2)
                {
                    // Just quit - close immediately, discarding unsaved work.
                    JUCEApplication::getInstance()->systemRequestedQuit();
                    return;
                }
                if (picked == 0)
                {
                    // Save and quit. saveSessionAndThen handles both the
                    // sync (existing session) and async (Save As file
                    // chooser) paths.
                    if (main == nullptr)
                    {
                        JUCEApplication::getInstance()->systemRequestedQuit();
                        return;
                    }
                    main->saveSessionAndThen ([] (bool savedOk)
                    {
                        if (savedOk)
                            JUCEApplication::getInstance()->systemRequestedQuit();
                        // If save failed (chooser cancelled, write error), the
                        // window stays open - user can retry or pick "Just quit".
                    });
                    return;
                }
                // Unknown / dismissed: treat as cancel.
            });
    }
};

FocalApp::FocalApp() = default;
FocalApp::~FocalApp() = default;

#if JUCE_LINUX
static void primeRealtimeAudio()
{
    // Pin every page of the process in physical RAM so the audio thread
    // never blocks on a page fault during a callback. Ardour, Bitwig, and
    // every other low-latency Linux DAW does this. Requires `memlock` rlimit
    // - typically `unlimited` for the audio group via /etc/security/limits.d.
    if (mlockall (MCL_CURRENT | MCL_FUTURE) != 0)
    {
        DBG ("mlockall failed (errno=" << errno
             << ") - audio thread may suffer page-fault stalls under memory pressure");
    }
}
#endif

// Headless self-test entry: triggered by setting FOCAL_RUN_SELFTEST=1 in
// the environment before launching. Runs AudioPipelineSelfTest::runAll() at
// startup, writes the formatted report to stdout, and quits. The MainWindow
// (and the entire UI) is never created in this mode. Useful for automated
// loops without a display server.
static bool envFlagSet (const char* name)
{
    const char* v = std::getenv (name);
    if (v == nullptr) return false;
    const juce::String s (v);
    return s.getIntValue() != 0
        || s.equalsIgnoreCase ("true")
        || s.equalsIgnoreCase ("yes");
}

// Headless tone-test probe: opens a chosen device at a chosen rate/buffer
// and runs the same juce::AudioDeviceManager::playTestSound() that the GUI
// "Test" button in AudioDeviceSelectorComponent calls. No AudioEngine is
// attached - this isolates the JUCE backend + driver path from any engine
// DSP. Useful for diagnosing distortion that appears when pressing the
// Test button at low buffer sizes on ALSA + USB interfaces.
//
// Env vars (all optional, sensible defaults):
//   FOCAL_TONE_BACKEND     "ALSA" | "JACK"           (default "ALSA")
//   FOCAL_TONE_DEVICE      output device name        (default "" = backend default)
//   FOCAL_TONE_RATE        sample rate in Hz         (default 48000)
//   FOCAL_TONE_BUFFER      buffer size in samples    (default 128)
//   FOCAL_TONE_DURATION_MS playback duration (ms)    (default 2000)
static void runHeadlessToneTest()
{
    auto env = [] (const char* name) -> juce::String {
        if (const char* v = std::getenv (name)) return juce::String (v);
        return {};
    };
    const juce::String backendName = env ("FOCAL_TONE_BACKEND").isNotEmpty()
                                     ? env ("FOCAL_TONE_BACKEND") : juce::String ("ALSA");
    const juce::String deviceName  = env ("FOCAL_TONE_DEVICE");
    const double       targetRate  = env ("FOCAL_TONE_RATE").isNotEmpty()
                                     ? env ("FOCAL_TONE_RATE").getDoubleValue() : 48000.0;
    const int          targetBuf   = env ("FOCAL_TONE_BUFFER").isNotEmpty()
                                     ? env ("FOCAL_TONE_BUFFER").getIntValue()  : 128;
    const int          durationMs  = env ("FOCAL_TONE_DURATION_MS").isNotEmpty()
                                     ? env ("FOCAL_TONE_DURATION_MS").getIntValue() : 2000;

    juce::AudioDeviceManager dm;

    std::fprintf (stdout, "=== Focal Headless Tone Test ===\n");
    std::fprintf (stdout, "Requested: backend=%s device=\"%s\" rate=%.0f buf=%d duration=%dms\n",
                  backendName.toRawUTF8(), deviceName.toRawUTF8(),
                  targetRate, targetBuf, durationMs);

    // Linux: pre-register Focal's ALSA backend + JACK before init, same
    // pattern AudioEngine uses. Stops JUCE's createDeviceTypesIfNeeded
    // from auto-registering its stock ALSA path (which would collide on
    // the type-name "ALSA" with ours). Pre-scanning lets init's
    // pickCurrentDeviceTypeWithDevices read device counts without tripping
    // hasScanned assertions.
   #if defined(__linux__)
    if (auto* jackType = juce::AudioIODeviceType::createAudioIODeviceType_JACK())
        dm.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (jackType));
    dm.addAudioDeviceType (std::make_unique<focal::AlsaAudioIODeviceType>());
    for (auto* type : dm.getAvailableDeviceTypes())
        if (type != nullptr) type->scanForDevices();
   #endif

    if (const auto err = dm.initialiseWithDefaultDevices (0, 2); err.isNotEmpty())
        std::fprintf (stdout, "init: %s\n", err.toRawUTF8());

    dm.setCurrentAudioDeviceType (backendName, /*treatAsChosen*/ true);

    auto setup = dm.getAudioDeviceSetup();
    if (deviceName.isNotEmpty())
        setup.outputDeviceName = deviceName;
    setup.sampleRate            = targetRate;
    setup.bufferSize            = targetBuf;
    setup.useDefaultInputChannels  = true;
    setup.useDefaultOutputChannels = true;

    if (const auto err = dm.setAudioDeviceSetup (setup, /*treatAsChosen*/ true); err.isNotEmpty())
        std::fprintf (stdout, "setAudioDeviceSetup: %s\n", err.toRawUTF8());

    if (auto* dev = dm.getCurrentAudioDevice())
    {
        std::fprintf (stdout,
                      "Opened: \"%s\" type=%s rate=%.0f buf=%d activeOut=%d activeIn=%d bitDepth=%d\n",
                      dev->getName().toRawUTF8(),
                      dm.getCurrentAudioDeviceType().toRawUTF8(),
                      dev->getCurrentSampleRate(),
                      dev->getCurrentBufferSizeSamples(),
                      dev->getActiveOutputChannels().countNumberOfSetBits(),
                      dev->getActiveInputChannels().countNumberOfSetBits(),
                      dev->getCurrentBitDepth());
        const int xrunBefore = dev->getXRunCount();

        // Same call the GUI Test button makes. Plays a 440 Hz sine at -6 dB
        // through the open device for ~1 s.
        dm.playTestSound();
        juce::Thread::sleep (durationMs);

        const int xrunAfter = dev->getXRunCount();
        std::fprintf (stdout, "Backend xrun count: before=%d after=%d delta=%d\n",
                      xrunBefore, xrunAfter, xrunAfter - xrunBefore);
    }
    else
    {
        std::fprintf (stdout, "Open failed: getCurrentAudioDevice() returned nullptr\n");
    }

    std::fprintf (stdout, "=== End of Tone Test ===\n");
    std::fflush (stdout);
}

static void runHeadlessSelfTest()
{
    // Heap-allocated so destruction order matches the GUI path: AudioEngine
    // first, then Session, before this function returns and FocalApp::quit()
    // tears down the message loop.
    auto session = std::make_unique<Session>();
    auto engine  = std::make_unique<AudioEngine> (*session);

    // Poll for engine readiness instead of a fixed sleep. AudioEngine's
    // constructor calls initialiseWithDefaultDevices(16, 2) and adds itself
    // as an AudioDeviceCallback; the audio thread then fires
    // audioDeviceAboutToStart, which sets currentSampleRate to a non-zero
    // value as a side effect of preparing strip/aux/master state. So
    // sampleRate > 0 is the load-bearing readiness signal.
    //
    // 5-second timeout so a stuck-or-failing device-open can't hang the
    // headless test indefinitely (relevant on CI / slow boxes / contended
    // PipeWire setups). If we time out, we still proceed - the synthetic
    // tests don't strictly require a real device since they call
    // prepareForSelfTest() with their own SR/BS.
    constexpr int maxWaitMs       = 5000;
    constexpr int pollIntervalMs  = 10;
    int waited = 0;
    while (engine->getCurrentSampleRate() <= 0.0 && waited < maxWaitMs)
    {
        juce::Thread::sleep (pollIntervalMs);
        waited += pollIntervalMs;
    }
    if (engine->getCurrentSampleRate() <= 0.0)
        std::fprintf (stderr,
                      "[Focal/selftest] WARNING: audio engine not ready after %d ms - "
                      "synthetic tests will still run, backend tests may show degraded info\n",
                      maxWaitMs);

    AudioPipelineSelfTest test (*engine, engine->getDeviceManager(), *session);
    const auto report = test.runAll();

    std::fprintf (stdout, "%s\n", report.toRawUTF8());
    std::fflush (stdout);
}

void FocalApp::initialise (const juce::String&)
{
   #if JUCE_LINUX
    primeRealtimeAudio();
   #endif

    if (envFlagSet ("FOCAL_RUN_SELFTEST"))
    {
        runHeadlessSelfTest();
        quit();
        return;
    }

    if (envFlagSet ("FOCAL_RUN_TONE_TEST"))
    {
        runHeadlessToneTest();
        quit();
        return;
    }

   #if defined(__linux__)
    if (envFlagSet ("FOCAL_RUN_ALSA_PERF"))
    {
        // Tier 1 ALSA backend perf test. Drives the backend directly (no
        // AudioDeviceManager involvement, no engine), output-only, silent.
        // Configurable via env vars - device picks default of hw:0,0 if
        // none set.
        focal::AlsaPerformanceTest::Options opts;
        if (const auto* v = std::getenv ("FOCAL_ALSA_PERF_DEVICE"))      opts.deviceId      = v;
        if (const auto* v = std::getenv ("FOCAL_ALSA_PERF_RATE"))        opts.sampleRate    = (unsigned int) juce::String (v).getIntValue();
        if (const auto* v = std::getenv ("FOCAL_ALSA_PERF_DURATION_MS")) opts.durationMs    = juce::String (v).getIntValue();
        if (const auto* v = std::getenv ("FOCAL_ALSA_PERF_LOAD_US"))     opts.fakeDspLoadUs = juce::String (v).getIntValue();
        if (const auto* v = std::getenv ("FOCAL_ALSA_PERF_LOOPBACK"))    opts.runLoopback   = juce::String (v).getIntValue() != 0;

        // Tier 2: comma-separated list, e.g. "44100,48000,96000". Empty
        // (or unset) keeps the single-rate Tier 1 behaviour.
        if (const auto* v = std::getenv ("FOCAL_ALSA_PERF_RATES"))
        {
            const auto tokens = juce::StringArray::fromTokens (juce::String (v), ",", "");
            for (const auto& t : tokens)
            {
                const int rate = t.trim().getIntValue();
                if (rate > 0) opts.sampleRates.add ((unsigned int) rate);
            }
        }

        const auto report = focal::AlsaPerformanceTest::runAll (opts);
        std::fprintf (stdout, "%s\n", report.toRawUTF8());
        std::fflush (stdout);

        quit();
        return;
    }
   #endif

    if (envFlagSet ("FOCAL_RUN_PERF_TEST"))
    {
        // Headless engine-CPU benchmark. Builds a Session+AudioEngine the
        // same way the GUI path does, then drives many callbacks directly
        // and reports per-callback wall-clock vs. buffer budget across a
        // matrix of (sample rate, buffer size, channel load) configs.
        auto session = std::make_unique<Session>();
        auto engine  = std::make_unique<AudioEngine> (*session);

        // Wait briefly for device init so engine's DSP graph is warm.
        for (int waited = 0;
             engine->getCurrentSampleRate() <= 0.0 && waited < 5000;
             waited += 10)
            juce::Thread::sleep (10);

        AudioPipelineSelfTest test (*engine, engine->getDeviceManager(), *session);
        const auto report = test.runPerfSuite();
        std::fprintf (stdout, "%s\n", report.toRawUTF8());
        std::fflush (stdout);

        quit();
        return;
    }

    // User UI-scale override. JUCE composes this with each display's own
    // OS-reported DPI scale, so 1.0 here means "let the OS decide" and
    // anything else is the user's manual zoom. Applied BEFORE creating
    // the main window so its initial layout uses the right metrics.
    juce::Desktop::getInstance().setGlobalScaleFactor (appconfig::getUiScaleOverride());

    mainWindow = std::make_unique<MainWindow> (getApplicationName());
}

void FocalApp::shutdown()
{
    // Persist window geometry before tearing down the window. Reading
    // getWindowStateAsString() AFTER mainWindow.reset() would crash; doing
    // it here captures the user's last visible position/size/fullscreen.
    if (mainWindow != nullptr)
        WindowState::save (mainWindow->getWindowStateAsString());

    // Dismiss any still-open modal dialogs (e.g. the Audio Device selector)
    // BEFORE destroying mainWindow. The selector's `AudioDeviceSelectorComponent`
    // is registered as a change-listener on `AudioEngine::deviceManager`. If we
    // skip this, `mainWindow.reset()` destroys MainComponent → AudioEngine →
    // AudioDeviceManager, then ScopedJuceInitialiser_GUI's destructor (which
    // runs AFTER us, in JUCEApplicationBase::main) destroys ModalComponentManager,
    // which finally destroys the dialog - its destructor calls removeChangeListener
    // on the freed AudioDeviceManager → SIGSEGV.
    //
    // The AudioSettingsPanel modal dialog is freed inside MainComponent's
    // destructor (via a tracked Component::SafePointer) so the
    // AudioDeviceSelectorComponent's listener-removal happens while
    // AudioDeviceManager is still alive. cancelAllModalComponents() here
    // is unhelpful - it only marks dialogs inactive and queues an async
    // delete that never fires before main() returns.
    mainWindow.reset();
}

void FocalApp::systemRequestedQuit()
{
    quit();
}

void FocalApp::anotherInstanceStarted (const juce::String&) {}
} // namespace focal
