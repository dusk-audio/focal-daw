#include "AuxBusComponent.h"
#include "ADHDawLookAndFeel.h"  // fourKColors palette

namespace adhdaw
{
AuxBusComponent::AuxBusComponent (AuxBus& a) : aux (a)
{
    nameLabel.setText (aux.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    addAndMakeVisible (nameLabel);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (aux.strip.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    faderSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
    faderSlider.setTextValueSuffix (" dB");
    faderSlider.onValueChange = [this]
    {
        aux.strip.faderDb.store ((float) faderSlider.getValue(), std::memory_order_relaxed);
    };
    addAndMakeVisible (faderSlider);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (aux.strip.mute.load (std::memory_order_relaxed), juce::dontSendNotification);
    muteButton.onClick = [this]
    {
        aux.strip.mute.store (muteButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (muteButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::yellow.darker (0.2f));
    soloButton.setToggleState (aux.strip.solo.load (std::memory_order_relaxed), juce::dontSendNotification);
    soloButton.onClick = [this]
    {
        aux.strip.solo.store (soloButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (soloButton);
}

void AuxBusComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showColourMenu();
}

void AuxBusComponent::applyAuxColour (juce::Colour c)
{
    aux.colour = c;
    repaint();
    // Channel-strip bus buttons poll for aux colour changes in their timer
    // callback (see ChannelStripComponent::timerCallback) and re-skin from
    // there, so changing the aux colour here automatically propagates.
}

void AuxBusComponent::showColourMenu()
{
    const std::pair<const char*, juce::uint32> presets[] = {
        { "Red",        fourKColors::kHfRed     },
        { "Orange",     fourKColors::kHmOrange  },
        { "Amber",      fourKColors::kLmAmber   },
        { "Green",      fourKColors::kLfGreen   },
        { "Cyan",       fourKColors::kPanCyan   },
        { "Blue",       fourKColors::kHpfBlue   },
        { "Purple",     fourKColors::kSendPurple},
        { "Tan",        fourKColors::kMasterTan },
    };

    juce::PopupMenu menu;
    menu.addSectionHeader ("Bus colour");
    for (size_t i = 0; i < std::size (presets); ++i)
    {
        juce::PopupMenu::Item item;
        item.itemID = (int) (i + 1);
        item.text = presets[i].first;
        item.colour = juce::Colour (presets[i].second);
        menu.addItem (item);
    }

    juce::Component::SafePointer<AuxBusComponent> safe (this);
    std::vector<std::pair<juce::String, juce::uint32>> presetCopy;
    presetCopy.reserve (std::size (presets));
    for (auto& p : presets) presetCopy.emplace_back (juce::String (p.first), p.second);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [safe, presetCopy] (int result)
        {
            if (result <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            const int idx = result - 1;
            if (idx >= 0 && idx < (int) presetCopy.size())
                self->applyAuxColour (juce::Colour (presetCopy[(size_t) idx].second));
        });
}

static void drawSectionPlaceholder (juce::Graphics& g, juce::Rectangle<int> r,
                                    const juce::String& label, juce::Colour accent)
{
    if (r.isEmpty()) return;
    g.setColour (juce::Colour (0xff222226));
    g.fillRoundedRectangle (r.toFloat(), 3.0f);
    g.setColour (accent.withAlpha (0.45f));
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 0.8f);
    g.setColour (accent.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    g.drawText (label, r.reduced (3, 2), juce::Justification::centredTop, false);
}

void AuxBusComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    g.setColour (juce::Colour (0xff181820));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (aux.colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xff2a2a2e));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.0f);

    drawSectionPlaceholder (g, fxArea,   "FX",   juce::Colour (0xff9080c0));
    drawSectionPlaceholder (g, eqArea,   "EQ",   juce::Colour (0xff80c090));
    drawSectionPlaceholder (g, compArea, "COMP", juce::Colour (0xffd09060));
}

void AuxBusComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);
    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);

    fxArea   = area.removeFromTop (40);
    area.removeFromTop (3);
    eqArea   = area.removeFromTop (66);
    area.removeFromTop (3);
    compArea = area.removeFromTop (52);
    area.removeFromTop (6);

    auto buttons = area.removeFromBottom (24);
    muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2));
    soloButton.setBounds (buttons.reduced (2));
    area.removeFromBottom (4);

    faderSlider.setBounds (area);
}
} // namespace adhdaw
