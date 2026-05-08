#include "AnalogVuMeter.h"
#include <cmath>

namespace focal
{
namespace
{
const juce::Colour kFaceTop        { 0xfff4e7b4 };
const juce::Colour kFaceBottom     { 0xffe8d895 };
const juce::Colour kBezelOuter     { 0xff141416 };
const juce::Colour kBezelInner     { 0xff2a2a2e };
const juce::Colour kInk            { 0xff111114 };
const juce::Colour kInkSoft        { 0xff5a5a60 };
const juce::Colour kRedZone        { 0xffb02020 };
const juce::Colour kNeedle         { 0xff141418 };
const juce::Colour kNeedleR        { 0xff8a2828 };
const juce::Colour kPivotHousing   { 0xff222226 };

// Voltage-linear mapping (10^(dB/20)) - matches a real galvanometer's
// mechanical response and puts 0 VU at ~70 % of the dial travel exactly
// like the reference photo.
float dbToFraction (float vuDb) noexcept
{
    constexpr float kMaxVu = 3.0f;
    const float maxV = std::pow (10.0f, kMaxVu / 20.0f);
    const float v    = std::pow (10.0f, juce::jmin (vuDb, kMaxVu) / 20.0f);
    return juce::jlimit (0.0f, 1.0f, v / maxV);
}

// Visible idle position so the needle doesn't collapse into a flat-left
// stub when the meter is silent. -25 dB ≈ 5 % travel - reads as "off"
// without looking broken.
constexpr float kIdleVuDb = -25.0f;

struct LabelledTick
{
    float vuDb;
    const char* label;        // nullptr = unlabelled minor tick
    bool red;
    bool primary;             // primary labels survive small-width filtering
};

const LabelledTick kTicks[] =
{
    { -20.0f, "20", false, true  },
    { -10.0f, "10", false, true  },
    {  -7.0f,  "7", false, false },
    {  -5.0f,  "5", false, false },
    {  -3.0f,  "3", false, false },
    {  -2.0f, nullptr, false, false },
    {  -1.0f, nullptr, false, false },
    {   0.0f,  "0", false, true  },
    {   1.0f, nullptr, true,  false },
    {   2.0f, nullptr, true,  false },
    {   3.0f,  "3", true,  true  },
};
} // namespace

AnalogVuMeter::AnalogVuMeter (const std::atomic<float>* l, const std::atomic<float>* r)
    : leftDb (l), rightDb (r)
{
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
    startTimerHz (30);
}

AnalogVuMeter::~AnalogVuMeter() = default;

void AnalogVuMeter::setReferenceLevelDb (float refDb)
{
    if (std::abs (referenceDbFs - refDb) < 0.01f) return;
    referenceDbFs = refDb;
    rebuildCachedFace();
    repaint();
}

void AnalogVuMeter::resized()
{
    // Pivot sits just below the face, close enough that the needle's
    // visible portion still spans a satisfying length but the tip stays
    // inside the upper half of the face. arcRadius lands the tip near
    // the top of the labelled tick row.
    const auto bounds = getLocalBounds().toFloat();
    pivot.x   = bounds.getCentreX();
    pivot.y   = bounds.getBottom() + bounds.getHeight() * 0.18f;
    arcRadius = pivot.y - bounds.getY() - bounds.getHeight() * 0.10f;
    halfArcDeg = 55.0f;   // wider sweep than 45° so tick labels spread out
    rebuildCachedFace();
}

void AnalogVuMeter::timerCallback()
{
    if (leftDb == nullptr) return;

    constexpr float kAlpha = 0.40f;
    auto smooth = [] (float& current, float target)
    {
        current += kAlpha * (target - current);
    };

    const float l = leftDb->load (std::memory_order_relaxed) - referenceDbFs;
    smooth (displayedDbL, juce::jmax (l, kIdleVuDb));

    if (rightDb != nullptr)
    {
        const float r = rightDb->load (std::memory_order_relaxed) - referenceDbFs;
        smooth (displayedDbR, juce::jmax (r, kIdleVuDb));
    }

    repaint();
}

void AnalogVuMeter::rebuildCachedFace()
{
    const auto w = getWidth();
    const auto h = getHeight();
    if (w <= 0 || h <= 0)
    {
        cachedFace = juce::Image();
        return;
    }

    cachedFace = juce::Image (juce::Image::ARGB, w, h, true);
    juce::Graphics g (cachedFace);
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);

    // Bezel + face. Two-tone outline + cream paper fill with a vertical
    // gradient so the face has a faint analog patina.
    const float corner = juce::jmin (3.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.06f);
    g.setColour (kBezelOuter);
    g.fillRoundedRectangle (bounds, corner);
    g.setColour (kBezelInner);
    g.fillRoundedRectangle (bounds.reduced (1.0f), corner * 0.85f);

    auto faceRect = bounds.reduced (2.0f);
    {
        juce::ColourGradient grad (kFaceTop, faceRect.getX(), faceRect.getY(),
                                    kFaceBottom, faceRect.getX(), faceRect.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (faceRect, corner * 0.7f);
    }

    auto fracToScreenAngleRad = [this] (float frac)
    {
        const float deg = (frac - 0.5f) * 2.0f * halfArcDeg - 90.0f;
        return juce::degreesToRadians (deg);
    };
    auto pointOnArc = [&] (float frac, float radius)
    {
        const float a = fracToScreenAngleRad (frac);
        return juce::Point<float> (pivot.x + std::cos (a) * radius,
                                     pivot.y + std::sin (a) * radius);
    };

    // Red overload arc - thick stroked stroke, sits along the tick line.
    {
        juce::Path arc;
        const float fracStart = dbToFraction (0.0f);
        const float fracEnd   = 1.0f;
        const float jucePi = juce::MathConstants<float>::pi;
        // JUCE addCentredArc uses radians from 12-o-clock clockwise.
        // Convert: trig angle (0 = right) -> JUCE angle (0 = up) = trig + π/2.
        arc.addCentredArc (pivot.x, pivot.y, arcRadius, arcRadius, 0.0f,
                            fracToScreenAngleRad (fracStart) + jucePi * 0.5f,
                            fracToScreenAngleRad (fracEnd)   + jucePi * 0.5f,
                            true);
        g.setColour (kRedZone);
        g.strokePath (arc, juce::PathStrokeType (juce::jmax (2.5f, (float) h * 0.06f)));
    }

    // Tick marks only - no numeric labels, no baseline arc connecting them. Labels were too crowded at the
    // bus/master strip width; the tick density alone communicates the
    // scale, and the red zone marks where overload begins.
    const float tickOuter = arcRadius;
    const float tickInner = arcRadius - juce::jmax (4.0f, (float) h * 0.13f);
    const float dotR      = juce::jmax (0.9f, (float) h * 0.020f);

    for (const auto& t : kTicks)
    {
        const float frac = dbToFraction (t.vuDb);
        const auto pa = pointOnArc (frac, tickOuter);
        const auto pb = pointOnArc (frac, tickInner);
        const auto pd = pointOnArc (frac, tickInner - dotR * 2.5f);

        g.setColour (t.red ? kRedZone : kInk);
        // Major (primary) ticks are slightly longer + thicker so the eye
        // still gets a coarse landmark grid even without numbers.
        const float thickness = t.primary ? 1.3f : 0.8f;
        g.drawLine (juce::Line<float> (pa, pb), thickness);
        g.fillEllipse (juce::Rectangle<float> (dotR * 2.0f, dotR * 2.0f).withCentre (pd));
    }

    // "vu" badge - lower-left of the face, where there's empty headroom
    // below the labelled-tick row. Avoids the +3 / red-zone clutter on the
    // right side that the previous layout collided with.
    {
        const float vuFont = juce::jlimit (8.0f, 14.0f, (float) h * 0.24f);
        g.setFont (juce::Font (juce::FontOptions (vuFont, juce::Font::italic | juce::Font::bold)));
        g.setColour (kInk);
        const auto vuBox = juce::Rectangle<float> (faceRect.getRight() - vuFont * 2.4f - 2.0f,
                                                     faceRect.getBottom() - vuFont * 1.25f,
                                                     vuFont * 2.2f, vuFont * 1.15f);
        g.drawText ("vu", vuBox, juce::Justification::centredRight, false);
    }

}

void AnalogVuMeter::paint (juce::Graphics& g)
{
    if (cachedFace.isValid())
        g.drawImageAt (cachedFace, 0, 0);

    auto drawNeedle = [&] (float displayDb, juce::Colour c, float thickness)
    {
        const float frac = dbToFraction (displayDb);
        const float deg  = (frac - 0.5f) * 2.0f * halfArcDeg - 90.0f;
        const float rad  = juce::degreesToRadians (deg);
        // Needle starts inside the visible pivot housing and ends just
        // shy of the tick line, never overshooting into the tick numbers.
        const float baseR = juce::jmax (0.0f, arcRadius * 0.18f);
        const float tipR  = arcRadius - juce::jmax (1.0f, (float) getHeight() * 0.04f);
        const auto base = juce::Point<float> (pivot.x + std::cos (rad) * baseR,
                                                 pivot.y + std::sin (rad) * baseR);
        const auto tip  = juce::Point<float> (pivot.x + std::cos (rad) * tipR,
                                                 pivot.y + std::sin (rad) * tipR);
        g.setColour (c);
        g.drawLine (juce::Line<float> (base, tip), thickness);
    };

    // Right needle slightly behind the left one (drawn first) so a centred-
    // pan stereo signal shows the mono read-out cleanly with the L on top.
    if (rightDb != nullptr)
        drawNeedle (displayedDbR, kNeedleR, 1.2f);
    drawNeedle (displayedDbL, kNeedle, 1.4f);
}
} // namespace focal
