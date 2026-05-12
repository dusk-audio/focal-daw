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
class AudioEngine;
class AuxEditorHost;

// One AUX return lane. Mute, return-level fader, name + colour, plus
// AuxLaneParams::kMaxLanePlugins plugin slots. The slot's plugin editor
// is hosted by an AuxEditorHost - a borderless X11 toplevel positioned
// over the lane's slot rect so the user perceives it as embedded. The
// editor cannot be a true Component child of the lane on Wayland because
// X11 plugin windows can't reparent into a wl_surface.
class AuxLaneComponent final : public juce::Component, private juce::Timer
{
public:
    AuxLaneComponent (AuxLane& laneRef, AuxLaneStrip& strip, int laneIndex,
                       AudioEngine& engineRef);
    ~AuxLaneComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Push the current slot screen-rect to every loaded slot's
    // AuxEditorHost so the host tracks main-window movement and lane
    // re-layout. Called from MainComponent's movement watcher.
    void repositionEditorHosts();

    // Hide / show every loaded slot's AuxEditorHost without destroying
    // them. Used when AUX view is swapped out (MIXING / RECORDING /
    // MASTERING) or another AUX lane becomes active.
    void setEditorHostsHidden (bool hidden);

    // Tear down every editor host owned by this lane through the
    // X-focus-safe path. Called from AuxView::closeAllAuxPopouts at app
    // shutdown.
    void closeAllPopoutsForShutdown();

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
    void createEditorHostForSlot (int slotIdx);
    void destroyEditorHostForSlot (int slotIdx);
    juce::Rectangle<int> computeSlotScreenRect (int slotIdx) const;

    AuxLane& lane;
    AuxLaneStrip& strip;
    AudioEngine& engine;
    int laneIndex;

    juce::Label   nameLabel;
    juce::Slider  returnFader { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton muteButton { "M" };

    struct SlotUI
    {
        juce::TextButton openOrAddButton;     // "+ Plugin" when empty; plugin name + click-to-toggle when loaded
        juce::TextButton bypassButton;
        juce::TextButton removeButton;

        // Editor lives inside an AuxEditorHost (borderless X11 toplevel
        // tracked to the slot's screen rect), not as a Component child
        // of the lane. The unique_ptr keeps the editor alive across
        // load/replace cycles; AuxEditorHost holds a non-owning content
        // pointer.
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        std::unique_ptr<AuxEditorHost>              editorHost;
        juce::String displayedName;
    };
    std::array<SlotUI, AuxLaneParams::kMaxLanePlugins> slots;
    std::unique_ptr<juce::FileChooser> activePluginChooser;
};
} // namespace focal
