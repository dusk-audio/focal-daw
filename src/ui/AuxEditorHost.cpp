#include "AuxEditorHost.h"
#include "PlatformWindowing.h"
#include "../engine/AudioEngine.h"

namespace focal
{
AuxEditorHost::AuxEditorHost (const juce::String& title,
                                juce::Component& editor,
                                juce::AudioProcessor* processorForResizability,
                                AudioEngine* engineForTransport)
    : juce::DocumentWindow (([&]
                              {
                                  // X11-route the toplevel peer so VST3 /
                                  // LV2 X11 sub-windows have an X11 host
                                  // to reparent into. Same comma-expression
                                  // pattern PluginEditorWindow uses.
                                  focal::platform::preferX11ForNextNativeWindow();
                                  return title;
                              })(),
                              juce::Colour (0xff202024),
                              /*requiredButtons*/ 0,
                              /*addToDesktop*/ true),
      trackedEditor (&editor),
      processor (processorForResizability),
      enginePtr (engineForTransport)
{
    setUsingNativeTitleBar (false);
    setTitleBarHeight (0);
    setDropShadowEnabled (false);
    setResizable (false, false);
    setWantsKeyboardFocus (false);

    setContentNonOwned (&editor, /*resizeToFitContent*/ true);

    // Bring above the main window so the user sees the editor immediately.
    juce::Component::SafePointer<AuxEditorHost> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (auto* self = safeThis.getComponent())
            if (auto* peer = self->getPeer())
                focal::platform::bringWindowToFront (*peer);
    });

    // LV2 + some VST3 editors finalize their preferred size on a later
    // idle pump; the size known at construction is stale. Same staged
    // re-fit cadence PluginEditorWindow uses for the channel-strip path.
    for (int delayMs : { 100, 350, 800 })
    {
        juce::Component::SafePointer<AuxEditorHost> refit (this);
        juce::Timer::callAfterDelay (delayMs, [refit]
        {
            if (auto* self = refit.getComponent())
                self->applyEditorLayout();
        });
    }

    focal::platform::clearPreferX11ForNativeWindow();
}

AuxEditorHost::~AuxEditorHost()
{
    setContentNonOwned (nullptr, false);
}

bool AuxEditorHost::keyPressed (const juce::KeyPress& k)
{
    if (enginePtr == nullptr) return false;
    if (! k.getModifiers().isAnyModifierKeyDown()
        && k == juce::KeyPress::spaceKey)
    {
        auto& transport = enginePtr->getTransport();
        if (transport.isStopped()) enginePtr->play();
        else                       enginePtr->stop();
        return true;
    }
    return false;
}

void AuxEditorHost::setLaneScreenBounds (juce::Rectangle<int> screenRect)
{
    if (screenRect.isEmpty()) return;
    if (screenRect == lastBounds) return;
    lastBounds = screenRect;
    setBounds (screenRect);
    applyEditorLayout();
}

void AuxEditorHost::setHostHidden (bool hidden)
{
    if (hidden == ! isVisible()) return;
    setVisible (! hidden);
}

void AuxEditorHost::resized()
{
    juce::DocumentWindow::resized();
    applyEditorLayout();
}

void AuxEditorHost::applyEditorLayout()
{
    auto* content = getContentComponent();
    if (content == nullptr) return;
    const int hostW = content->getWidth();
    const int hostH = content->getHeight();
    if (hostW <= 0 || hostH <= 0) return;

    if (auto* ape = dynamic_cast<juce::AudioProcessorEditor*> (trackedEditor))
    {
        if (ape->isResizable())
        {
            // Resizable plugin: stretch to fill the lane.
            if (ape->getWidth() != hostW || ape->getHeight() != hostH)
                ape->setSize (hostW, hostH);
        }
        else
        {
            // Fixed-layout plugin: keep at native, centre within host.
            // Overflow clips at the host frame.
            const int x = juce::jmax (0, (hostW - ape->getWidth())  / 2);
            const int y = juce::jmax (0, (hostH - ape->getHeight()) / 2);
            ape->setTopLeftPosition (x, y);
        }
    }
}
} // namespace focal
