#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../session/Session.h"

namespace adhdaw
{
// Overlay editor for a channel's 4-band EQ. Constructed each time the user
// clicks the strip's "EQ" button — controls bind to the same atomics on the
// Track, so values persist across open/close. Designed for juce::CallOutBox.
class ChannelEqEditor final : public juce::Component
{
public:
    explicit ChannelEqEditor (Track& trackRef);
    ~ChannelEqEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Track& track;

    juce::Label titleLabel;
    juce::TextButton typeButton { "E" };

    struct BandRow
    {
        juce::Label nameLabel;
        std::unique_ptr<juce::Slider> gain;
        std::unique_ptr<juce::Slider> freq;
    };
    std::array<BandRow, 4> rows;

    void refreshTypeButton();
};
} // namespace adhdaw
