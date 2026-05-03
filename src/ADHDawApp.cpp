#include "ADHDawApp.h"
#include "ui/MainComponent.h"

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
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

ADHDawApp::ADHDawApp() = default;
ADHDawApp::~ADHDawApp() = default;

void ADHDawApp::initialise (const juce::String&)
{
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
