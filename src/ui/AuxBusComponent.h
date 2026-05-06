#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "../session/Session.h"

namespace focal
{
class PluginSlot;

class AuxBusComponent final : public juce::Component, private juce::Timer
{
public:
    AuxBusComponent (AuxBus& auxRef, class Session& sessionRef, int auxIndex,
                      PluginSlot& slotRef);
    ~AuxBusComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    void timerCallback() override;
    void showColourMenu();
    void applyAuxColour (juce::Colour c);
    void refreshFxButton();
    void showFxButtonMenu();      // right-click menu (replace / remove / re-enable)

    AuxBus& aux;
    Session& sessionRef;
    int auxIndex;
    PluginSlot& pluginSlot;
    juce::Label nameLabel;
    juce::Rectangle<int> fxArea;     // bounds of the plugin-slot FX button

    // FX plugin slot button. Mirrors the channel strip's PluginSlotButton:
    // left-click opens the picker; right-click opens a context menu for
    // unload / replace / re-enable. Right-click handling is inline so we
    // don't need a separate subclass header.
    struct FxButton final : public juce::TextButton
    {
        using juce::TextButton::TextButton;
        std::function<void(const juce::MouseEvent&)> onRightClick;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onRightClick) { onRightClick (e); return; }
            juce::TextButton::mouseDown (e);
        }
    };
    FxButton fxButton { "+ FX" };
    juce::String lastSlotName;
    std::unique_ptr<juce::FileChooser> activeFxChooser;

    // 3-band EQ controls (LF / MID / HF gains, fixed musical frequencies).
    juce::TextButton eqButton  { "EQ" };
    juce::Slider     eqLfGain  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqMidGain { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfGain  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfLbl, eqMidLbl, eqHfLbl;

    // Bus compressor controls.
    juce::TextButton compButton { "COMP" };
    juce::Slider     compThresh  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRatio   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compAttack  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRelease { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compMakeup  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      compThrLbl, compRatLbl, compAtkLbl, compRelLbl, compMakLbl;

    // Pan knob.
    juce::Slider panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label  panLbl;

    juce::Slider     faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };

    // Stereo output meter (L | R) on the right side of the fader, matching
    // the master strip's layout. Smoothed and peak-hold values per channel.
    // Plus a slim vertical GR bar (top-down fill, gold→red) so the user
    // sees compressor activity at a glance, not just as a numeric readout.
    juce::Rectangle<int> meterArea;
    juce::Rectangle<int> grMeterArea;
    juce::Rectangle<int> faderScaleArea;
    juce::Label outputPeakLabel;
    juce::Label grPeakLabel;
    float displayedOutputLDb = -100.0f;
    float displayedOutputRDb = -100.0f;
    float displayedGrDb      = 0.0f;
    float outputPeakHoldLDb  = -100.0f;
    float outputPeakHoldRDb  = -100.0f;
    int   outputPeakHoldFramesL = 0;
    int   outputPeakHoldFramesR = 0;
};
} // namespace focal
