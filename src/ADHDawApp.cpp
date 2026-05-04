#include "ADHDawApp.h"
#include "ui/ConsoleView.h"
#include "ui/MainComponent.h"

#if JUCE_LINUX
 #include <sys/mman.h>
 #include <sys/resource.h>
#endif

namespace adhdaw
{
class ADHDawApp::MainWindow final : public juce::DocumentWindow
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
        setResizeLimits (ConsoleView::minimumContentWidth() + 24, 880, 32768, 32768);
        centreWithSize (getWidth(), getHeight());

        // Some tiling/Wayland WMs auto-maximize new windows. Explicitly opt
        // out of full-screen so we open at the size MainComponent requested.
        setFullScreen (false);

        setVisible (true);
    }

    void closeButtonPressed() override
    {
        JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

ADHDawApp::ADHDawApp() = default;
ADHDawApp::~ADHDawApp() = default;

#if JUCE_LINUX
static void primeRealtimeAudio()
{
    // Pin every page of the process in physical RAM so the audio thread
    // never blocks on a page fault during a callback. Ardour, Bitwig, and
    // every other low-latency Linux DAW does this. Requires `memlock` rlimit
    // — typically `unlimited` for the audio group via /etc/security/limits.d.
    if (mlockall (MCL_CURRENT | MCL_FUTURE) != 0)
    {
        DBG ("mlockall failed (errno=" << errno
             << ") — audio thread may suffer page-fault stalls under memory pressure");
    }
}
#endif

void ADHDawApp::initialise (const juce::String&)
{
   #if JUCE_LINUX
    primeRealtimeAudio();
   #endif
    mainWindow = std::make_unique<MainWindow> (getApplicationName());
}

void ADHDawApp::shutdown()
{
    mainWindow.reset();
}

void ADHDawApp::systemRequestedQuit()
{
    quit();
}

void ADHDawApp::anotherInstanceStarted (const juce::String&) {}
} // namespace adhdaw
