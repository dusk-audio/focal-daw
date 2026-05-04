#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace adhdaw
{
// Console-style rotary knob look: dark grey body with a soft inner gradient
// plus a colored "cap" indicator that points to the current value, modelled
// on the SSL 4K / Harrison Mixbus knob aesthetic.
//
// Per-knob accent comes from the slider's `rotarySliderFillColourId` — set
// that to the band/section color and the cap takes it.
class ADHDawLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ADHDawLookAndFeel()
    {
        setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour (0xff4a7c9e));
        setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
        setColour (juce::Slider::thumbColourId,               juce::Colour (0xffe0e0e0));
        setColour (juce::Slider::trackColourId,               juce::Colour (0xff303034));
        setColour (juce::Slider::backgroundColourId,          juce::Colour (0xff1a1a1c));
    }

    // Console-style fader cap for vertical linear sliders. The fader rides on
    // a thin centered track; the thumb is a wide horizontal cap with a soft
    // gradient and grip lines, modelled on a real motorized fader handle.
    void drawLinearSlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style,
                           juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearBarVertical)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height,
                                                     sliderPos, minSliderPos, maxSliderPos,
                                                     style, slider);
            return;
        }

        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
        const float cx = bounds.getCentreX();

        // Track — thin vertical channel with a faint unity-gain mark.
        const float trackW = juce::jmin (4.0f, bounds.getWidth() * 0.18f);
        const auto trackRect = juce::Rectangle<float> (cx - trackW * 0.5f, bounds.getY() + 6.0f,
                                                        trackW, bounds.getHeight() - 12.0f);
        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillRoundedRectangle (trackRect, trackW * 0.5f);
        g.setColour (juce::Colour (0xff2a2a2e));
        g.drawRoundedRectangle (trackRect, trackW * 0.5f, 0.6f);

        // Subtle 0 dB / unity tick across the track.
        const auto range = slider.getNormalisableRange();
        const float zeroPos = (float) range.convertTo0to1 (0.0);
        const float zeroY = bounds.getBottom() - 6.0f - zeroPos * (bounds.getHeight() - 12.0f);
        g.setColour (juce::Colour (0x40ffffff));
        g.drawLine (trackRect.getX() - 4.0f, zeroY, trackRect.getRight() + 4.0f, zeroY, 0.8f);

        // Cap.
        const float capW = juce::jmin (bounds.getWidth() - 6.0f, 38.0f);
        const float capH = 22.0f;
        const auto cap = juce::Rectangle<float> (cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);

        // Cast shadow under the cap.
        g.setColour (juce::Colour (0x80000000));
        g.fillRoundedRectangle (cap.translated (0.0f, 2.0f), 3.0f);

        // Body gradient (top-bright, bottom-dark).
        juce::ColourGradient body (juce::Colour (0xffd0c8b0), cap.getX(), cap.getY(),
                                    juce::Colour (0xff3a3530), cap.getX(), cap.getBottom(), false);
        body.addColour (0.45, juce::Colour (0xff908878));
        body.addColour (0.55, juce::Colour (0xff605850));
        g.setGradientFill (body);
        g.fillRoundedRectangle (cap, 3.0f);

        // Bright top edge.
        g.setColour (juce::Colour (0x60ffffff));
        g.drawHorizontalLine ((int) cap.getY() + 1, cap.getX() + 2.0f, cap.getRight() - 2.0f);

        // Outer rim.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);

        // Center grip groove (where the engineer's finger sits).
        g.setColour (juce::Colour (0xff20201c));
        g.fillRoundedRectangle (juce::Rectangle<float> (cap.getX() + 4.0f, cap.getCentreY() - 1.0f,
                                                          cap.getWidth() - 8.0f, 2.0f), 1.0f);
        g.setColour (juce::Colour (0x30ffffff));
        g.drawHorizontalLine ((int) cap.getCentreY() - 2,
                               cap.getX() + 4.0f, cap.getRight() - 4.0f);
    }

    void drawRotarySlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        const float cx = x + width  * 0.5f;
        const float cy = y + height * 0.5f;
        const float radius = juce::jmin (width, height) * 0.5f - 2.0f;
        if (radius <= 2.0f) return;

        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const auto fill = slider.findColour (juce::Slider::rotarySliderFillColourId);

        // Soft drop shadow under the body.
        {
            juce::ColourGradient shadow (juce::Colour (0x60000000), cx, cy,
                                         juce::Colour (0x00000000), cx, cy + radius + 6.0f, true);
            g.setGradientFill (shadow);
            g.fillEllipse (cx - radius - 2.0f, cy - radius, (radius + 2.0f) * 2.0f, (radius + 2.0f) * 2.0f);
        }

        // Body — fully coloured in the slider's accent (SSL-style).
        // Radial gradient: top-left highlight, bottom-right shadow.
        {
            juce::ColourGradient body (fill.brighter (0.15f), cx - radius * 0.55f, cy - radius * 0.55f,
                                       fill.darker  (0.55f),  cx + radius * 0.55f, cy + radius * 0.55f, true);
            g.setGradientFill (body);
            g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        }

        // Subtle plastic-sheen highlight at the top.
        g.setColour (juce::Colour (0x20ffffff));
        g.fillEllipse (cx - radius * 0.85f, cy - radius * 0.95f, radius * 1.7f, radius * 0.7f);

        // Crisp dark rim.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.2f);

        // Indicator — white line from inner radius to near the rim, plus a
        // small white dot at the tip. Replaces the old coloured cap.
        const float dx = std::cos (angle - juce::MathConstants<float>::halfPi);
        const float dy = std::sin (angle - juce::MathConstants<float>::halfPi);
        const float lineInnerR = radius * 0.30f;
        const float lineOuterR = radius * 0.92f;
        const float tipR       = juce::jmax (1.5f, radius * 0.12f);
        const float lineX1 = cx + lineInnerR * dx;
        const float lineY1 = cy + lineInnerR * dy;
        const float lineX2 = cx + lineOuterR * dx;
        const float lineY2 = cy + lineOuterR * dy;

        g.setColour (juce::Colours::white);
        g.drawLine (lineX1, lineY1, lineX2, lineY2, 2.0f);

        // Tip dot.
        const float tipX = cx + (lineOuterR - tipR * 0.4f) * dx;
        const float tipY = cy + (lineOuterR - tipR * 0.4f) * dy;
        g.setColour (juce::Colours::white);
        g.fillEllipse (tipX - tipR, tipY - tipR, tipR * 2.0f, tipR * 2.0f);
        g.setColour (juce::Colour (0x80000000));
        g.drawEllipse (tipX - tipR, tipY - tipR, tipR * 2.0f, tipR * 2.0f, 0.5f);
    }
};

// 4K-derived band/section accent colors. Re-used wherever we need the
// canonical SSL palette. Names also drive the track colour-picker labels.
namespace fourKColors
{
    inline constexpr juce::uint32 kHpfBlue   = 0xff4a7c9e;
    inline constexpr juce::uint32 kLfGreen   = 0xff5c9a5c;
    inline constexpr juce::uint32 kLmAmber   = 0xffd9a35a;
    inline constexpr juce::uint32 kHmOrange  = 0xffc47a44;
    inline constexpr juce::uint32 kHfRed     = 0xffc44444;
    inline constexpr juce::uint32 kCompGold  = 0xffd09060;
    inline constexpr juce::uint32 kSendPurple= 0xff9080c0;
    inline constexpr juce::uint32 kPanCyan   = 0xff70b8c0;
    inline constexpr juce::uint32 kMasterTan = 0xffd0a060;
}

// SSL 9000J band colours — used only for the EQ knob bodies. Kept in a
// separate namespace so the track colour-picker (driven by fourKColors) keeps
// matching its labels (Red / Orange / Amber / Green).
namespace sslEqColors
{
    inline constexpr juce::uint32 kHfRed   = 0xffc44444;
    inline constexpr juce::uint32 kHmGreen = 0xff5fa55f;
    inline constexpr juce::uint32 kLmBlue  = 0xff5878b0;
    inline constexpr juce::uint32 kLfBlack = 0xff353538;
    inline constexpr juce::uint32 kHpfBlue = 0xff4a7c9e;
}
} // namespace adhdaw
