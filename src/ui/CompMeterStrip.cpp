#include "CompMeterStrip.h"
#include "ADHDawLookAndFeel.h"

namespace adhdaw
{
CompMeterStrip::CompMeterStrip (Track& t) : track (t)
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    startTimerHz (30);
}

CompMeterStrip::~CompMeterStrip() = default;

float CompMeterStrip::dbToFrac (float db) const noexcept
{
    return juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / (kCeilingDb - kFloorDb));
}

float CompMeterStrip::fracToDb (float frac) const noexcept
{
    return juce::jlimit (kFloorDb, kCeilingDb,
                          kFloorDb + frac * (kCeilingDb - kFloorDb));
}

float CompMeterStrip::yForDb (float db, juce::Rectangle<float> area) const noexcept
{
    const float frac = dbToFrac (db);
    return area.getBottom() - 2.0f - frac * (area.getHeight() - 4.0f);
}

float CompMeterStrip::dbForY (int y, juce::Rectangle<float> area) const noexcept
{
    const float relative = (area.getBottom() - 2.0f - (float) y) / (area.getHeight() - 4.0f);
    return fracToDb (relative);
}

void CompMeterStrip::resized()
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    // Threshold handle on the LEFT, then a gap, then input bar, then GR bar.
    handleArea   = b.removeFromLeft  (kHandleW);
    b.removeFromLeft (1.0f);
    inputBarArea = b.removeFromLeft  (kBarW);
    b.removeFromLeft (1.0f);
    grBarArea    = b.removeFromLeft  (kBarW);
}

void CompMeterStrip::timerCallback()
{
    const float inputDb = track.meterInputDb.load (std::memory_order_relaxed);
    if (inputDb > displayedInputDb)
        displayedInputDb = inputDb;
    else
        displayedInputDb += (inputDb - displayedInputDb) * 0.15f;

    if (inputDb >= inputPeakHoldDb)
    {
        inputPeakHoldDb = inputDb;
        inputPeakHoldFrames = 18;  // ~600 ms at 30 Hz
    }
    else if (inputPeakHoldFrames > 0)
    {
        --inputPeakHoldFrames;
    }
    else
    {
        inputPeakHoldDb = juce::jmax (kFloorDb, inputPeakHoldDb - 1.5f);
    }

    const float gr = track.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb)
        displayedGrDb = gr;
    else
        displayedGrDb += (gr - displayedGrDb) * 0.18f;

    repaint();
}

void CompMeterStrip::paint (juce::Graphics& g)
{
    auto bg = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff141418));
    g.fillRoundedRectangle (bg, 2.0f);
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawRoundedRectangle (bg, 2.0f, 0.6f);

    // ── Input meter (left bar) ─────────────────────────────────────────────
    {
        const auto bar = inputBarArea;
        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillRoundedRectangle (bar, 1.5f);

        const float frac = dbToFrac (displayedInputDb);
        if (frac > 0.001f)
        {
            const float fillTop = bar.getBottom() - 1.0f - frac * (bar.getHeight() - 2.0f);
            juce::ColourGradient grad (juce::Colour (0xff44d044), bar.getX(), bar.getBottom(),
                                        juce::Colour (0xffd05050), bar.getX(), bar.getY(), false);
            grad.addColour (dbToFrac (-12.0f), juce::Colour (0xffe0c050));
            grad.addColour (dbToFrac  (-3.0f), juce::Colour (0xffd07040));
            g.setGradientFill (grad);
            g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f,
                                                 fillTop,
                                                 bar.getWidth() - 2.0f,
                                                 bar.getBottom() - 1.0f - fillTop));
        }

        // Peak-hold tick
        const float peakFrac = dbToFrac (inputPeakHoldDb);
        if (peakFrac > 0.001f)
        {
            const float y = bar.getBottom() - 1.0f - peakFrac * (bar.getHeight() - 2.0f);
            g.setColour (peakFrac > dbToFrac (-3.0f) ? juce::Colour (0xffff5050)
                                                     : juce::Colour (0xffe0e0e0));
            g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, y - 0.5f,
                                                 bar.getWidth() - 2.0f, 1.5f));
        }
    }

    // ── GR meter (right bar) — fills DOWN from the top as compression bites
    {
        const auto bar = grBarArea;
        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillRoundedRectangle (bar, 1.5f);

        const float grAbs = juce::jlimit (0.0f, std::abs (kGrFloorDb), std::abs (displayedGrDb));
        if (grAbs > 0.05f)
        {
            const float frac = grAbs / std::abs (kGrFloorDb);
            const float fillBottom = bar.getY() + 1.0f + frac * (bar.getHeight() - 2.0f);
            juce::ColourGradient grad (juce::Colour (fourKColors::kCompGold).brighter (0.2f),
                                        bar.getX(), bar.getY(),
                                        juce::Colour (fourKColors::kHfRed).brighter (0.1f),
                                        bar.getX(), bar.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f,
                                                 bar.getY() + 1.0f,
                                                 bar.getWidth() - 2.0f,
                                                 fillBottom - bar.getY() - 1.0f));
        }
    }

    // ── Threshold handle: triangle pointing toward the input bar ──────────
    {
        const float thresh = track.strip.compVcaThreshDb.load (std::memory_order_relaxed);
        const float y = yForDb (thresh, inputBarArea);

        const float tx = handleArea.getRight();  // tip points to the input meter
        const float by = y;
        juce::Path tri;
        tri.addTriangle (handleArea.getX(), by - 4.5f,
                         handleArea.getX(), by + 4.5f,
                         tx,                by);
        const bool engaged = track.strip.compEnabled.load (std::memory_order_relaxed);
        g.setColour (engaged ? juce::Colour (fourKColors::kCompGold).brighter (0.1f)
                              : juce::Colour (0xff707074));
        g.fillPath (tri);
        g.setColour (juce::Colour (0xff0a0a0a));
        g.strokePath (tri, juce::PathStrokeType (0.6f));

        // Faint horizontal threshold line across the input bar so the user
        // sees where the level crosses it.
        g.setColour (juce::Colour (0xffd09060).withAlpha (0.5f));
        g.drawLine (inputBarArea.getX() - 1.0f, by, inputBarArea.getRight() + 1.0f, by, 0.7f);
    }

    // Tick marks on the input bar (-3 / -12 / -24)
    g.setColour (juce::Colour (0x40ffffff));
    for (float dB : { -3.0f, -12.0f, -24.0f })
    {
        const float y = yForDb (dB, inputBarArea);
        g.drawLine (inputBarArea.getRight() - 1.5f, y, inputBarArea.getRight(), y, 0.6f);
    }
}

void CompMeterStrip::mouseDown (const juce::MouseEvent& e)
{
    // Drag anywhere on the LEFT half (handle column) sets threshold.
    draggingThreshold = (e.x <= (int) handleArea.getRight() + 2);
    if (draggingThreshold)
    {
        const float db = dbForY (e.y, inputBarArea);
        track.strip.compVcaThreshDb.store (db, std::memory_order_relaxed);
        repaint();
    }
}

void CompMeterStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingThreshold) return;
    const float db = dbForY (e.y, inputBarArea);
    track.strip.compVcaThreshDb.store (db, std::memory_order_relaxed);
    repaint();
}

void CompMeterStrip::mouseUp (const juce::MouseEvent&)
{
    draggingThreshold = false;
}

void CompMeterStrip::mouseDoubleClick (const juce::MouseEvent&)
{
    track.strip.compVcaThreshDb.store (-12.0f, std::memory_order_relaxed);
    repaint();
}
} // namespace adhdaw
