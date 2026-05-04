#include "TapeStrip.h"

namespace adhdaw
{
TapeStrip::TapeStrip (Session& s, AudioEngine& e) : session (s), engine (e)
{
    setOpaque (true);
    startTimerHz (30);
}

TapeStrip::~TapeStrip() = default;

int TapeStrip::naturalHeight()
{
    return kRulerH + Session::kNumTracks * (kTrackRowH + kRowGap) + 6;
}

juce::Rectangle<int> TapeStrip::labelColumnBounds() const noexcept
{
    return getLocalBounds().withTrimmedTop (kRulerH).withWidth (kTrackLabelW);
}

juce::Rectangle<int> TapeStrip::rulerBounds() const noexcept
{
    return juce::Rectangle<int> (kTrackLabelW, 0,
                                  juce::jmax (0, getWidth() - kTrackLabelW),
                                  kRulerH);
}

juce::Rectangle<int> TapeStrip::tracksColumnBounds() const noexcept
{
    return juce::Rectangle<int> (kTrackLabelW, kRulerH,
                                  juce::jmax (0, getWidth() - kTrackLabelW),
                                  juce::jmax (0, getHeight() - kRulerH));
}

juce::Rectangle<int> TapeStrip::rowBounds (int trackIdx) const noexcept
{
    auto col = tracksColumnBounds();
    const int y = col.getY() + trackIdx * (kTrackRowH + kRowGap);
    return juce::Rectangle<int> (col.getX(), y, col.getWidth(), kTrackRowH);
}

double TapeStrip::pixelsPerSecond() const noexcept
{
    const double sr = engine.getCurrentSampleRate();
    if (sr <= 0.0) return 0.0;

    // Find the rightmost sample we need to show — either the longest region
    // or the current playhead — then add a margin so there's always blank
    // tape past the last recorded thing.
    juce::int64 maxSample = engine.getTransport().getPlayhead();
    for (int t = 0; t < Session::kNumTracks; ++t)
        for (auto& r : session.track (t).regions)
            maxSample = juce::jmax (maxSample, r.timelineStart + r.lengthInSamples);

    const double maxSeconds = juce::jmax (60.0, (double) maxSample / sr * 1.20);

    auto col = tracksColumnBounds();
    if (col.getWidth() <= 0) return 0.0;
    return (double) col.getWidth() / maxSeconds;
}

juce::int64 TapeStrip::sampleAtX (int x) const noexcept
{
    const double sr = engine.getCurrentSampleRate();
    const double px = pixelsPerSecond();
    if (sr <= 0.0 || px <= 0.0) return 0;
    auto col = tracksColumnBounds();
    const double seconds = (double) (x - col.getX()) / px;
    return (juce::int64) juce::jmax (0.0, seconds * sr);
}

int TapeStrip::xForSample (juce::int64 s) const noexcept
{
    const double sr = engine.getCurrentSampleRate();
    const double px = pixelsPerSecond();
    if (sr <= 0.0 || px <= 0.0) return tracksColumnBounds().getX();
    return tracksColumnBounds().getX() + (int) ((double) s / sr * px);
}

void TapeStrip::resized() {}

void TapeStrip::timerCallback()
{
    const auto now = engine.getTransport().getPlayhead();
    if (now == lastPlayhead) return;

    const int oldX = xForSample (lastPlayhead < 0 ? 0 : lastPlayhead);
    const int newX = xForSample (now);
    lastPlayhead = now;

    // Repaint a thin vertical band covering both the old and new playhead
    // positions plus a few pixels of margin so we don't see ghosting.
    const int x = juce::jmin (oldX, newX) - 2;
    const int w = std::abs (newX - oldX) + 4;
    repaint (x, 0, w, getHeight());
}

void TapeStrip::mouseDown (const juce::MouseEvent& e)
{
    auto col = tracksColumnBounds();
    if (! col.contains (e.x, e.y)) return;
    const auto sample = sampleAtX (e.x);
    engine.getTransport().setPlayhead (sample);
    repaint();
}

void TapeStrip::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0e0e10));

    auto label = labelColumnBounds();
    auto col   = tracksColumnBounds();

    // ── Time ruler at the top of the tracks column ──
    auto ruler = rulerBounds();
    g.setColour (juce::Colour (0xff181820));
    g.fillRect (ruler);
    g.setColour (juce::Colour (0xff2a2a30));
    g.drawHorizontalLine (ruler.getBottom() - 1, (float) ruler.getX(), (float) ruler.getRight());

    const double px = pixelsPerSecond();
    const double sr = engine.getCurrentSampleRate();
    if (px > 0.0 && sr > 0.0)
    {
        // Draw second / 5-second / 30-second tick marks depending on zoom.
        double tickEverySec = 1.0;
        if (px < 6.0)       tickEverySec = 30.0;
        else if (px < 16.0) tickEverySec = 10.0;
        else if (px < 40.0) tickEverySec = 5.0;
        else                tickEverySec = 1.0;

        g.setColour (juce::Colour (0xff707074));
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                    9.5f, juce::Font::plain)));

        const double endSec = (double) col.getWidth() / px;
        for (double sec = 0.0; sec <= endSec; sec += tickEverySec)
        {
            const int x = col.getX() + (int) (sec * px);
            g.drawVerticalLine (x, (float) ruler.getY() + 8.0f, (float) ruler.getBottom());
            // Format seconds as mm:ss for readability
            const int mins = (int) (sec / 60.0);
            const int secs = (int) sec % 60;
            const auto timeLabel = juce::String::formatted ("%d:%02d", mins, secs);
            g.drawText (timeLabel, x + 3, ruler.getY(), 60, ruler.getHeight() - 1,
                         juce::Justification::centredLeft, false);
        }
    }

    // ── Track rows ──
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto row = rowBounds (t);

        // Row background — slightly darker every other row.
        g.setColour (t % 2 == 0 ? juce::Colour (0xff141418) : juce::Colour (0xff101014));
        g.fillRect (row);

        // Track label on the left (color stripe + number).
        auto labelRow = juce::Rectangle<int> (label.getX(), row.getY(),
                                                label.getWidth(), row.getHeight());
        g.setColour (session.track (t).colour.withAlpha (0.85f));
        g.fillRect (labelRow.removeFromLeft (3));
        g.setColour (juce::Colour (0xffd0d0d0));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (juce::String (t + 1), labelRow.withTrimmedLeft (4),
                     juce::Justification::centredLeft, false);

        // Recorded regions for this track.
        for (auto& region : session.track (t).regions)
        {
            const int x0 = xForSample (region.timelineStart);
            const int x1 = xForSample (region.timelineStart + region.lengthInSamples);
            if (x1 <= col.getX() || x0 >= col.getRight()) continue;

            auto regionRect = juce::Rectangle<int> (x0, row.getY() + 1,
                                                     juce::jmax (2, x1 - x0),
                                                     row.getHeight() - 2)
                                  .getIntersection (col.withTrimmedTop (1).withTrimmedBottom (1));
            if (regionRect.isEmpty()) continue;

            // Block fill — track color, slightly darker so the row label still pops.
            const auto fillColour = session.track (t).colour.withMultipliedSaturation (0.85f)
                                                            .withMultipliedBrightness (0.65f);
            g.setColour (fillColour);
            g.fillRoundedRectangle (regionRect.toFloat(), 2.0f);

            // Outline
            g.setColour (session.track (t).colour.withAlpha (0.9f));
            g.drawRoundedRectangle (regionRect.toFloat().reduced (0.5f), 2.0f, 0.8f);

            // Tiny waveform-stub stripe along the centre — not real audio, just
            // a visual cue that the block holds something.
            g.setColour (session.track (t).colour.brighter (0.4f).withAlpha (0.6f));
            const float midY = (float) regionRect.getCentreY();
            g.drawHorizontalLine ((int) midY, (float) regionRect.getX() + 2.0f,
                                   (float) regionRect.getRight() - 2.0f);
        }
    }

    // ── Vertical separator between labels and tracks ──
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawVerticalLine (col.getX() - 1, 0.0f, (float) getHeight());

    // ── Playhead line ──
    const auto playhead = engine.getTransport().getPlayhead();
    const int phX = xForSample (playhead);
    if (phX >= col.getX() && phX <= col.getRight())
    {
        g.setColour (juce::Colour (0xffe04040));
        g.drawVerticalLine (phX, 0.0f, (float) getHeight());
    }
}
} // namespace adhdaw
