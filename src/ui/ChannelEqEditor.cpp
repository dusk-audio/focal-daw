#include "ChannelEqEditor.h"
#include "ADHDawLookAndFeel.h"

namespace adhdaw
{
namespace
{
struct BandSpec
{
    const char* name;
    juce::Colour accent;
    float freqMin, freqMax;
    std::atomic<float>* (*gain) (ChannelStripParams&);
    std::atomic<float>* (*freq) (ChannelStripParams&);
};

const std::array<BandSpec, 4>& bandSpecs()
{
    static const std::array<BandSpec, 4> specs {{
        { "HF", juce::Colour (sslEqColors::kHfRed),    ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfFreq; } },
        { "HM", juce::Colour (sslEqColors::kHmGreen),  ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmFreq; } },
        { "LM", juce::Colour (sslEqColors::kLmBlue),   ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmFreq; } },
        { "LF", juce::Colour (sslEqColors::kLfBlack),  ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfFreq; } },
    }};
    return specs;
}

inline juce::String formatFrequency (double hz)
{
    if (hz >= 1000.0) return juce::String (hz / 1000.0, 1) + " kHz";
    return juce::String ((int) std::round (hz)) + " Hz";
}
} // namespace

ChannelEqEditor::ChannelEqEditor (Track& t) : track (t)
{
    titleLabel.setText ("EQ — " + track.name, juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

    typeButton.setClickingTogglesState (true);
    typeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff5a3a20));
    typeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff202020));
    typeButton.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
    typeButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    typeButton.setTooltip ("Brown (E-series) / Black (G-series)");
    typeButton.setToggleState (track.strip.eqBlackMode.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
    typeButton.onClick = [this]
    {
        track.strip.eqBlackMode.store (typeButton.getToggleState(), std::memory_order_relaxed);
        refreshTypeButton();
    };
    refreshTypeButton();
    addAndMakeVisible (typeButton);

    for (size_t i = 0; i < bandSpecs().size(); ++i)
    {
        const auto& spec = bandSpecs()[i];
        auto& row = rows[i];

        row.nameLabel.setText (spec.name, juce::dontSendNotification);
        row.nameLabel.setJustificationType (juce::Justification::centred);
        row.nameLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
        row.nameLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        addAndMakeVisible (row.nameLabel);

        auto makeKnob = [] (juce::Slider& k, juce::Colour fill, double mn, double mx,
                             double defaultVal, double skewMid)
        {
            k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k.setColour (juce::Slider::rotarySliderFillColourId, fill);
            k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
            k.setRange (mn, mx, mn < 0 ? 0.1 : 1.0);
            if (skewMid > 0) k.setSkewFactorFromMidPoint (skewMid);
            k.setDoubleClickReturnValue (true, defaultVal);
            k.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 80, 18);
            k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffe0e0e0));
            k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
            k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
        };

        row.gain = std::make_unique<juce::Slider>();
        makeKnob (*row.gain, spec.accent,
                  ChannelStripParams::kBandGainMin, ChannelStripParams::kBandGainMax, 0.0, 0.0);
        row.gain->setNumDecimalPlacesToDisplay (1);
        row.gain->setTextValueSuffix (" dB");
        row.gain->setValue (spec.gain (track.strip)->load (std::memory_order_relaxed),
                             juce::dontSendNotification);
        {
            auto* atomicPtr = spec.gain (track.strip);
            auto* knob = row.gain.get();
            knob->onValueChange = [knob, atomicPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
            };
        }
        addAndMakeVisible (row.gain.get());

        const float defaultFreq = (i == 0 ? 8000.0f : i == 1 ? 2000.0f : i == 2 ? 600.0f : 100.0f);
        row.freq = std::make_unique<juce::Slider>();
        makeKnob (*row.freq, spec.accent, spec.freqMin, spec.freqMax,
                   defaultFreq, defaultFreq);
        row.freq->setNumDecimalPlacesToDisplay (0);
        row.freq->textFromValueFunction = [] (double v) { return formatFrequency (v); };
        row.freq->setValue (spec.freq (track.strip)->load (std::memory_order_relaxed),
                             juce::dontSendNotification);
        {
            auto* atomicPtr = spec.freq (track.strip);
            auto* knob = row.freq.get();
            knob->onValueChange = [knob, atomicPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
            };
        }
        addAndMakeVisible (row.freq.get());
    }

    setSize (320, 360);
}

ChannelEqEditor::~ChannelEqEditor() = default;

void ChannelEqEditor::refreshTypeButton()
{
    typeButton.setButtonText (typeButton.getToggleState() ? "G" : "E");
}

void ChannelEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawRect (getLocalBounds(), 1);
}

void ChannelEqEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto header = area.removeFromTop (24);
    typeButton.setBounds (header.removeFromRight (40));
    titleLabel.setBounds (header);
    area.removeFromTop (10);

    constexpr int kKnobSize = 56;
    constexpr int kValueH   = 18;
    constexpr int kRowH     = kKnobSize + kValueH + 10;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto row = area.removeFromTop (kRowH);
        auto labelCol = row.removeFromLeft (40);
        rows[i].nameLabel.setBounds (labelCol);

        const int colW = row.getWidth() / 2;
        auto gainCol = row.removeFromLeft (colW);
        auto freqCol = row;
        rows[i].gain->setBounds (gainCol.reduced (6));
        rows[i].freq->setBounds (freqCol.reduced (6));
    }
}
} // namespace adhdaw
