#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../session/Session.h"

namespace adhdaw
{
class MasterStripComponent final : public juce::Component
{
public:
    explicit MasterStripComponent (MasterBusParams& paramsRef);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    MasterBusParams& params;

    juce::Label nameLabel;

    juce::TextButton tapeButton { "TAPE" };  // master tape saturation on/off
    juce::TextButton hqButton   { "HQ" };

    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
};
} // namespace adhdaw
