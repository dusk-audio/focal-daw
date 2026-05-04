#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../session/Session.h"

namespace adhdaw
{
class AuxBusComponent final : public juce::Component
{
public:
    explicit AuxBusComponent (AuxBus& auxRef);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    void showColourMenu();
    void applyAuxColour (juce::Colour c);

    AuxBus& aux;
    juce::Label nameLabel;
    juce::Rectangle<int> fxArea;     // future: plugin slot for this aux's FX
    juce::Rectangle<int> eqArea;     // future: bus EQ
    juce::Rectangle<int> compArea;   // future: bus compressor
    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };
};
} // namespace adhdaw
