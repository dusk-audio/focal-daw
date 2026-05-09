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
    const auto velocityArea = juce::Rectangle<int> (kKeyboardWidth,
                                                       bounds.getBottom() - kVelocityStripH,
                                                       bounds.getWidth() - kKeyboardWidth,
                                                       kVelocityStripH);
    const auto keyboardArea = juce::Rectangle<int> (0, kHeaderHeight,
                                                       kKeyboardWidth,
                                                       bounds.getHeight() - kHeaderHeight);
    const auto gridArea    = juce::Rectangle<int> (kKeyboardWidth, kHeaderHeight,
                                                     bounds.getWidth() - kKeyboardWidth,
                                                     bounds.getHeight() - kHeaderHeight - kVelocityStripH);

    paintNoteGrid     (g, gridArea);
    paintBeatRuler    (g, headerArea);
    paintKeyboard     (g, keyboardArea);
    paintNotes        (g, gridArea);
    paintVelocityStrip (g, velocityArea);
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
    const int ticksPerBar = kMidiTicksPerQuarter * beatsPerBar;
    const int totalBars = (int) (r->lengthInTicks / ticksPerBar) + 1;

    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    g.setColour (kHeaderText);
    for (int bar = 0; bar <= totalBars; ++bar)
    {
        const int bx = xForTick ((juce::int64) bar * ticksPerBar);
        if (bx < kKeyboardWidth || bx > area.getRight()) continue;
        g.drawText (juce::String (bar + 1), bx + 2, area.getY(),
                     40, area.getHeight(), juce::Justification::centredLeft, false);
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
        const bool selected = (i == selectedNote);
        // Velocity affects brightness (1..127 -> 0.4..1.0).
        const float velFactor = juce::jlimit (0.0f, 1.0f, (float) n.velocity / 127.0f);
        const auto fill = (selected ? kNoteSelected : kNoteFill)
                              .withMultipliedBrightness (0.6f + 0.4f * velFactor);
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

        const bool selected = (i == selectedNote);
        g.setColour ((selected ? kNoteSelected : kNoteFill)
                          .withMultipliedBrightness (0.9f));
        g.fillRoundedRectangle (bar, 1.0f);
        g.setColour (kNoteEdge.withAlpha (0.6f));
        g.drawRoundedRectangle (bar, 1.0f, 0.5f);
    }
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

    // Velocity strip click: pick the note whose bar was hit (or skip),
    // then enter EditVelocity drag mode so the user can drag up/down to
    // set 0..127. We compute the strip rect inline so it stays in sync
    // with paint() which derives the same bounds.
    const auto velocityArea = juce::Rectangle<int> (
        kKeyboardWidth, getHeight() - kVelocityStripH,
        getWidth() - kKeyboardWidth, kVelocityStripH);
    if (velocityArea.contains (e.x, e.y))
    {
        const int barIdx = hitTestVelocityBar (e.x, velocityArea);
        if (barIdx >= 0)
        {
            selectedNote = barIdx;
            dragMode = DragMode::EditVelocity;
            // Set velocity from the click's vertical position so a single
            // click already commits a value (no drag required).
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
        selectedNote = hit;
        const auto& n = r->notes[(size_t) hit];
        dragOriginTick    = tickForX (e.x) - n.startTick;
        dragOriginNoteNum = n.noteNumber;
        dragNoteStartTick = n.startTick;
        dragNoteLenTicks  = n.lengthInTicks;
        dragMode = onEdge ? DragMode::ResizeNote : DragMode::MoveNote;
        repaint();
        return;
    }

    // Empty-grid click: create a new note at the clicked tick + pitch.
    // Default length = 1 quarter note. Start tick snaps to the grid so the
    // user lands on a beat / sixteenth without fine-positioning.
    if (e.x < kKeyboardWidth || e.y < kHeaderHeight) return;
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
    selectedNote = (int) r->notes.size() - 1;
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
    if (selectedNote < 0 || selectedNote >= (int) r->notes.size()) return;
    auto& n = r->notes[(size_t) selectedNote];

    if (dragMode == DragMode::MoveNote)
    {
        // Snap the start-tick so move drops on the grid; pitch always
        // snaps to a key row since noteNumberForY already quantises to
        // integer keys.
        const auto newTick = snapTick (tickForX (e.x) - dragOriginTick, snapTicks);
        n.startTick = juce::jlimit<juce::int64> (0,
            juce::jmax ((juce::int64) 0, r->lengthInTicks - n.lengthInTicks), newTick);
        n.noteNumber = juce::jlimit (0, kNumKeys - 1, noteNumberForY (e.y));
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
        n.velocity = juce::jlimit (1, 127, (int) std::round (frac * 127.0f));
        repaint();
    }
    else if (dragMode == DragMode::ResizeNote)
    {
        // Snap the END tick so the note's right edge aligns with the grid.
        // Floor is start + 1 (any positive length); ceiling is region length.
        const auto rawEnd  = tickForX (e.x);
        const auto snapped = snapTick (rawEnd, snapTicks);
        const auto endTick = juce::jlimit<juce::int64> (
            n.startTick + 1, r->lengthInTicks, snapped);
        n.lengthInTicks = endTick - n.startTick;
        repaint();
    }
}

void PianoRollComponent::mouseUp (const juce::MouseEvent&)
{
    dragMode = DragMode::None;
}

bool PianoRollComponent::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::backspaceKey || k == juce::KeyPress::deleteKey)
    {
        auto* r = region();
        if (r == nullptr) return false;
        if (selectedNote < 0 || selectedNote >= (int) r->notes.size()) return false;
        r->notes.erase (r->notes.begin() + selectedNote);
        selectedNote = -1;
        repaint();
        return true;
    }
    if (k == juce::KeyPress::escapeKey)
    {
        if (onCloseRequested) onCloseRequested();
        return true;
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
