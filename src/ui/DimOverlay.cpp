#include "DimOverlay.h"

namespace focal
{
DimOverlay::DimOverlay()
{
    setInterceptsMouseClicks (true, false);
    setWantsKeyboardFocus (false);
    setOpaque (false);
}

void DimOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
}

void DimOverlay::mouseDown (const juce::MouseEvent&)
{
    if (onClick) onClick();
}

void DimOverlay::parentSizeChanged()
{
    if (auto* p = getParentComponent())
        setBounds (p->getLocalBounds());
}
} // namespace focal
