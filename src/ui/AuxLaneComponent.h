#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../session/Session.h"

namespace focal
{
class PluginSlot;
class AuxLaneStrip;

// One AUX return lane. Mute, return-level fader, name + colour, plus
// AuxLaneParams::kMaxLanePlugins plugin slots stacked vertically. Each slot
// embeds the loaded plugin's full juce::AudioProcessorEditor inline (no
// popup) - users see the plugin's own UI directly in the lane, which is why
// we cap slot count at 1-2.
class AuxLaneComponent final : public juce::Component, private juce::Timer
{
public:
    AuxLaneComponent (AuxLane& laneRef, AuxLaneStrip& strip, int laneIndex);
    ~AuxLaneComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kMinLaneWidth = 220;
    static constexpr int kHeaderHeight = 60;     // name + return level
    static constexpr int kSlotHeaderH  = 24;     // bypass + remove + name strip

private:
    void timerCallback() override;
    void rebuildSlots();
    void openPickerForSlot (int slotIdx);
    void unloadSlot (int slotIdx);
    void toggleEditorForSlot (int slotIdx);
    void refreshSlotControls (int slotIdx);
    void togglePopoutForSlot (int slotIdx);
    void closePopoutForSlot (int slotIdx);
    void attachEditorInline (int slotIdx);

    AuxLane& lane;
    AuxLaneStrip& strip;
    int laneIndex;

    juce::Label   nameLabel;
    juce::Slider  returnFader { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton muteButton { "M" };

    struct SlotUI
    {
        juce::TextButton openOrAddButton;     // "+ Plugin" when empty; plugin name + click-to-toggle-editor when loaded
        juce::TextButton bypassButton;
        juce::TextButton removeButton;
        juce::TextButton popoutButton;        // ↗ - opens the editor in a separate floating window

        // Editor is parented directly to AuxLaneComponent. We tried wrapping
        // it in a juce::Viewport for inline scrolling but that broke LV2
        // (Suil-hosted X11 child windows lose their visual parent across
        // Viewport contentHolder reparenting → black editor) and didn't
        // help most commercial plugins anyway (mouse-wheel events get
        // consumed by the plugin's own controls). Plugins that don't fit
        // get clipped at the slot bounds; users can pop the editor out
        // into a floating window for the full UI.
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        juce::String displayedName;
        int editorNativeW = 0;
        int editorNativeH = 0;

        // When non-null, the editor lives inside this floating window
        // instead of being a child of the lane. Closing the window snaps
        // the editor back inline.
        //
        // Uses a unique_ptr to a custom DocumentWindow subclass instead
        // of juce::DialogWindow::launchAsync's auto-deleting raw pointer:
        // the auto-delete + manual delete race was racing the windowing
        // system's destroy/unmap ordering and on Linux/Wayland (Mutter)
        // could take down the desktop session. The custom subclass calls
        // back to AuxLaneComponent when the user clicks the X button so
        // every close path converges on a single deferred destruction.
        class AuxPopoutWindow;
        std::unique_ptr<AuxPopoutWindow> popoutWindow;
    };
    std::array<SlotUI, AuxLaneParams::kMaxLanePlugins> slots;
    std::unique_ptr<juce::FileChooser> activePluginChooser;
};
} // namespace focal
