#include "AuxLaneComponent.h"
#include "AuxEditorHost.h"
#include "PlatformWindowing.h"
#include "PluginPickerHelpers.h"
#include "../dsp/AuxLaneStrip.h"
#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include "../engine/Transport.h"

namespace focal
{
AuxLaneComponent::AuxLaneComponent (AuxLane& l, AuxLaneStrip& s, int idx,
                                       AudioEngine& e)
    : lane (l), strip (s), engine (e), laneIndex (idx)
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

    }

    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        refreshSlotControls (i);
    rebuildSlots();
    startTimerHz (4);   // poll loaded-name + auto-bypass state for the slot UI
}

AuxLaneComponent::~AuxLaneComponent()
{
    // Tear down editor hosts first so their non-owning content pointers
    // are detached before the editor unique_ptrs delete the editors.
    // prepareForTopLevelDestruction handles the Wayland focus_window
    // hand-off so mutter doesn't trip meta_window_unmanage on the X11
    // host destroy.
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
        destroyEditorHostForSlot (i);
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
    }
    else
    {
        ui.displayedName.clear();
        ui.openOrAddButton.setButtonText ("+ Plugin");
        ui.bypassButton.setVisible (false);
        ui.removeButton.setVisible (false);
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
    destroyEditorHostForSlot (slotIdx);
    auto& ui = slots[(size_t) slotIdx];
    ui.editor.reset();   // delete editor BEFORE the processor goes away
    strip.getPluginSlot (slotIdx).unload();
    refreshSlotControls (slotIdx);
    rebuildSlots();
}

void AuxLaneComponent::toggleEditorForSlot (int slotIdx)
{
    // The host is always alive when the plugin is loaded (see
    // rebuildSlots). Clicking the slot button after-the-fact toggles
    // visibility of the host rather than building / tearing it down.
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editorHost != nullptr)
        ui.editorHost->setHostHidden (ui.editorHost->isVisible());
    rebuildSlots();
}

void AuxLaneComponent::createEditorHostForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editor == nullptr || ui.editorHost != nullptr) return;

    auto* instance = strip.getPluginSlot (slotIdx).getInstance();
    ui.editorHost = std::make_unique<AuxEditorHost> (
        lane.name + " - " + ui.editor->getName(),
        *ui.editor,
        instance,
        &engine);

    const auto target = computeSlotScreenRect (slotIdx);
    if (! target.isEmpty())
        ui.editorHost->setLaneScreenBounds (target);
}

void AuxLaneComponent::destroyEditorHostForSlot (int slotIdx)
{
    auto& ui = slots[(size_t) slotIdx];
    if (ui.editorHost == nullptr) return;
    focal::platform::prepareForTopLevelDestruction (*ui.editorHost);
    ui.editorHost.reset();
}

juce::Rectangle<int> AuxLaneComponent::computeSlotScreenRect (int slotIdx) const
{
    if (slotIdx < 0 || slotIdx >= (int) slots.size()) return {};
    if (! strip.getPluginSlot (slotIdx).isLoaded()) return {};

    auto area = getLocalBounds().reduced (6);
    area.removeFromTop (6);
    area.removeFromTop (kHeaderHeight);
    area.removeFromTop (8);
    area.removeFromTop (kSlotHeaderH);
    area.removeFromTop (4);
    if (area.isEmpty()) return {};
    // localAreaToGlobal walks up the Component tree and returns screen
    // coordinates the X11 toplevel can use directly.
    return localAreaToGlobal (area);
}

void AuxLaneComponent::repositionEditorHosts()
{
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        auto& ui = slots[(size_t) i];
        if (ui.editorHost == nullptr) continue;
        const auto target = computeSlotScreenRect (i);
        if (! target.isEmpty())
            ui.editorHost->setLaneScreenBounds (target);
    }
}

void AuxLaneComponent::setEditorHostsHidden (bool hidden)
{
    for (auto& ui : slots)
        if (ui.editorHost != nullptr)
            ui.editorHost->setHostHidden (hidden);
}

void AuxLaneComponent::closeAllPopoutsForShutdown()
{
    // Destroy every editor host through the X-focus-safe path so mutter
    // gets a FocusOut before the X11 peer goes away.
    for (int i = 0; i < (int) slots.size(); ++i)
        destroyEditorHostForSlot (i);
}

void AuxLaneComponent::rebuildSlots()
{
    // For each slot whose plugin is loaded, ensure the editor + its
    // host exist so the lane shows the plugin's UI without an extra
    // click. New session loads restore a plugin via
    // PluginSlot::restoreFromSavedState; the timer refresh sees
    // isLoaded() flip and we materialise the editor + host here.
    for (int i = 0; i < AuxLaneParams::kMaxLanePlugins; ++i)
    {
        auto& ui = slots[(size_t) i];
        auto* instance = strip.getPluginSlot (i).getInstance();
        if (instance != nullptr && ui.editor == nullptr)
        {
            // X11 latch around createEditorIfNeeded: LV2 plugin editors
            // eager-create an embedded X11 sub-window in their Editor
            // ctor and need an X11 peer. Latch covers the nested
            // addToDesktop. No-op for VST3 / non-LV2 paths.
            focal::platform::preferX11ForNextNativeWindow();
            ui.editor.reset (instance->createEditorIfNeeded());
            focal::platform::clearPreferX11ForNativeWindow();
        }
        if (instance != nullptr && ui.editor != nullptr && ui.editorHost == nullptr)
            createEditorHostForSlot (i);
        if (instance == nullptr && (ui.editor != nullptr || ui.editorHost != nullptr))
        {
            destroyEditorHostForSlot (i);
            ui.editor.reset();
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
        // Header strip: plugin name (clickable) + BYP + remove. The
        // editor itself lives in an AuxEditorHost top-level positioned
        // over the slot rect via repositionEditorHosts().
        auto headerStrip = area.removeFromTop (kSlotHeaderH);
        ui.removeButton.setBounds (headerStrip.removeFromRight (28));
        headerStrip.removeFromRight (4);
        ui.bypassButton.setBounds (headerStrip.removeFromRight (44));
        headerStrip.removeFromRight (4);
        ui.openOrAddButton.setBounds (headerStrip);
        area.removeFromTop (4);
    }
    else
    {
        // Empty slot: the placeholder button fills the whole body so it
        // reads as a clear "drop zone" with a generous click target.
        ui.openOrAddButton.setBounds (area);
    }

    // Lane layout changed - push the new screen-rect to any live editor
    // hosts so they track the resize.
    repositionEditorHosts();
}
} // namespace focal
