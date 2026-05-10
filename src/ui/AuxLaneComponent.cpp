#include "AuxLaneComponent.h"
#include "PlatformWindowing.h"
#include "PluginPickerHelpers.h"
#include "../dsp/AuxLaneStrip.h"
#include "../engine/PluginSlot.h"

namespace focal
{
AuxLaneComponent::AuxLaneComponent (AuxLane& l, AuxLaneStrip& s, int idx)
    : lane (l), strip (s), laneIndex (idx)
{
    nameLabel.setText (lane.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centredLeft);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    nameLabel.setEditable (false, true, false);
    nameLabel.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff202024));
    nameLabel.setColour (juce::Label::textWhenEditingColourId,       juce::Colours::white);
    nameLabel.onTextChange = [this]
    {
        const auto txt = nameLabel.getText().trim();
        if (txt.isEmpty()) nameLabel.setText (lane.name, juce::dontSendNotification);
        else lane.name = txt;
    };
    addAndMakeVisible (nameLabel);

    returnFader.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    returnFader.setSkewFactorFromMidPoint (-12.0);
    returnFader.setValue (lane.params.returnLevelDb.load (std::memory_order_relaxed),
                            juce::dontSendNotification);
    returnFader.setDoubleClickReturnValue (true, 0.0);
    returnFader.setTextValueSuffix (" dB");
    returnFader.onValueChange = [this]
    {
        lane.params.returnLevelDb.store ((float) returnFader.getValue(),
                                           std::memory_order_relaxed);
    };
    addAndMakeVisible (returnFader);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (lane.params.mute.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    muteButton.setTooltip ("Mute this AUX return lane");
    muteButton.onClick = [this]
    {
        lane.params.mute.store (muteButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (muteButton);

    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
    {
        auto& s = slots[(size_t) i];

        s.openOrAddButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff222226));
        s.openOrAddButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff5a4880));
        s.openOrAddButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff9080c0));
        s.openOrAddButton.setColour (juce::TextButton::textColourOnId,  juce::Colours::white);
        s.openOrAddButton.onClick = [this, i]
        {
            auto& slotRef = strip.getPluginSlot (i);
            if (slotRef.isLoaded())
            {
                toggleEditorForSlot (i);
            }
            else
            {
                openPickerForSlot (i);
            }
        };
        addAndMakeVisible (s.openOrAddButton);

        s.bypassButton.setButtonText ("BYP");
        s.bypassButton.setClickingTogglesState (true);
        s.bypassButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd0a060));
        s.bypassButton.setTooltip ("Bypass this plugin slot");
        s.bypassButton.onClick = [this, i]
        {
            auto& slotRef = strip.getPluginSlot (i);
            auto& uiRef   = slots[(size_t) i];
            slotRef.setBypassed (uiRef.bypassButton.getToggleState());
            if (slotRef.wasAutoBypassed()) slotRef.clearAutoBypass();
            refreshSlotControls (i);
        };
        addChildComponent (s.bypassButton);

        s.removeButton.setButtonText ("X");
        s.removeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff402020));
        s.removeButton.setTooltip ("Remove this plugin");
        s.removeButton.onClick = [this, i] { unloadSlot (i); };
        addChildComponent (s.removeButton);

        // Pop-out button. Opens the loaded editor in a resizable floating
        // window at native size - useful for plugins that don't fit in
        // the slot (Helix Native, Kontakt, etc) and for plugin formats
        // whose UI doesn't render reliably as an inline child (LV2 via
        // Suil). Clicking again snaps it back inline.
        s.popoutButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x86\x97"));   // ↗
        s.popoutButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff282834));
        s.popoutButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff9080c0));
        s.popoutButton.setTooltip ("Open the plugin editor in a floating window");
        s.popoutButton.onClick = [this, i] { togglePopoutForSlot (i); };
        addChildComponent (s.popoutButton);
    }

    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        refreshSlotControls (i);
    rebuildSlots();
    startTimerHz (4);   // poll loaded-name + auto-bypass state for the slot UI
}

AuxLaneComponent::~AuxLaneComponent()
{
    // Close any popout windows first so their content (the editor) is
    // detached before our unique_ptr deletes it. The pop-out window holds
    // a non-owning pointer to the editor; if it's still alive when the
    // editor is freed, its closeButtonPressed callback could dereference
    // a freed pointer.
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        closePopoutForSlot (i);
    for (auto& s : slots)
        s.editor.reset();
}

void AuxLaneComponent::timerCallback()
{
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        refreshSlotControls (i);
}

void AuxLaneComponent::refreshSlotControls (int i)
{
    auto& slotRef = strip.getPluginSlot (i);
    auto& ui      = slots[(size_t) i];

    if (slotRef.isLoaded())
    {
        const auto name = slotRef.getLoadedName();
        if (name != ui.displayedName)
        {
            ui.displayedName = name;
            ui.openOrAddButton.setButtonText (name);
        }
        // Auto-bypass / crash = loud failure modes; tag with distinct copy
        // so the user can tell whether to retry (stalled) vs reload
        // (crashed; the OOP child died and won't recover by itself).
        if (slotRef.wasCrashed())
            ui.openOrAddButton.setButtonText ("! " + name + " (crashed)");
        else if (slotRef.wasAutoBypassed())
            ui.openOrAddButton.setButtonText ("! " + name + " (stalled)");
        ui.bypassButton.setVisible (true);
        ui.bypassButton.setToggleState (slotRef.isBypassed(), juce::dontSendNotification);
        ui.removeButton.setVisible (true);
        ui.popoutButton.setVisible (true);
        // Detect a popout window that was closed by the user clicking
        // its close button. The SafePointer goes null when the dialog is
        // deleted - on the next refresh we re-inline the editor.
        if (ui.popoutWindow == nullptr && ui.editor != nullptr
            && ui.editor->getParentComponent() != this)
        {
            attachEditorInline (i);
            resized();
        }
    }
    else
    {
        ui.displayedName.clear();
        ui.openOrAddButton.setButtonText ("+ Plugin");
        ui.bypassButton.setVisible (false);
        ui.removeButton.setVisible (false);
        ui.popoutButton.setVisible (false);
    }
}

void AuxLaneComponent::openPickerForSlot (int slotIdx)
{
    // The empty-slot placeholder button fills the entire AUX body, so
    // anchoring the menu at the button's top-left would land it up at
    // the AUX selector row. Use the cursor's current screen position
    // instead so the menu lands at the click.
    const auto cursor = juce::Desktop::getMousePosition();
    pluginpicker::openPickerMenu (strip.getPluginSlot (slotIdx),
                                    slots[(size_t) slotIdx].openOrAddButton,
                                    activePluginChooser,
                                    [this, slotIdx]
                                    {
                                        refreshSlotControls (slotIdx);
                                        rebuildSlots();
                                    },
                                    pluginpicker::PluginKind::Effects,
                                    cursor);
}

void AuxLaneComponent::unloadSlot (int slotIdx)
{
    closePopoutForSlot (slotIdx);
    auto& ui = slots[(size_t) slotIdx];
    ui.editor.reset();   // delete editor BEFORE the processor goes away
    ui.editorNativeW = ui.editorNativeH = 0;
    strip.getPluginSlot (slotIdx).unload();
    refreshSlotControls (slotIdx);
    rebuildSlots();
}

void AuxLaneComponent::toggleEditorForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editor != nullptr)
    {
        closePopoutForSlot (slotIdx);
        ui.editor.reset();
        ui.editorNativeW = ui.editorNativeH = 0;
    }
    else
    {
        if (auto* p = strip.getPluginSlot (slotIdx).getInstance())
        {
            ui.editor.reset (p->createEditorIfNeeded());
            if (ui.editor != nullptr)
            {
                ui.editorNativeW = juce::jmax (1, ui.editor->getWidth());
                ui.editorNativeH = juce::jmax (1, ui.editor->getHeight());
                attachEditorInline (slotIdx);
            }
        }
    }
    rebuildSlots();
}

void AuxLaneComponent::attachEditorInline (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editor == nullptr) return;
    addAndMakeVisible (ui.editor.get());
    ui.editor->setTransform ({});
}

// Custom popout window. Owned by AuxLaneComponent::SlotUI as a
// unique_ptr (NOT juce::DialogWindow::launchAsync, which auto-deletes
// itself on user X-close and races our manual delete - that ordering
// race against the windowing system was a reliable trigger for
// compositor faults on Linux/Wayland). When the user clicks the OS
// X button, closeButtonPressed forwards back to the host so it can
// detach the editor and drop the unique_ptr on the next message-loop
// tick (never destruct from inside a button-press callback).
class AuxLaneComponent::SlotUI::AuxPopoutWindow final : public juce::DocumentWindow
{
public:
    AuxPopoutWindow (const juce::String& title,
                     juce::AudioProcessorEditor& editor,
                     std::function<void()> onCloseButton)
        : juce::DocumentWindow (([&]
                                  {
                                      focal::platform::preferX11ForNextNativeWindow();
                                      return title;
                                  })(),
                                  juce::Colour (0xff202024),
                                  juce::DocumentWindow::closeButton,
                                  /*addToDesktop*/ true),
          onClose (std::move (onCloseButton))
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&editor, /*resizeToFit*/ true);
        setResizable (true, true);
        centreAroundComponent (nullptr, getWidth(), getHeight());
        setVisible (true);
        focal::platform::clearPreferX11ForNativeWindow();
    }

    void closeButtonPressed() override
    {
        // Detach content so this window's destructor doesn't touch the
        // shared editor; ask the host to drop us deferred.
        setContentNonOwned (nullptr, false);
        if (onClose) onClose();
    }

private:
    std::function<void()> onClose;
};

void AuxLaneComponent::togglePopoutForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editor == nullptr) return;

    if (ui.popoutWindow != nullptr)
    {
        // Already popped out - close + re-inline.
        closePopoutForSlot (slotIdx);
        return;
    }

    // Move the editor out of the lane and into a fresh top-level window
    // at native size. The window is non-owning of the content so we keep
    // managing the editor's lifetime via the unique_ptr; closing the
    // window detaches the editor and re-inlines it.
    ui.editor->setTransform ({});
    ui.editor->setSize (juce::jmax (1, ui.editorNativeW),
                          juce::jmax (1, ui.editorNativeH));

    juce::Component::SafePointer<AuxLaneComponent> safeThis (this);
    ui.popoutWindow = std::make_unique<SlotUI::AuxPopoutWindow> (
        lane.name + " - " + ui.editor->getName(),
        *ui.editor,
        [safeThis, slotIdx]
        {
            // User clicked the OS X button. Defer the actual close /
            // re-inline by one message-loop tick so we don't destruct
            // the window from inside its own closeButtonPressed.
            juce::MessageManager::callAsync ([safeThis, slotIdx]
            {
                if (auto* self = safeThis.getComponent())
                    self->closePopoutForSlot (slotIdx);
            });
        });
    refreshSlotControls (slotIdx);
}

void AuxLaneComponent::closePopoutForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.popoutWindow == nullptr) return;

    // Detach the editor first so the window's destructor doesn't touch
    // it, then drop the window. unique_ptr.reset() runs ~DocumentWindow
    // synchronously, but with content already detached the destructor
    // only tears down the window's own peer - no race with the editor
    // and no double-delete.
    if (ui.editor != nullptr
        && ui.editor->getParentComponent() != nullptr)
    {
        ui.editor->getParentComponent()->removeChildComponent (ui.editor.get());
    }

    focal::platform::prepareForTopLevelDestruction (*ui.popoutWindow);

    // Defer destruction so mutter's compositor loop has a tick to
    // process the EWMH activate above before this xdg_toplevel is
    // unmapped - else meta_window_unmanage trips on focus_window
    // still pointing at this peer.
    juce::Component::SafePointer<AuxLaneComponent> safe (this);
    juce::MessageManager::callAsync ([safe, slotIdx]
    {
        if (auto* self = safe.getComponent())
            self->slots[(size_t) slotIdx].popoutWindow.reset();
    });

    if (ui.editor != nullptr)
        attachEditorInline (slotIdx);
    refreshSlotControls (slotIdx);
}

void AuxLaneComponent::closeAllPopoutsForShutdown()
{
    // Mirror of closePopoutForSlot's destruction half, but with no
    // re-inline step (we're shutting down - no parent left to host
    // the editor). Walk every slot, defocus + destroy each live
    // popout window; the XSetInputFocus inside the helper guarantees
    // mutter sees a FocusOut before the X11 peer goes away.
    for (auto& ui : slots)
    {
        if (ui.popoutWindow == nullptr) continue;
        if (ui.editor != nullptr
            && ui.editor->getParentComponent() != nullptr)
        {
            ui.editor->getParentComponent()->removeChildComponent (ui.editor.get());
        }
        focal::platform::prepareForTopLevelDestruction (*ui.popoutWindow);
        ui.popoutWindow.reset();
    }
}

void AuxLaneComponent::rebuildSlots()
{
    // For each slot whose plugin is loaded, ensure the editor exists so the
    // lane shows the plugin's UI without an extra click. New session loads
    // restore a plugin via PluginSlot::restoreFromSavedState; the timer
    // refresh sees isLoaded() flip and we materialise the editor here.
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
    {
        auto& ui = slots[(size_t) i];
        auto* instance = strip.getPluginSlot (i).getInstance();
        if (instance != nullptr && ui.editor == nullptr)
        {
            ui.editor.reset (instance->createEditorIfNeeded());
            if (ui.editor != nullptr)
            {
                ui.editorNativeW = juce::jmax (1, ui.editor->getWidth());
                ui.editorNativeH = juce::jmax (1, ui.editor->getHeight());
                attachEditorInline (i);
            }
        }
        if (instance == nullptr && ui.editor != nullptr)
        {
            closePopoutForSlot (i);
            ui.editor.reset();
            ui.editorNativeW = ui.editorNativeH = 0;
        }
    }
    resized();
}

void AuxLaneComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    g.setColour (juce::Colour (0xff181820));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (lane.colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xff2a2a2e));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.0f);

    // When the editor is popped out the slot body is empty - paint a hint
    // so the lane doesn't look broken.
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
    {
        auto const& ui = slots[(size_t) i];
        if (ui.popoutWindow == nullptr || ui.editor == nullptr) continue;
        // Approximate the slot-body area; painting outside leaks behind the
        // header buttons but they're opaque so it doesn't matter visually.
        auto body = getLocalBounds().reduced (8).withTrimmedTop (kHeaderHeight + kSlotHeaderH + 18);
        if (body.isEmpty()) continue;
        g.setColour (juce::Colour (0xff909094));
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::plain)));
        g.drawText ("Editor in floating window. Click " + juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x97"))
                      + " to bring it back inline.",
                     body, juce::Justification::centred, true);
    }
}

void AuxLaneComponent::resized()
{
    auto area = getLocalBounds().reduced (6);
    area.removeFromTop (6);

    // Header: name + mute + return-level fader.
    {
        auto header = area.removeFromTop (kHeaderHeight);
        auto top = header.removeFromTop (22);
        muteButton.setBounds (top.removeFromRight (28));
        top.removeFromRight (4);
        nameLabel.setBounds (top);

        header.removeFromTop (4);
        returnFader.setBounds (header);
    }
    area.removeFromTop (8);

    // Single plugin slot - takes the whole remaining lane body.
    auto& ui = slots[0];
    const bool loaded = strip.getPluginSlot (0).isLoaded();

    if (loaded)
    {
        // Header strip: plugin name (clickable) + popout + BYP + remove.
        auto headerStrip = area.removeFromTop (kSlotHeaderH);
        ui.removeButton.setBounds (headerStrip.removeFromRight (28));
        headerStrip.removeFromRight (4);
        ui.bypassButton.setBounds (headerStrip.removeFromRight (44));
        headerStrip.removeFromRight (4);
        ui.popoutButton.setBounds (headerStrip.removeFromRight (28));
        headerStrip.removeFromRight (4);
        ui.openOrAddButton.setBounds (headerStrip);
        area.removeFromTop (4);

        // Editor is a direct child of the lane (no Viewport). When popped
        // out, the editor lives in a separate floating window and the
        // slot area paints a hint instead.
        if (ui.editor != nullptr && ui.popoutWindow == nullptr)
        {
            // Render at the editor's native size and centre it within the
            // slot area. We don't try to resize the plugin to fit - that
            // squeezed plugins with fixed layouts and made resizable
            // plugins look stretched. If the editor overflows the slot,
            // it clips at the lane bounds and the user can use the
            // pop-out button (↗) to see the full UI in a floating window.
            const int natW = juce::jmax (1, ui.editorNativeW);
            const int natH = juce::jmax (1, ui.editorNativeH);
            const int xOff = juce::jmax (0, (area.getWidth()  - natW) / 2);
            const int yOff = juce::jmax (0, (area.getHeight() - natH) / 2);
            ui.editor->setTransform ({});
            ui.editor->setBounds (area.getX() + xOff, area.getY() + yOff,
                                    natW, natH);
        }
    }
    else
    {
        // Empty slot: the placeholder button fills the whole body so it
        // reads as a clear "drop zone" with a generous click target.
        ui.openOrAddButton.setBounds (area);
    }
}
} // namespace focal
