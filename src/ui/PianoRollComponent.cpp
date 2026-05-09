#include "PianoRollComponent.h"
#include <algorithm>

namespace focal
{
namespace
{
const juce::Colour kBgDark        { 0xff181820 };
const juce::Colour kRowWhite      { 0xff1c1c24 };
const juce::Colour kRowBlack      { 0xff141418 };
const juce::Colour kGridLine      { 0xff2a2a32 };
const juce::Colour kBeatLine      { 0xff3c3c46 };
const juce::Colour kBarLine       { 0xff5a5a64 };
const juce::Colour kHeaderBg      { 0xff202028 };
const juce::Colour kHeaderText    { 0xffb0b0b8 };
const juce::Colour kKeyWhite      { 0xffe0e0e6 };
const juce::Colour kKeyBlack      { 0xff242428 };
const juce::Colour kKeyText       { 0xff404048 };
const juce::Colour kKeyOctaveLine { 0xff8a8a94 };
const juce::Colour kNoteFill      { 0xff70b0e0 };
const juce::Colour kNoteSelected  { 0xffffd060 };
const juce::Colour kNoteEdge      { 0xff141418 };

// Pitch-class palette. Twelve distinct hues, manually picked rather
// than a pure hue-cycle so the chromatic neighbours stay visually
// distinct (a pure cycle puts e.g. F#/G next to each other at almost
// identical hues, which defeats the purpose). Indexed by note % 12;
// C = 0, B = 11.
const juce::Colour kPitchClassPalette[12] = {
    juce::Colour (0xffe0464a),  // C   - red
    juce::Colour (0xffe07f2a),  // C#  - orange
    juce::Colour (0xffe0c050),  // D   - amber
    juce::Colour (0xffb0d050),  // D#  - lime
    juce::Colour (0xff60c060),  // E   - green
    juce::Colour (0xff40c0a8),  // F   - teal
    juce::Colour (0xff50b0d0),  // F#  - cyan
    juce::Colour (0xff5090e0),  // G   - sky blue
    juce::Colour (0xff7068d8),  // G#  - indigo
    juce::Colour (0xff9858c8),  // A   - violet
    juce::Colour (0xffc060c0),  // A#  - magenta
    juce::Colour (0xffe06090),  // B   - rose
};

bool isBlackKey (int noteNumber) noexcept
{
    static constexpr bool blacks[12] =
        { false, true, false, true, false, false, true, false, true, false, true, false };
    return blacks[(noteNumber + 1200) % 12];
}

const char* noteNameForC (int noteNumber)
{
    // Returns labels only for C of each octave (e.g. "C4"); empty for others.
    // Pre-built static buffer is fine because the caller copies into a juce::String.
    if (noteNumber % 12 != 0) return "";
    static char buf[16];
    const int octave = (noteNumber / 12) - 1;     // MIDI 60 = C4
    std::snprintf (buf, sizeof (buf), "C%d", octave);
    return buf;
}
} // namespace

PianoRollComponent::PianoRollComponent (Session& s, int t, int r)
    : session (s), trackIdx (t), regionIdx (r)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
}

PianoRollComponent::~PianoRollComponent() = default;

MidiRegion* PianoRollComponent::region()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    auto& v = session.track (trackIdx).midiRegions.currentMutable();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

const MidiRegion* PianoRollComponent::region() const
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return nullptr;
    const auto& v = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return nullptr;
    return &v[(size_t) regionIdx];
}

int PianoRollComponent::yForNoteNumber (int n) const
{
    // High notes at the top (matches a piano keyboard's visual layout).
    // Note 127 → row 0, note 0 → row kNumKeys-1.
    return kHeaderHeight + (kNumKeys - 1 - n) * kNoteHeight - scrollY;
}

int PianoRollComponent::noteNumberForY (int y) const
{
    const int row = (y - kHeaderHeight + scrollY) / kNoteHeight;
    return juce::jlimit (0, kNumKeys - 1, kNumKeys - 1 - row);
}

int PianoRollComponent::xForTick (juce::int64 tick) const
{
    return kKeyboardWidth - scrollX
         + (int) std::round ((double) tick * pixelsPerTick);
}

juce::int64 PianoRollComponent::tickForX (int x) const
{
    const auto t = (juce::int64) std::round (
        (double) (x - kKeyboardWidth + scrollX) / pixelsPerTick);
    return juce::jmax ((juce::int64) 0, t);
}

void PianoRollComponent::resized() {}

void PianoRollComponent::paint (juce::Graphics& g)
{
    g.fillAll (kBgDark);

    const auto bounds = getLocalBounds();
    const auto headerArea  = bounds.withHeight (kHeaderHeight);
    // Bottom strips stack: CC lane at the very bottom, velocity lane
    // above it. The note grid fills the space between header and
    // velocity lane.
    const auto ccArea = juce::Rectangle<int> (kKeyboardWidth,
                                                 bounds.getBottom() - kCcStripH,
                                                 bounds.getWidth() - kKeyboardWidth,
                                                 kCcStripH);
    const auto velocityArea = juce::Rectangle<int> (kKeyboardWidth,
                                                       ccArea.getY() - kVelocityStripH,
                                                       bounds.getWidth() - kKeyboardWidth,
                                                       kVelocityStripH);
    const auto keyboardArea = juce::Rectangle<int> (0, kHeaderHeight,
                                                       kKeyboardWidth,
                                                       bounds.getHeight() - kHeaderHeight);
    const auto gridArea    = juce::Rectangle<int> (kKeyboardWidth, kHeaderHeight,
                                                     bounds.getWidth() - kKeyboardWidth,
                                                     bounds.getHeight() - kHeaderHeight - kVelocityStripH - kCcStripH);

    paintNoteGrid     (g, gridArea);
    paintBeatRuler    (g, headerArea);
    paintKeyboard     (g, keyboardArea);
    paintNotes        (g, gridArea);
    paintVelocityStrip (g, velocityArea);
    paintCcStrip      (g, ccArea);

    // Rubber-band overlay during BoxSelect drag. Rendered LAST so it
    // sits on top of every other layer.
    if (! rubberBand.isEmpty())
    {
        g.setColour (kNoteSelected.withAlpha (0.18f));
        g.fillRect (rubberBand);
        g.setColour (kNoteSelected.withAlpha (0.85f));
        g.drawRect (rubberBand, 1);
    }
}

void PianoRollComponent::paintNoteGrid (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.reduceClipRegion (area);

    // Row stripes - alternate so each octave reads as a unit. White-key
    // rows get the lighter shade, black-key rows the darker one.
    for (int n = 0; n < kNumKeys; ++n)
    {
        const int y = yForNoteNumber (n);
        if (y + kNoteHeight < area.getY() || y > area.getBottom()) continue;
        g.setColour (isBlackKey (n) ? kRowBlack : kRowWhite);
        g.fillRect (area.getX(), y, area.getWidth(), kNoteHeight);
    }

    // Scale-highlight overlay: shade rows whose note isn't in the
    // active scale so in-scale notes pop. Skipped (cheap branch) when
    // scale is Off.
    if (scale != Scale::Off)
    {
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        for (int n = 0; n < kNumKeys; ++n)
        {
            if (isInScale (n)) continue;
            const int y = yForNoteNumber (n);
            if (y + kNoteHeight < area.getY() || y > area.getBottom()) continue;
            g.fillRect (area.getX(), y, area.getWidth(), kNoteHeight);
        }
    }

    // Octave lines (C of each octave gets a brighter horizontal line).
    g.setColour (kKeyOctaveLine.withAlpha (0.20f));
    for (int n = 0; n < kNumKeys; n += 12)
    {
        const int y = yForNoteNumber (n);
        if (y < area.getY() || y > area.getBottom()) continue;
        g.drawHorizontalLine (y + kNoteHeight - 1,
                                (float) area.getX(), (float) area.getRight());
    }

    // Vertical beat lines. Pull tempo + region length so we can draw bars
    // 4 beats apart (assumes 4/4 - we'll consume Session::beatsPerBar
    // when the model surfaces it more directly).
    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;
    const int beatsPerBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
    const int ticksPerBeat = kMidiTicksPerQuarter;
    const int ticksPerBar  = ticksPerBeat * beatsPerBar;
    const int totalBars = (int) (r->lengthInTicks / ticksPerBar) + 1;
    for (int bar = 0; bar <= totalBars; ++bar)
    {
        const auto barTick = (juce::int64) bar * ticksPerBar;
        const int  bx = xForTick (barTick);
        if (bx < area.getX() || bx > area.getRight()) continue;
        g.setColour (kBarLine);
        g.drawVerticalLine (bx, (float) area.getY(), (float) area.getBottom());
        for (int beat = 1; beat < beatsPerBar; ++beat)
        {
            const int  bex = xForTick (barTick + beat * ticksPerBeat);
            if (bex < area.getX() || bex > area.getRight()) continue;
            g.setColour (kBeatLine);
            g.drawVerticalLine (bex, (float) area.getY(), (float) area.getBottom());
        }
    }

    // Region end shading - dim the area past the region's last tick so
    // the editable extent is visually obvious.
    const int endX = xForTick (r->lengthInTicks);
    if (endX < area.getRight())
    {
        g.setColour (juce::Colours::black.withAlpha (0.40f));
        g.fillRect (endX, area.getY(), area.getRight() - endX, area.getHeight());
    }
}

void PianoRollComponent::paintBeatRuler (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (kHeaderBg);
    g.fillRect (area);
    g.setColour (kGridLine);
    g.drawHorizontalLine (area.getBottom() - 1,
                            (float) area.getX(), (float) area.getRight());

    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;
    const int beatsPerBar = juce::jmax (1, session.beatsPerBar.load (std::memory_order_relaxed));
    const int ticksPerBeat = kMidiTicksPerQuarter;
    const int ticksPerBar = ticksPerBeat * beatsPerBar;
    const int totalBars = (int) (r->lengthInTicks / ticksPerBar) + 1;

    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    g.setColour (kHeaderText);
    // Pixels per beat tells us whether sub-beat labels ("4.2", "4.3", ...)
    // would visually fit. Below ~36 px/beat the labels overlap each other,
    // so we drop them and show bar numbers only.
    const float pxPerBeat = (float) ticksPerBeat * pixelsPerTick;
    const bool showSubBeats = pxPerBeat >= 36.0f;
    for (int bar = 0; bar <= totalBars; ++bar)
    {
        const int bx = xForTick ((juce::int64) bar * ticksPerBar);
        if (bx >= kKeyboardWidth && bx <= area.getRight())
        {
            g.drawText (juce::String (bar + 1), bx + 2, area.getY(),
                         40, area.getHeight(), juce::Justification::centredLeft, false);
        }

        if (! showSubBeats) continue;
        // Reaper-style sub-beat labels: "<bar>.<beat>" at every beat
        // line within the bar. Beat 1 is the bar number we already drew,
        // so start from beat 2.
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::plain)));
        g.setColour (kHeaderText.withAlpha (0.65f));
        for (int beat = 1; beat < beatsPerBar; ++beat)
        {
            const int bbx = xForTick ((juce::int64) bar * ticksPerBar
                                          + (juce::int64) beat * ticksPerBeat);
            if (bbx < kKeyboardWidth || bbx > area.getRight()) continue;
            g.drawText (juce::String (bar + 1) + "." + juce::String (beat + 1),
                         bbx + 2, area.getY(),
                         40, area.getHeight(),
                         juce::Justification::centredLeft, false);
        }
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.setColour (kHeaderText);
    }

    // Quantize HUD - tiny label in the upper-right of the ruler showing
    // the current snap denomination so the user can see what the 1..6 / 0
    // keyboard shortcuts are doing. Format: "Q: 1/16" or "Q: Off".
    juce::String qLabel ("Q: Off");
    if (snapTicks > 0)
    {
        // Convert ticks back to a denomination of a quarter. 480 ticks =
        // 1 quarter; 240 = 1/8; 120 = 1/16; etc. Whole-bar values map back
        // to "1/1" / "1/2" / "1/4". Anything outside the standard table
        // falls back to a raw tick count so the user still gets a hint.
        const int q = (int) snapTicks;
        const char* label =
            (q == kMidiTicksPerQuarter * 4) ? "Q: 1/1"  :
            (q == kMidiTicksPerQuarter * 2) ? "Q: 1/2"  :
            (q == kMidiTicksPerQuarter)     ? "Q: 1/4"  :
            (q == kMidiTicksPerQuarter / 2) ? "Q: 1/8"  :
            (q == kMidiTicksPerQuarter / 4) ? "Q: 1/16" :
            (q == kMidiTicksPerQuarter / 8) ? "Q: 1/32" :
                                                nullptr;
        qLabel = label != nullptr ? juce::String (label)
                                   : "Q: " + juce::String (q) + "t";
    }
    g.setColour (kHeaderText.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.drawText (qLabel,
                 area.getRight() - 60, area.getY(),
                 56, area.getHeight(),
                 juce::Justification::centredRight, false);

    // Color-mode HUD just to the left of the Q label. Same font / tone.
    // 'C' cycles the mode at runtime.
    const char* colorLabel = colorMode == ColorMode::Pitch    ? "C: Pitch"
                            : colorMode == ColorMode::Velocity ? "C: Vel"
                                                                : "C: Chan";
    g.drawText (colorLabel,
                 area.getRight() - 130, area.getY(),
                 64, area.getHeight(),
                 juce::Justification::centredRight, false);

    // Scale label, left of the colour HUD. "S: Off" when the scale-
    // highlight overlay is disabled; otherwise "S: <root> <mode>"
    // (e.g. "S: D Min"). 'S' opens the picker.
    juce::String scaleLabel;
    if (scale == Scale::Off)
    {
        scaleLabel = "S: Off";
    }
    else
    {
        static const char* roots[] = { "C", "C#", "D", "D#", "E", "F", "F#",
                                         "G", "G#", "A", "A#", "B" };
        const char* modeShort =
            scale == Scale::Major      ? "Maj" :
            scale == Scale::Minor      ? "Min" :
            scale == Scale::Dorian     ? "Dor" :
            scale == Scale::Phrygian   ? "Phr" :
            scale == Scale::Lydian     ? "Lyd" :
            scale == Scale::Mixolydian ? "Mix" :
            scale == Scale::Locrian    ? "Loc" : "";
        scaleLabel = "S: " + juce::String (roots[scaleRoot]) + " " + modeShort;
    }
    g.drawText (scaleLabel,
                 area.getRight() - 210, area.getY(),
                 76, area.getHeight(),
                 juce::Justification::centredRight, false);
}

void PianoRollComponent::paintKeyboard (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (kBgDark);
    g.fillRect (area);

    for (int n = 0; n < kNumKeys; ++n)
    {
        const int y = yForNoteNumber (n);
        if (y + kNoteHeight < area.getY() || y > area.getBottom()) continue;
        const bool black = isBlackKey (n);
        g.setColour (black ? kKeyBlack : kKeyWhite);
        const int rectX = area.getX();
        const int rectW = black ? area.getWidth() - 14 : area.getWidth() - 1;
        g.fillRect (rectX, y, rectW, kNoteHeight - 1);

        if (! black && (n % 12 == 0))
        {
            g.setColour (kKeyText);
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::plain)));
            g.drawText (noteNameForC (n),
                         area.getRight() - 22, y, 20, kNoteHeight,
                         juce::Justification::centredRight, false);
        }
    }

    // Right-edge separator between keyboard and grid.
    g.setColour (kGridLine);
    g.drawVerticalLine (area.getRight() - 1,
                         (float) area.getY(), (float) area.getBottom());
}

void PianoRollComponent::paintNotes (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto* r = region();
    if (r == nullptr) return;
    g.reduceClipRegion (area);

    for (int i = 0; i < (int) r->notes.size(); ++i)
    {
        const auto& n = r->notes[(size_t) i];
        const int x  = xForTick (n.startTick);
        const int x2 = xForTick (n.startTick + n.lengthInTicks);
        const int y  = yForNoteNumber (n.noteNumber);
        if (x2 < area.getX() || x > area.getRight()) continue;
        if (y + kNoteHeight < area.getY() || y > area.getBottom()) continue;

        const auto rect = juce::Rectangle<int> (x, y + 1, juce::jmax (2, x2 - x), kNoteHeight - 2);
        const bool selected = isNoteSelected (i);
        const auto fill = selected ? kNoteSelected : colourForNote (n);
        g.setColour (fill);
        g.fillRoundedRectangle (rect.toFloat(), 1.5f);
        g.setColour (kNoteEdge);
        g.drawRoundedRectangle (rect.toFloat(), 1.5f, 0.8f);
    }
}

void PianoRollComponent::paintVelocityStrip (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff121218));
    g.fillRect (area);
    g.setColour (kGridLine);
    g.drawHorizontalLine (area.getY(),
                            (float) area.getX(), (float) area.getRight());

    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;
    g.reduceClipRegion (area);

    // Floor + ceiling baselines so the strip reads as a "level meter" -
    // 0 / 64 / 127 lines act as soft reference rules behind the bars.
    const float ax = (float) area.getX();
    const float ay = (float) area.getY();
    const float aw = (float) area.getWidth();
    const float ah = (float) area.getHeight();
    g.setColour (kKeyOctaveLine.withAlpha (0.10f));
    g.drawHorizontalLine ((int) (ay + ah * 0.5f), ax, ax + aw);

    constexpr float kBarWidth = 4.0f;
    const float baseY = ay + ah - 2.0f;
    const float topY  = ay + 4.0f;
    const float span  = baseY - topY;

    for (int i = 0; i < (int) r->notes.size(); ++i)
    {
        const auto& n = r->notes[(size_t) i];
        const int nx = xForTick (n.startTick);
        if (nx < area.getX() - 4 || nx > area.getRight() + 4) continue;

        const float frac = juce::jlimit (0.0f, 1.0f, (float) n.velocity / 127.0f);
        const float top  = baseY - span * frac;
        const auto bar = juce::Rectangle<float> ((float) nx - kBarWidth * 0.5f,
                                                    top,
                                                    kBarWidth,
                                                    baseY - top);

        const bool selected = isNoteSelected (i);
        const auto barColour = selected ? kNoteSelected
                                          : colourForNote (n).withMultipliedBrightness (1.05f);
        g.setColour (barColour);
        g.fillRoundedRectangle (bar, 1.0f);
        g.setColour (kNoteEdge.withAlpha (0.6f));
        g.drawRoundedRectangle (bar, 1.0f, 0.5f);
    }
}

bool PianoRollComponent::isNoteSelected (int idx) const noexcept
{
    return std::binary_search (selectedNotes.begin(), selectedNotes.end(), idx);
}

void PianoRollComponent::clearSelection()
{
    selectedNotes.clear();
    dragAnchor = -1;
}

void PianoRollComponent::selectOnly (int idx)
{
    selectedNotes.clear();
    if (idx >= 0) selectedNotes.push_back (idx);
}

void PianoRollComponent::addToSelection (int idx)
{
    if (idx < 0) return;
    auto it = std::lower_bound (selectedNotes.begin(), selectedNotes.end(), idx);
    if (it == selectedNotes.end() || *it != idx)
        selectedNotes.insert (it, idx);
}

void PianoRollComponent::toggleSelected (int idx)
{
    if (idx < 0) return;
    auto it = std::lower_bound (selectedNotes.begin(), selectedNotes.end(), idx);
    if (it != selectedNotes.end() && *it == idx)
        selectedNotes.erase (it);
    else
        selectedNotes.insert (it, idx);
}

void PianoRollComponent::beginGroupDrag (int anchorIdx)
{
    dragAnchor = anchorIdx;
    dragSnapshots.clear();
    const auto* r = region();
    if (r == nullptr) return;
    dragSnapshots.reserve (selectedNotes.size());
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        const auto& n = r->notes[(size_t) idx];
        dragSnapshots.push_back ({ n.startTick, n.noteNumber, n.lengthInTicks });
    }
}

void PianoRollComponent::applyGroupMove (juce::int64 deltaTicks, int deltaPitch)
{
    auto* r = region();
    if (r == nullptr) return;
    if (selectedNotes.size() != dragSnapshots.size()) return;

    // Clamp the group delta so no note ends up out of bounds. Without
    // this, individual per-note clamps would let some notes "stick" at
    // the boundary while others kept moving, breaking relative spacing.
    juce::int64 minStart   = std::numeric_limits<juce::int64>::max();
    juce::int64 maxEnd     = std::numeric_limits<juce::int64>::min();
    int         minPitch   = 127;
    int         maxPitch   = 0;
    for (const auto& s : dragSnapshots)
    {
        minStart  = juce::jmin (minStart, s.startTick);
        maxEnd    = juce::jmax (maxEnd,   s.startTick + s.lengthInTicks);
        minPitch  = juce::jmin (minPitch, s.noteNumber);
        maxPitch  = juce::jmax (maxPitch, s.noteNumber);
    }
    const juce::int64 maxDeltaTickRight = r->lengthInTicks - maxEnd;
    deltaTicks = juce::jlimit (-minStart, maxDeltaTickRight, deltaTicks);
    deltaPitch = juce::jlimit (-minPitch, 127 - maxPitch, deltaPitch);

    for (size_t i = 0; i < selectedNotes.size(); ++i)
    {
        const int idx = selectedNotes[i];
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        auto& n = r->notes[(size_t) idx];
        n.startTick  = dragSnapshots[i].startTick + deltaTicks;
        n.noteNumber = dragSnapshots[i].noteNumber + deltaPitch;
    }
}

void PianoRollComponent::transposeSelected (int semitones)
{
    auto* r = region();
    if (r == nullptr || selectedNotes.empty()) return;
    for (int idx : selectedNotes)
    {
        if (idx < 0 || idx >= (int) r->notes.size()) continue;
        auto& n = r->notes[(size_t) idx];
        n.noteNumber = juce::jlimit (0, kNumKeys - 1, n.noteNumber + semitones);
    }
}

void PianoRollComponent::quantizeSelected (juce::int64 gridTicks, float strength)
{
    auto* r = region();
    if (r == nullptr || gridTicks <= 0) return;
    strength = juce::jlimit (0.0f, 1.0f, strength);
    // No selection → quantize every note in the region. Matches Logic
    // and Reaper (Q on empty selection = quantize all).
    auto applyToOne = [gridTicks, strength] (MidiNote& n)
    {
        const auto orig    = n.startTick;
        const auto snapped = ((orig + gridTicks / 2) / gridTicks) * gridTicks;
        const auto blended = orig + (juce::int64) std::round (
            (double) (snapped - orig) * strength);
        n.startTick = juce::jmax ((juce::int64) 0, blended);
    };
    if (selectedNotes.empty())
    {
        for (auto& n : r->notes) applyToOne (n);
    }
    else
    {
        for (int idx : selectedNotes)
        {
            if (idx < 0 || idx >= (int) r->notes.size()) continue;
            applyToOne (r->notes[(size_t) idx]);
        }
    }
}

bool PianoRollComponent::isInScale (int noteNumber) const noexcept
{
    if (scale == Scale::Off) return true;
    static constexpr int kMajorIntervals[]      = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kMinorIntervals[]      = { 0, 2, 3, 5, 7, 8, 10 };
    static constexpr int kDorianIntervals[]     = { 0, 2, 3, 5, 7, 9, 10 };
    static constexpr int kPhrygianIntervals[]   = { 0, 1, 3, 5, 7, 8, 10 };
    static constexpr int kLydianIntervals[]     = { 0, 2, 4, 6, 7, 9, 11 };
    static constexpr int kMixolydianIntervals[] = { 0, 2, 4, 5, 7, 9, 10 };
    static constexpr int kLocrianIntervals[]    = { 0, 1, 3, 5, 6, 8, 10 };
    const int* table = nullptr;
    int len = 7;
    switch (scale)
    {
        case Scale::Major:      table = kMajorIntervals;      break;
        case Scale::Minor:      table = kMinorIntervals;      break;
        case Scale::Dorian:     table = kDorianIntervals;     break;
        case Scale::Phrygian:   table = kPhrygianIntervals;   break;
        case Scale::Lydian:     table = kLydianIntervals;     break;
        case Scale::Mixolydian: table = kMixolydianIntervals; break;
        case Scale::Locrian:    table = kLocrianIntervals;    break;
        case Scale::Off: default: return true;
    }
    const int rel = ((noteNumber - scaleRoot) % 12 + 12) % 12;
    for (int i = 0; i < len; ++i) if (table[i] == rel) return true;
    return false;
}

void PianoRollComponent::showQuantizePopup()
{
    juce::PopupMenu m;
    // Each item is "<grid> @ <strength>". 100% items first (full snap),
    // then humanise variants. Selected/all-notes is implicit in
    // quantizeSelected (empty selection = whole region).
    struct Choice { const char* label; juce::int64 grid; float strength; };
    const Choice choices[] = {
        { "1/4 @ 100%",   kMidiTicksPerQuarter,        1.00f },
        { "1/8 @ 100%",   kMidiTicksPerQuarter / 2,    1.00f },
        { "1/16 @ 100%",  kMidiTicksPerQuarter / 4,    1.00f },
        { "1/32 @ 100%",  kMidiTicksPerQuarter / 8,    1.00f },
        { nullptr, 0, 0.0f },                          // separator
        { "1/16 @ 75%",   kMidiTicksPerQuarter / 4,    0.75f },
        { "1/16 @ 50%",   kMidiTicksPerQuarter / 4,    0.50f },
        { "1/8 @ 50%",    kMidiTicksPerQuarter / 2,    0.50f },
    };
    for (int i = 0; i < (int) (sizeof (choices) / sizeof (choices[0])); ++i)
    {
        if (choices[i].label == nullptr) { m.addSeparator(); continue; }
        m.addItem (i + 1, choices[i].label);
    }
    juce::Component::SafePointer<PianoRollComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [safe, choices] (int chosen)
        {
            if (chosen <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            const auto& c = choices[chosen - 1];
            self->quantizeSelected (c.grid, c.strength);
            self->repaint();
        });
}

void PianoRollComponent::showScalePopup()
{
    juce::PopupMenu m;
    static const char* roots[] = { "C", "C#", "D", "D#", "E", "F", "F#",
                                     "G", "G#", "A", "A#", "B" };
    static const char* scaleNames[] = { "Off", "Major", "Minor", "Dorian",
                                          "Phrygian", "Lydian", "Mixolydian", "Locrian" };
    // Item ID encoding: root * 100 + scaleType + 1. ScaleType 0 = Off
    // ignores root. Add an "Off" entry at the top for quick clear.
    m.addItem (1, "Off", true, scale == Scale::Off);
    m.addSeparator();
    for (int s = 1; s < 8; ++s)
    {
        juce::PopupMenu sub;
        for (int r = 0; r < 12; ++r)
        {
            const int id = r * 100 + s + 1;
            const bool ticked = (scale != Scale::Off
                                  && (int) scale == s
                                  && scaleRoot == r);
            sub.addItem (id, juce::String (roots[r]), true, ticked);
        }
        m.addSubMenu (juce::String (scaleNames[s]), sub);
    }
    juce::Component::SafePointer<PianoRollComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [safe] (int chosen)
        {
            if (chosen <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            if (chosen == 1)
            {
                self->scale = Scale::Off;
            }
            else
            {
                const int s = (chosen - 1) % 100;        // 1..7
                const int r = (chosen - 1) / 100;        // 0..11
                self->scale     = (Scale) s;
                self->scaleRoot = r;
            }
            self->repaint();
        });
}

juce::Colour PianoRollComponent::colourForNote (const MidiNote& n) const noexcept
{
    const float velFactor = juce::jlimit (0.0f, 1.0f, (float) n.velocity / 127.0f);
    const float bri = 0.55f + 0.45f * velFactor;   // 0.55..1.00 brightness from velocity

    switch (colorMode)
    {
        case ColorMode::Pitch:
        {
            const int pc = ((n.noteNumber % 12) + 12) % 12;
            return kPitchClassPalette[pc].withMultipliedBrightness (bri);
        }
        case ColorMode::Channel:
        {
            // 16-way hue cycle. Channels 1..16 -> hue 0..15/16. Modulo 16
            // so channel 0 (running-status / unset) doesn't divide-by-zero.
            const int idx = juce::jlimit (1, 16, n.channel) - 1;
            const float hue = (float) idx / 16.0f;
            return juce::Colour::fromHSV (hue, 0.65f, bri, 1.0f);
        }
        case ColorMode::Velocity:
        default:
            return kNoteFill.withMultipliedBrightness (bri);
    }
}

void PianoRollComponent::paintCcStrip (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff0e0e14));
    g.fillRect (area);
    g.setColour (kGridLine);
    g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());

    const auto* r = region();
    if (r == nullptr || r->lengthInTicks <= 0) return;
    g.reduceClipRegion (area);

    const float ax = (float) area.getX();
    const float ay = (float) area.getY();
    const float aw = (float) area.getWidth();
    const float ah = (float) area.getHeight();
    // 0 / 64 / 127 reference rules so the user can read approximate
    // values without dragging.
    g.setColour (kKeyOctaveLine.withAlpha (0.10f));
    g.drawHorizontalLine ((int) (ay + ah * 0.5f), ax, ax + aw);

    const float baseY = ay + ah - 2.0f;
    const float topY  = ay + 4.0f;
    const float span  = baseY - topY;
    constexpr float kBarWidth = 3.0f;

    // Use a fixed accent for CC bars - visual contrast against the
    // pitch-coloured note grid above. (Coloring CCs by velocity-of-
    // associated-note isn't meaningful; CCs are independent events.)
    const auto barFill = juce::Colour (0xff60c0a8);

    for (int i = 0; i < (int) r->ccs.size(); ++i)
    {
        const auto& c = r->ccs[(size_t) i];
        if (c.controller != activeCcController) continue;
        const int nx = xForTick (c.atTick);
        if (nx < area.getX() - 4 || nx > area.getRight() + 4) continue;

        const float frac = juce::jlimit (0.0f, 1.0f, (float) c.value / 127.0f);
        const float top  = baseY - span * frac;
        const auto bar = juce::Rectangle<float> ((float) nx - kBarWidth * 0.5f,
                                                    top, kBarWidth, baseY - top);
        g.setColour (barFill);
        g.fillRect (bar);
    }

    // Header label - "CC: <num> (<name>)" pinned to the upper-left of
    // the strip. Names cover the most-used controllers; unknown CCs
    // fall back to "CC <num>".
    const char* name =
        activeCcController == 1   ? "Mod"     :
        activeCcController == 7   ? "Volume"  :
        activeCcController == 11  ? "Express" :
        activeCcController == 64  ? "Sustain" :
        activeCcController == 74  ? "Filter"  :
                                     nullptr;
    juce::String label;
    if (name != nullptr) label = "CC " + juce::String (activeCcController) + " (" + name + ")";
    else                  label = "CC " + juce::String (activeCcController);
    g.setColour (kHeaderText.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    g.drawText (label, area.getX() + 6, area.getY() + 2,
                 area.getWidth() - 12, 14,
                 juce::Justification::centredLeft, false);
}

int PianoRollComponent::hitTestCcBar (int x, juce::Rectangle<int> stripArea) const
{
    const auto* r = region();
    if (r == nullptr) return -1;
    constexpr int kHitSlopPx = 5;
    // Walk newest-first so an overlapping bar prefers the most-recent
    // event - matches the painter's z-order.
    for (int i = (int) r->ccs.size() - 1; i >= 0; --i)
    {
        const auto& c = r->ccs[(size_t) i];
        if (c.controller != activeCcController) continue;
        const int nx = xForTick (c.atTick);
        if (std::abs (x - nx) <= kHitSlopPx
            && x >= stripArea.getX() && x <= stripArea.getRight()) return i;
    }
    return -1;
}

int PianoRollComponent::hitTestVelocityBar (int x, juce::Rectangle<int> stripArea) const
{
    const auto* r = region();
    if (r == nullptr) return -1;
    constexpr int kHitSlopPx = 4;   // bar is ~4 px wide; same in either side
    // Walk newest-first so an overlapping bar prefers the most-recent
    // note (matches the painter's draw order).
    for (int i = (int) r->notes.size() - 1; i >= 0; --i)
    {
        const auto& n = r->notes[(size_t) i];
        const int nx = xForTick (n.startTick);
        if (std::abs (x - nx) <= kHitSlopPx
            && x >= stripArea.getX() && x <= stripArea.getRight()) return i;
    }
    return -1;
}

// Snap a tick to the nearest grid step. Pure helper so create / move /
// resize all use the same rounding rule. Negative ticks clamp to zero
// (the region origin); 0 snapTicks is the "no snap" sentinel.
static juce::int64 snapTick (juce::int64 t, juce::int64 step)
{
    if (step <= 0) return juce::jmax ((juce::int64) 0, t);
    if (t < 0) return 0;
    const auto half = step / 2;
    return ((t + half) / step) * step;
}

int PianoRollComponent::hitTestNote (int x, int y, bool& onRightEdge) const
{
    onRightEdge = false;
    const auto* r = region();
    if (r == nullptr) return -1;
    constexpr int kEdgeGrabPx = 5;
    // Walk newest-first so overlaid notes prefer the most-recently-added
    // (matches the painter's draw order which currently is in-order; for
    // overlaps in 4c, we'll switch to a z-order list).
    for (int i = (int) r->notes.size() - 1; i >= 0; --i)
    {
        const auto& n = r->notes[(size_t) i];
        const int x0 = xForTick (n.startTick);
        const int x1 = xForTick (n.startTick + n.lengthInTicks);
        const int yy = yForNoteNumber (n.noteNumber);
        if (x < x0 || x > x1) continue;
        if (y < yy || y > yy + kNoteHeight) continue;
        onRightEdge = (x1 - x) <= kEdgeGrabPx;
        return i;
    }
    return -1;
}

void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    auto* r = region();
    if (r == nullptr) return;

    const bool extendSelection = e.mods.isShiftDown() || e.mods.isCommandDown();

    // Bottom-strip layout: CC lane at the very bottom, velocity lane
    // above it. Compute both rects inline so they match paint().
    const auto ccArea = juce::Rectangle<int> (
        kKeyboardWidth, getHeight() - kCcStripH,
        getWidth() - kKeyboardWidth, kCcStripH);
    const auto velocityArea = juce::Rectangle<int> (
        kKeyboardWidth, ccArea.getY() - kVelocityStripH,
        getWidth() - kKeyboardWidth, kVelocityStripH);

    // CC lane click: edit existing bar, or create a new CC event at
    // the clicked tick + value. Drag-y updates the value continuously.
    if (ccArea.contains (e.x, e.y))
    {
        const float frac = juce::jlimit (0.0f, 1.0f,
            1.0f - ((float) (e.y - ccArea.getY())
                      / (float) juce::jmax (1, ccArea.getHeight())));
        const int value = juce::jlimit (0, 127, (int) std::round (frac * 127.0f));

        int hit = hitTestCcBar (e.x, ccArea);
        if (hit < 0)
        {
            // Empty CC space: create a new event at the snapped tick.
            MidiCc c;
            c.channel    = 1;
            c.controller = activeCcController;
            c.value      = value;
            const auto rawTick = tickForX (e.x);
            c.atTick = juce::jlimit<juce::int64> (0,
                juce::jmax ((juce::int64) 0, r->lengthInTicks - 1),
                snapTick (rawTick, snapTicks));
            r->ccs.push_back (c);
            hit = (int) r->ccs.size() - 1;
        }
        else
        {
            r->ccs[(size_t) hit].value = value;
        }
        draggedCcIdx = hit;
        dragMode = DragMode::EditCcValue;
        repaint();
        return;
    }
    if (velocityArea.contains (e.x, e.y))
    {
        const int barIdx = hitTestVelocityBar (e.x, velocityArea);
        if (barIdx >= 0)
        {
            selectOnly (barIdx);
            dragAnchor = barIdx;
            dragMode = DragMode::EditVelocity;
            const float frac = juce::jlimit (0.0f, 1.0f,
                1.0f - ((float) (e.y - velocityArea.getY())
                          / (float) juce::jmax (1, velocityArea.getHeight())));
            r->notes[(size_t) barIdx].velocity =
                juce::jlimit (1, 127, (int) std::round (frac * 127.0f));
            repaint();
        }
        return;
    }

    bool onEdge = false;
    const int hit = hitTestNote (e.x, e.y, onEdge);

    if (hit >= 0)
    {
        // Shift / Cmd-click on a note: toggle in/out of selection
        // without entering drag mode. Matches Logic / Reaper.
        if (extendSelection)
        {
            toggleSelected (hit);
            dragMode = DragMode::None;
            repaint();
            return;
        }

        // Plain click on a note: if it was already selected, keep the
        // existing multi-selection so a group drag works. Otherwise
        // collapse to single-note selection. Either way, snapshot the
        // selection's drag origins so MoveNote can apply a group delta.
        if (! isNoteSelected (hit))
            selectOnly (hit);

        const auto& n = r->notes[(size_t) hit];
        dragOriginTick    = tickForX (e.x) - n.startTick;
        dragOriginNoteNum = n.noteNumber;
        dragNoteStartTick = n.startTick;
        dragNoteLenTicks  = n.lengthInTicks;
        dragMode = onEdge ? DragMode::ResizeNote : DragMode::MoveNote;
        beginGroupDrag (hit);
        repaint();
        return;
    }

    // Empty-grid hit. Two paths:
    //   - Shift / Cmd held: start a rubber-band box-select drag.
    //   - Otherwise: create a new note (existing pencil-on-default).
    if (e.x < kKeyboardWidth || e.y < kHeaderHeight) return;

    if (extendSelection)
    {
        rubberBand = juce::Rectangle<int> (e.x, e.y, 0, 0);
        dragMode = DragMode::BoxSelect;
        // Don't clear existing selection on Shift-drag; the new box
        // ADDS to the current selection on mouseUp. Plain Cmd/Shift-
        // empty-click can't reach here without a drag, so this is the
        // right behaviour for the only path that fires it.
        repaint();
        return;
    }

    MidiNote n;
    n.channel = 1;
    n.noteNumber = noteNumberForY (e.y);
    n.velocity = 100;
    const auto rawStart = juce::jlimit<juce::int64> (0,
        juce::jmax ((juce::int64) 0, r->lengthInTicks - 1), tickForX (e.x));
    n.startTick = juce::jlimit<juce::int64> (0,
        juce::jmax ((juce::int64) 0, r->lengthInTicks - 1),
        snapTick (rawStart, snapTicks));
    n.lengthInTicks = juce::jmin ((juce::int64) kMidiTicksPerQuarter,
                                                  r->lengthInTicks - n.startTick);
    if (n.lengthInTicks <= 0) return;
    r->notes.push_back (n);
    const int newIdx = (int) r->notes.size() - 1;
    selectOnly (newIdx);
    dragAnchor        = newIdx;
    dragNoteStartTick = n.startTick;
    dragNoteLenTicks  = n.lengthInTicks;
    dragOriginTick    = 0;
    dragOriginNoteNum = n.noteNumber;
    dragMode = DragMode::ResizeNote;
    repaint();
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    auto* r = region();
    if (r == nullptr) return;

    if (dragMode == DragMode::BoxSelect)
    {
        // Rubber band tracks from mouseDown origin to current point.
        // Keep the rect in screen coords so finalize can intersect
        // against each note's painted rect directly.
        const int x0 = e.getMouseDownX();
        const int y0 = e.getMouseDownY();
        rubberBand = juce::Rectangle<int> (juce::jmin (x0, e.x), juce::jmin (y0, e.y),
                                              std::abs (e.x - x0), std::abs (e.y - y0));
        repaint();
        return;
    }

    if (dragMode == DragMode::EditCcValue)
    {
        if (draggedCcIdx < 0 || draggedCcIdx >= (int) r->ccs.size()) return;
        const auto ccArea = juce::Rectangle<int> (
            kKeyboardWidth, getHeight() - kCcStripH,
            getWidth() - kKeyboardWidth, kCcStripH);
        const float frac = juce::jlimit (0.0f, 1.0f,
            1.0f - ((float) (e.y - ccArea.getY())
                      / (float) juce::jmax (1, ccArea.getHeight())));
        r->ccs[(size_t) draggedCcIdx].value =
            juce::jlimit (0, 127, (int) std::round (frac * 127.0f));
        repaint();
        return;
    }

    if (dragAnchor < 0 || dragAnchor >= (int) r->notes.size()) return;
    auto& anchor = r->notes[(size_t) dragAnchor];

    if (dragMode == DragMode::MoveNote)
    {
        // Compute the snapped delta against the anchor, then apply that
        // same delta to every selected note via applyGroupMove. This
        // keeps relative spacing across the whole selection.
        const auto newAnchorStart = snapTick (tickForX (e.x) - dragOriginTick, snapTicks);
        const int  newAnchorPitch = juce::jlimit (0, kNumKeys - 1, noteNumberForY (e.y));
        const auto deltaTicks = newAnchorStart - dragNoteStartTick;
        const int  deltaPitch = newAnchorPitch - dragOriginNoteNum;
        applyGroupMove (deltaTicks, deltaPitch);
        repaint();
    }
    else if (dragMode == DragMode::EditVelocity)
    {
        const auto velocityArea = juce::Rectangle<int> (
            kKeyboardWidth, getHeight() - kVelocityStripH,
            getWidth() - kKeyboardWidth, kVelocityStripH);
        const float frac = juce::jlimit (0.0f, 1.0f,
            1.0f - ((float) (e.y - velocityArea.getY())
                      / (float) juce::jmax (1, velocityArea.getHeight())));
        anchor.velocity = juce::jlimit (1, 127, (int) std::round (frac * 127.0f));
        repaint();
    }
    else if (dragMode == DragMode::ResizeNote)
    {
        // Resize the anchor note only; multi-resize is ambiguous and
        // most DAWs match this behaviour.
        const auto rawEnd  = tickForX (e.x);
        const auto snapped = snapTick (rawEnd, snapTicks);
        const auto endTick = juce::jlimit<juce::int64> (
            anchor.startTick + 1, r->lengthInTicks, snapped);
        anchor.lengthInTicks = endTick - anchor.startTick;
        repaint();
    }
}

void PianoRollComponent::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::BoxSelect)
    {
        // Finalise selection: every note whose painted rect intersects
        // the rubber band joins the existing selection. We work in
        // screen coords because that's what the rubber band tracks.
        const auto* r = region();
        if (r != nullptr && ! rubberBand.isEmpty())
        {
            for (int i = 0; i < (int) r->notes.size(); ++i)
            {
                const auto& n = r->notes[(size_t) i];
                const int x0 = xForTick (n.startTick);
                const int x1 = xForTick (n.startTick + n.lengthInTicks);
                const int yy = yForNoteNumber (n.noteNumber);
                const auto noteRect = juce::Rectangle<int> (x0, yy + 1,
                                                              juce::jmax (2, x1 - x0),
                                                              kNoteHeight - 2);
                if (rubberBand.intersects (noteRect))
                    addToSelection (i);
            }
        }
        rubberBand = {};
        repaint();
    }
    dragMode = DragMode::None;
    dragAnchor = -1;
    draggedCcIdx = -1;
    dragSnapshots.clear();
}

bool PianoRollComponent::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::backspaceKey || k == juce::KeyPress::deleteKey)
    {
        auto* r = region();
        if (r == nullptr || selectedNotes.empty()) return false;
        // Erase in descending index order so earlier indices stay valid
        // through the loop. selectedNotes is sorted ascending, so walk
        // backwards.
        for (auto it = selectedNotes.rbegin(); it != selectedNotes.rend(); ++it)
        {
            const int idx = *it;
            if (idx >= 0 && idx < (int) r->notes.size())
                r->notes.erase (r->notes.begin() + idx);
        }
        clearSelection();
        repaint();
        return true;
    }
    if (k == juce::KeyPress::escapeKey)
    {
        // First Esc clears a non-empty selection (matches Reaper's
        // behaviour); a second Esc with nothing selected closes the
        // overlay. Lets the user deselect without dismissing.
        if (! selectedNotes.empty())
        {
            clearSelection();
            repaint();
            return true;
        }
        if (onCloseRequested) onCloseRequested();
        return true;
    }
    // Arrow-key transpose. Up/Down = ±1 semitone; Shift+Up/Down = ±12.
    // Acts on every selected note. Left/Right are reserved for future
    // tick-nudge (Phase 2c quantize work).
    if (! selectedNotes.empty())
    {
        if (k == juce::KeyPress::upKey)
        {
            transposeSelected (k.getModifiers().isShiftDown() ? 12 : 1);
            repaint();
            return true;
        }
        if (k == juce::KeyPress::downKey)
        {
            transposeSelected (k.getModifiers().isShiftDown() ? -12 : -1);
            repaint();
            return true;
        }
    }
    // Quantize shortcuts. Number keys swap the snap resolution; '0' = free.
    // Values are denominators of a quarter note (1 = whole at 4/4, 4 = 16th).
    // Picked the most common entry grids; the user can extend with triplet
    // / dotted variants in a polish pass.
    if (k.getKeyCode() == '1') { snapTicks = kMidiTicksPerQuarter * 4; repaint(); return true; }   // whole
    if (k.getKeyCode() == '2') { snapTicks = kMidiTicksPerQuarter * 2; repaint(); return true; }   // half
    if (k.getKeyCode() == '3') { snapTicks = kMidiTicksPerQuarter;     repaint(); return true; }   // quarter
    if (k.getKeyCode() == '4') { snapTicks = kMidiTicksPerQuarter / 2; repaint(); return true; }   // 8th
    if (k.getKeyCode() == '5') { snapTicks = kMidiTicksPerQuarter / 4; repaint(); return true; }   // 16th (default)
    if (k.getKeyCode() == '6') { snapTicks = kMidiTicksPerQuarter / 8; repaint(); return true; }   // 32nd
    if (k.getKeyCode() == '0') { snapTicks = 0;                       repaint(); return true; }    // free
    // 'C' cycles the colour mode: Pitch -> Velocity -> Channel -> Pitch.
    // 'C' was free in the existing key map (the 1..6/0 family + delete +
    // Esc are the only bindings). Lowercase falls through if Caps Lock is
    // off, so we match either case.
    if (k.getKeyCode() == 'C')
    {
        colorMode = colorMode == ColorMode::Pitch    ? ColorMode::Velocity
                  : colorMode == ColorMode::Velocity ? ColorMode::Channel
                                                      : ColorMode::Pitch;
        repaint();
        return true;
    }
    // 'L' cycles the active CC controller: 1 (mod) -> 7 (vol) -> 11
    // (express) -> 64 (sustain) -> 74 (filter) -> back to 1. Covers the
    // most-used continuous controllers; uncommon ones can still be
    // captured via Record (the region's ccs vector holds them all) and
    // viewed by extending this rotation later.
    if (k.getKeyCode() == 'L')
    {
        activeCcController =
            activeCcController == 1   ?  7 :
            activeCcController == 7   ? 11 :
            activeCcController == 11  ? 64 :
            activeCcController == 64  ? 74 :
                                          1;
        repaint();
        return true;
    }
    // 'Q' opens the quantize popup. Action runs on selected notes (or
    // every note if nothing selected).
    if (k.getKeyCode() == 'Q')
    {
        showQuantizePopup();
        return true;
    }
    // 'S' opens the scale-highlight picker (root × mode).
    if (k.getKeyCode() == 'S')
    {
        showScalePopup();
        return true;
    }
    return false;
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e,
                                          const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        // Horizontal zoom anchored on the cursor's tick: capture the tick
        // under the cursor BEFORE changing pixelsPerTick, then adjust
        // scrollX so the same tick stays under the cursor afterwards.
        // This makes zoom feel like it's pulling the timeline through
        // the cursor instead of resetting to tick 0.
        const auto cursorTickBefore = tickForX (e.x);
        const float factor = w.deltaY > 0.0f ? 1.15f : (1.0f / 1.15f);
        pixelsPerTick = juce::jlimit (0.005f, 1.0f, pixelsPerTick * factor);
        const int newCursorPx = (int) std::round ((double) cursorTickBefore * pixelsPerTick);
        scrollX = juce::jmax (0, newCursorPx - (e.x - kKeyboardWidth));
        repaint();
        return;
    }

    // Horizontal scroll: trackpad delta-X always counts; shift+wheel maps
    // a vertical wheel onto horizontal motion. Vertical scroll is the
    // default.
    if (std::abs (w.deltaX) > 0.001f || e.mods.isShiftDown())
    {
        const float dx = std::abs (w.deltaX) > 0.001f ? w.deltaX : w.deltaY;
        const int dxPx = (int) (-dx * 120.0f);
        scrollX = juce::jmax (0, scrollX + dxPx);
        repaint();
        return;
    }
    // Vertical scroll. wheel deltaY > 0 = scroll up (show higher notes).
    const int delta = (int) (-w.deltaY * 60.0f);
    const int maxScroll = juce::jmax (0,
        kFullGridHeight - (getHeight() - kHeaderHeight - kVelocityStripH));
    scrollY = juce::jlimit (0, maxScroll, scrollY + delta);
    repaint();
}
} // namespace focal
