#include "TapeStrip.h"
#include "../session/RegionEditActions.h"

namespace focal
{
TapeStrip::TapeStrip (Session& s, AudioEngine& e) : session (s), engine (e)
{
    setOpaque (true);
    startTimerHz (30);
    engine.getUndoManager().addChangeListener (this);
}

TapeStrip::~TapeStrip()
{
    engine.getUndoManager().removeChangeListener (this);
}

void TapeStrip::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Undo / redo just mutated the regions - repaint to reflect the swap.
    // The action itself called preparePlayback if stopped, so audio is
    // already aligned with what we'll draw. Selection indices might now
    // point at a region that has been deleted or shifted, so clear them.
    selectedTrack  = -1;
    selectedRegion = -1;
    repaint();
}

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

    // Find the rightmost sample we need to show - either the longest region
    // or the current playhead - then add a margin so there's always blank
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

TapeStrip::RegionHit TapeStrip::hitTestRegion (int x, int y) const noexcept
{
    auto col = tracksColumnBounds();
    if (! col.contains (x, y)) return {};

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto row = rowBounds (t);
        if (! row.contains (x, y)) continue;

        const auto& regions = session.track (t).regions;
        // Iterate from most-recent (last) to first so the topmost region on
        // overlap wins the hit, matching the painter's order.
        for (int i = (int) regions.size() - 1; i >= 0; --i)
        {
            const auto& r = regions[(size_t) i];
            const int x0 = xForSample (r.timelineStart);
            const int x1 = xForSample (r.timelineStart + r.lengthInSamples);
            if (x < x0 || x > x1) continue;

            RegionHit hit;
            hit.track = t;
            hit.regionIdx = i;

            // Take-history badge takes precedence over the trim-start gutter
            // since the two share screen area at the region's top-left. Same
            // bounds as the painter so the click target visibly lines up.
            if (! r.previousTakes.empty())
            {
                const int regionWidth = juce::jmax (2, x1 - x0);
                const int badgeW = juce::jmin (regionWidth, 16);
                const int badgeH = juce::jmin (row.getHeight() - 2, 10);
                const int badgeYTop = row.getY() + 1;
                if (badgeW >= 8 && badgeH >= 6
                    && x >= x0 && x <= x0 + badgeW
                    && y >= badgeYTop && y <= badgeYTop + badgeH)
                {
                    hit.op = RegionOp::TakeBadge;
                    return hit;
                }
            }

            // Edge gutters at the ends - only when the region is wide enough
            // that body + two gutters still leaves a body to drag.
            const int edgeBudget = juce::jmax (1, (x1 - x0) / 4);
            const int gutter     = juce::jmin (kEdgeHitPx, edgeBudget);
            if      (x <= x0 + gutter)  hit.op = RegionOp::TrimStart;
            else if (x >= x1 - gutter)  hit.op = RegionOp::TrimEnd;
            else                        hit.op = RegionOp::Move;
            return hit;
        }
    }
    return {};
}

void TapeStrip::rebuildPlaybackIfStopped()
{
    // Re-prep the playback engine so the next play picks up the edited
    // regions. We avoid this during play/record because preparePlayback
    // closes and re-opens audio readers - momentary I/O on the message
    // thread is safe at rest but risks an xrun mid-transport.
    auto& transport = engine.getTransport();
    if (transport.getState() == Transport::State::Stopped)
        engine.getPlaybackEngine().preparePlayback();
}

void TapeStrip::resized() {}

void TapeStrip::timerCallback()
{
    // Detect track color / name changes and repaint the whole strip if
    // anything changed. Cheap - there are 16 tracks and we just compare
    // a String + a Colour each tick.
    bool stateChanged = false;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& tr = session.track (t);
        if (lastNames[(size_t) t]   != tr.name)   { lastNames[(size_t) t]   = tr.name;   stateChanged = true; }
        if (lastColours[(size_t) t] != tr.colour) { lastColours[(size_t) t] = tr.colour; stateChanged = true; }
    }

    auto& transport = engine.getTransport();
    const bool        loopOn = transport.isLoopEnabled();
    const juce::int64 loopS  = transport.getLoopStart();
    const juce::int64 loopE  = transport.getLoopEnd();
    const bool        pOn    = transport.isPunchEnabled();
    const juce::int64 pIn    = transport.getPunchIn();
    const juce::int64 pOut   = transport.getPunchOut();
    if (loopOn != lastLoopEnabled || loopS != lastLoopStart || loopE != lastLoopEnd
        || pOn != lastPunchEnabled || pIn != lastPunchIn || pOut != lastPunchOut)
    {
        lastLoopEnabled = loopOn;
        lastLoopStart   = loopS;
        lastLoopEnd     = loopE;
        lastPunchEnabled = pOn;
        lastPunchIn     = pIn;
        lastPunchOut    = pOut;
        stateChanged = true;
    }

    if (stateChanged) repaint();

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
    auto ruler = rulerBounds();

    // Left-click on a marker flag → start a marker drag. We DON'T seek
    // here - that's deferred to mouseUp so a click-without-movement seeks
    // and a click-with-movement repositions the marker.
    if (! e.mods.isRightButtonDown())
    {
        if (const int markerIdx = hitTestMarker (e.x, e.y); markerIdx >= 0)
        {
            markerDrag.active           = true;
            markerDrag.moved            = false;
            markerDrag.index            = markerIdx;
            markerDrag.originSample     = session.getMarkers()[(size_t) markerIdx]
                                                .timelineSamples;
            markerDrag.mouseDownSample  = sampleAtX (e.x);
            return;
        }
    }

    // Left-click on an existing loop / punch pill or bar → start a
    // bracket drag. Endpoint pills move that endpoint; the bar in the
    // middle translates the whole range by the drag delta. Tested
    // before ruler-selection so dragging on top of an existing bracket
    // doesn't accidentally start a new selection.
    if (! e.mods.isRightButtonDown())
    {
        if (const auto bh = hitTestBracket (e.x, e.y); bh != BracketHit::None)
        {
            auto& transport = engine.getTransport();
            bracketDrag.active = true;
            bracketDrag.type   = bh;
            bracketDrag.mouseDownSample = sampleAtX (e.x);
            const bool isPunch = (bh == BracketHit::PunchIn
                                || bh == BracketHit::PunchOut
                                || bh == BracketHit::PunchBar);
            bracketDrag.origStart = isPunch ? transport.getPunchIn()  : transport.getLoopStart();
            bracketDrag.origEnd   = isPunch ? transport.getPunchOut() : transport.getLoopEnd();
            return;
        }
    }

    // Left-click in the ruler band (not on a marker / bracket) → start
    // a neutral selection drag. The range is painted as a translucent
    // highlight during drag; on mouseUp we show a popup that asks
    // whether to make it a loop range or a punch range. We don't touch
    // transport state until the user picks.
    if (! e.mods.isRightButtonDown() && ruler.contains (e.x, e.y))
    {
        rulerSelection.active        = true;
        rulerSelection.originSample  = sampleAtX (e.x);
        rulerSelection.currentSample = rulerSelection.originSample;
        repaint();
        return;
    }

    // Right-click on a region opens a region-specific menu (delete, split).
    // Right-click on empty timeline / ruler opens the loop+punch menu.
    if (e.mods.isRightButtonDown())
    {
        const auto hit = hitTestRegion (e.x, e.y);
        if (hit.op != RegionOp::None)
        {
            showRegionContextMenu (hit, e.getScreenPosition());
            return;
        }
    }

    // Right-click anywhere over the ruler or track area opens a context menu
    // for setting loop / punch points and seeking. The clicked sample is
    // captured here so the menu items act on a fixed timeline position even
    // if the user moves the mouse before picking.
    if (e.mods.isRightButtonDown()
        && (col.contains (e.x, e.y) || ruler.contains (e.x, e.y)))
    {
        const auto clickedSample = sampleAtX (e.x);
        auto& transport = engine.getTransport();

        juce::PopupMenu m;
        m.addSectionHeader ("Loop");
        m.addItem ("Set loop in here",  [&transport, clickedSample]
        {
            const auto end = transport.getLoopEnd();
            transport.setLoopRange (clickedSample,
                                     end > clickedSample ? end : clickedSample);
        });
        m.addItem ("Set loop out here", [&transport, clickedSample]
        {
            const auto start = transport.getLoopStart();
            transport.setLoopRange (start < clickedSample ? start : clickedSample,
                                     clickedSample);
        });
        m.addItem ("Clear loop", [&transport]
        {
            transport.setLoopRange (0, 0);
            transport.setLoopEnabled (false);
        });
        m.addSeparator();
        m.addSectionHeader ("Punch");
        m.addItem ("Set punch in here",  [&transport, clickedSample]
        {
            const auto end = transport.getPunchOut();
            transport.setPunchRange (clickedSample,
                                      end > clickedSample ? end : clickedSample);
        });
        m.addItem ("Set punch out here", [&transport, clickedSample]
        {
            const auto start = transport.getPunchIn();
            transport.setPunchRange (start < clickedSample ? start : clickedSample,
                                      clickedSample);
        });
        m.addItem ("Clear punch", [&transport]
        {
            transport.setPunchRange (0, 0);
            transport.setPunchEnabled (false);
        });
        m.addSeparator();
        m.addSectionHeader ("Markers");
        const int hoveredMarkerIdx = hitTestMarker (e.x, e.y);
        if (hoveredMarkerIdx >= 0)
        {
            const auto& mk = session.getMarkers()[(size_t) hoveredMarkerIdx];
            m.addItem ("Delete \"" + mk.name + "\"",
                        [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                         hoveredMarkerIdx]
                        {
                            if (safeThis == nullptr) return;
                            safeThis->session.removeMarker (hoveredMarkerIdx);
                            safeThis->repaint();
                        });
        }
        m.addItem ("Add marker here",
                    [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                     clickedSample]
                    {
                        if (safeThis == nullptr) return;
                        safeThis->session.addMarker (clickedSample);
                        safeThis->repaint();
                    });
        if (! session.getMarkers().empty())
        {
            const double sr = engine.getCurrentSampleRate();
            juce::PopupMenu jumpSub;
            for (int i = 0; i < (int) session.getMarkers().size(); ++i)
            {
                const auto& mk = session.getMarkers()[(size_t) i];
                const int secs = sr > 0.0
                                   ? (int) ((double) mk.timelineSamples / sr)
                                   : 0;
                jumpSub.addItem (mk.name + "  (" + juce::String (secs) + "s)",
                                  [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                                   i]
                                  {
                                      if (safeThis == nullptr) return;
                                      const auto& m = safeThis->session.getMarkers();
                                      if (i < 0 || i >= (int) m.size()) return;
                                      safeThis->engine.getTransport()
                                          .setPlayhead (m[(size_t) i].timelineSamples);
                                      safeThis->repaint();
                                  });
            }
            m.addSubMenu ("Jump to marker", jumpSub);
        }
        m.addSeparator();
        m.addItem ("Move playhead here", [&transport, clickedSample]
        {
            transport.setPlayhead (clickedSample);
        });
        // Anchor the menu at the cursor instead of the TapeStrip's
        // top-left corner. Same fix as the plugin picker.
        const auto cursor = e.getScreenPosition();
        m.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (juce::Rectangle<int> (cursor.x, cursor.y, 1, 1)),
                          [safeThis = juce::Component::SafePointer<TapeStrip> (this)] (int)
                          {
                              if (safeThis != nullptr) safeThis->repaint();
                          });
        return;
    }

    if (! col.contains (e.x, e.y)) return;

    // Left-click on a region body or edge starts a drag (move / trim) AND
    // marks that region as selected - keyboard copy/cut/delete then act on
    // it without needing a separate selection step.
    const auto hit = hitTestRegion (e.x, e.y);

    // Take-history badge: rotate to the next take. No drag, no selection
    // change beyond marking the rotated region as selected. Rotation is
    // FIFO - front of previousTakes surfaces, current goes to the back.
    // We keep this off the UndoManager for now: a rotation is reversible
    // by N more clicks, and undo of a single click would be visually
    // identical to clicking back manually.
    if (hit.op == RegionOp::TakeBadge)
    {
        auto& region = session.track (hit.track).regions[(size_t) hit.regionIdx];
        if (! region.previousTakes.empty())
        {
            TakeRef next = std::move (region.previousTakes.front());
            region.previousTakes.erase (region.previousTakes.begin());
            TakeRef current { region.file, region.sourceOffset, region.lengthInSamples };
            region.previousTakes.push_back (std::move (current));
            region.file            = next.file;
            region.sourceOffset    = next.sourceOffset;
            region.lengthInSamples = next.lengthInSamples;

            selectedTrack  = hit.track;
            selectedRegion = hit.regionIdx;
            rebuildPlaybackIfStopped();
            repaint();
        }
        return;
    }

    if (hit.op != RegionOp::None)
    {
        const auto& region = session.track (hit.track).regions[(size_t) hit.regionIdx];
        drag.track             = hit.track;
        drag.regionIdx         = hit.regionIdx;
        drag.op                = hit.op;
        drag.mouseDownSample   = sampleAtX (e.x);
        drag.origTimelineStart = region.timelineStart;
        drag.origLength        = region.lengthInSamples;
        drag.origSourceOffset  = region.sourceOffset;

        selectedTrack  = hit.track;
        selectedRegion = hit.regionIdx;
        repaint();
        return;
    }

    // Click on empty timeline → seek the playhead AND clear the selection.
    selectedTrack  = -1;
    selectedRegion = -1;

    const auto sample = sampleAtX (e.x);
    engine.getTransport().setPlayhead (sample);
    repaint();
}

void TapeStrip::mouseDrag (const juce::MouseEvent& e)
{
    // Marker drag - reposition a marker once the cursor moves more than
    // a small threshold from the click. Below threshold, it's still a
    // click (mouseUp will seek). The marker's array index stays valid
    // throughout because we re-sort only on mouseUp.
    if (markerDrag.active)
    {
        const auto cur = sampleAtX (e.x);
        constexpr juce::int64 kDragThresholdSamples = 256;   // ~5 ms @ 48k
        if (! markerDrag.moved
            && std::abs (cur - markerDrag.mouseDownSample) > kDragThresholdSamples)
        {
            markerDrag.moved = true;
        }
        if (markerDrag.moved
            && markerDrag.index >= 0
            && markerDrag.index < (int) session.getMarkers().size())
        {
            session.getMarkers()[(size_t) markerDrag.index].timelineSamples =
                juce::jmax ((juce::int64) 0, cur);
            repaint();
        }
        return;
    }

    // Bracket drag - reposition loop/punch endpoints or translate the
    // whole range. Endpoint drags clamp to keep start ≤ end - 1024
    // samples (the same useful-range floor we use elsewhere) so the
    // user can't accidentally collapse a bracket to zero length by
    // dragging one end through the other.
    if (bracketDrag.active)
    {
        constexpr juce::int64 kMinUsefulRangeSamples = 1024;
        const auto cur = juce::jmax ((juce::int64) 0, sampleAtX (e.x));
        const auto delta = cur - bracketDrag.mouseDownSample;
        auto& transport = engine.getTransport();
        switch (bracketDrag.type)
        {
            case BracketHit::LoopIn:
            {
                const auto newStart = juce::jlimit ((juce::int64) 0,
                                                       bracketDrag.origEnd - kMinUsefulRangeSamples,
                                                       cur);
                transport.setLoopRange (newStart, bracketDrag.origEnd);
                break;
            }
            case BracketHit::LoopOut:
            {
                const auto newEnd = juce::jmax (bracketDrag.origStart + kMinUsefulRangeSamples, cur);
                transport.setLoopRange (bracketDrag.origStart, newEnd);
                break;
            }
            case BracketHit::LoopBar:
            {
                const auto newStart = juce::jmax ((juce::int64) 0,
                                                    bracketDrag.origStart + delta);
                const auto length   = bracketDrag.origEnd - bracketDrag.origStart;
                transport.setLoopRange (newStart, newStart + length);
                break;
            }
            case BracketHit::PunchIn:
            {
                const auto newStart = juce::jlimit ((juce::int64) 0,
                                                       bracketDrag.origEnd - kMinUsefulRangeSamples,
                                                       cur);
                transport.setPunchRange (newStart, bracketDrag.origEnd);
                break;
            }
            case BracketHit::PunchOut:
            {
                const auto newEnd = juce::jmax (bracketDrag.origStart + kMinUsefulRangeSamples, cur);
                transport.setPunchRange (bracketDrag.origStart, newEnd);
                break;
            }
            case BracketHit::PunchBar:
            {
                const auto newStart = juce::jmax ((juce::int64) 0,
                                                    bracketDrag.origStart + delta);
                const auto length   = bracketDrag.origEnd - bracketDrag.origStart;
                transport.setPunchRange (newStart, newStart + length);
                break;
            }
            case BracketHit::None: break;
        }
        repaint();
        return;
    }

    // Ruler selection - sweep the highlight range as the user drags.
    // Range start is min(origin, current); end is max - works whether
    // the user drags left-to-right or right-to-left.
    if (rulerSelection.active)
    {
        rulerSelection.currentSample = juce::jmax ((juce::int64) 0, sampleAtX (e.x));
        repaint();
        return;
    }

    if (drag.op == RegionOp::None) return;

    auto& regions = session.track (drag.track).regions;
    if (drag.regionIdx < 0 || drag.regionIdx >= (int) regions.size()) return;

    const auto current = sampleAtX (e.x);
    juce::int64 deltaSamples = current - drag.mouseDownSample;

    // Snap-to-grid: round the *delta* to a whole-step value so the drag
    // motion still feels continuous but the destination lands on a tick.
    // Step is one beat when tempo > 0 (gives the user beat-aligned drags
    // even when their tempo is unusual), or one second otherwise.
    // The delta is rounded (not the absolute target) so a region whose
    // origin is mid-step stays mid-step on small drags - only large
    // drags re-align it to the grid.
    if (session.snapToGrid)
    {
        const double sr  = engine.getCurrentSampleRate();
        const float  bpm = session.tempoBpm.load();
        if (sr > 0.0)
        {
            const juce::int64 step = (bpm > 0.0f)
                ? (juce::int64) (sr * 60.0 / (double) bpm)
                : (juce::int64) sr;
            if (step > 0)
            {
                deltaSamples = ((deltaSamples + (deltaSamples >= 0 ? step / 2 : -step / 2))
                                / step) * step;
            }
        }
    }

    constexpr juce::int64 kMinLengthSamples = 1024;  // ~21 ms @ 48k
    auto& r = regions[(size_t) drag.regionIdx];

    switch (drag.op)
    {
        case RegionOp::Move:
        {
            r.timelineStart = juce::jmax ((juce::int64) 0,
                                drag.origTimelineStart + deltaSamples);
            break;
        }
        case RegionOp::TrimStart:
        {
            // Bound delta so newSourceOffset >= 0 and length stays above the
            // floor. Equivalent constraints in sample space:
            //   delta >= -origSourceOffset            (don't expose negative source)
            //   delta <=  origLength - kMinLength     (don't shrink below the floor)
            //   timelineStart + delta >= 0            (don't fall off the timeline)
            juce::int64 d = deltaSamples;
            d = juce::jmax (d, -drag.origSourceOffset);
            d = juce::jmax (d, -drag.origTimelineStart);
            d = juce::jmin (d, drag.origLength - kMinLengthSamples);
            r.timelineStart   = drag.origTimelineStart + d;
            r.lengthInSamples = drag.origLength        - d;
            r.sourceOffset    = drag.origSourceOffset  + d;
            break;
        }
        case RegionOp::TrimEnd:
        {
            const juce::int64 newLen = juce::jmax (kMinLengthSamples,
                                                    drag.origLength + deltaSamples);
            r.lengthInSamples = newLen;
            break;
        }
        case RegionOp::None:
        case RegionOp::TakeBadge:
            break;  // mouseDown handled the rotation; no drag semantics
    }
    repaint();
}

void TapeStrip::mouseUp (const juce::MouseEvent& e)
{
    // Bracket drag finalisation. Transport state is already up-to-date
    // from each mouseDrag call; nothing to do beyond clearing the drag
    // so future mouseDrags route through the right path.
    if (bracketDrag.active)
    {
        bracketDrag = {};
        return;
    }

    // Marker drag finalisation. A click without movement seeks; a real
    // drag commits the new marker position (and re-sorts the vector so
    // hit-testing stays consistent next time).
    if (markerDrag.active)
    {
        if (markerDrag.moved
            && markerDrag.index >= 0
            && markerDrag.index < (int) session.getMarkers().size())
        {
            // Re-sort by timelineSamples so the painter still iterates
            // left-to-right after the drag.
            auto& mks = session.getMarkers();
            std::stable_sort (mks.begin(), mks.end(),
                [] (const Marker& a, const Marker& b)
                { return a.timelineSamples < b.timelineSamples; });
        }
        else if (! markerDrag.moved
                 && markerDrag.index >= 0
                 && markerDrag.index < (int) session.getMarkers().size())
        {
            // Pure click on a flag → seek to the marker.
            const auto& m = session.getMarkers()[(size_t) markerDrag.index];
            engine.getTransport().setPlayhead (m.timelineSamples);
        }
        markerDrag = {};
        repaint();
        return;
    }

    // Ruler-selection finalisation. A drag shorter than ~24ms is
    // treated as a click and just dismisses the selection. Otherwise we
    // offer a context menu so the user picks whether the range is for
    // loop or punch - dragging itself doesn't auto-commit.
    //
    // Important: we DON'T clear rulerSelection here. Keeping it active
    // means the grey highlight stays painted while the menu is open;
    // each menu lambda clears it as part of the same setLoopRange/
    // setPunchRange call, so the very next paint shows the committed
    // green/red range instead of the grey - no blink between states.
    if (rulerSelection.active)
    {
        const auto a = juce::jmin (rulerSelection.originSample,
                                     rulerSelection.currentSample);
        const auto b = juce::jmax (rulerSelection.originSample,
                                     rulerSelection.currentSample);

        constexpr juce::int64 kMinUsefulRangeSamples = 1024;
        if (b - a <= kMinUsefulRangeSamples)
        {
            rulerSelection = {};
            repaint();
            return;
        }

        const auto cursor = e.getScreenPosition();
        juce::PopupMenu m;
        m.addItem ("Set loop here",
                    [safeThis = juce::Component::SafePointer<TapeStrip> (this), a, b]
                    {
                        if (safeThis == nullptr) return;
                        auto& transport = safeThis->engine.getTransport();
                        transport.setLoopRange (a, b);
                        transport.setLoopEnabled (true);
                        safeThis->rulerSelection = {};
                        safeThis->repaint();
                    });
        m.addItem ("Set punch in / out here",
                    [safeThis = juce::Component::SafePointer<TapeStrip> (this), a, b]
                    {
                        if (safeThis == nullptr) return;
                        auto& transport = safeThis->engine.getTransport();
                        transport.setPunchRange (a, b);
                        transport.setPunchEnabled (true);
                        safeThis->rulerSelection = {};
                        safeThis->repaint();
                    });
        m.addSeparator();
        m.addItem ("Cancel",
                    [safeThis = juce::Component::SafePointer<TapeStrip> (this)]
                    {
                        if (safeThis == nullptr) return;
                        safeThis->rulerSelection = {};
                        safeThis->repaint();
                    });
        // Menu-dismiss callback catches escape / click-outside (no item
        // chosen) so the grey highlight still goes away in those paths.
        m.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (
                                juce::Rectangle<int> (cursor.x, cursor.y, 1, 1)),
                          [safeThis = juce::Component::SafePointer<TapeStrip> (this)] (int)
                          {
                              if (safeThis == nullptr) return;
                              if (safeThis->rulerSelection.active)
                              {
                                  safeThis->rulerSelection = {};
                                  safeThis->repaint();
                              }
                          });
        return;
    }

    if (drag.op == RegionOp::None) return;

    auto& regions = session.track (drag.track).regions;
    if (drag.regionIdx >= 0 && drag.regionIdx < (int) regions.size())
    {
        // The drag mutated `regions[idx]` in-place for live feedback; capture
        // the final (after) state, rebuild the (before) state from the
        // captured originals, and route the swap through UndoManager so it
        // becomes one undoable transaction. perform() re-applies `after` to
        // the region, which is idempotent - the user sees no glitch.
        AudioRegion afterState  = regions[(size_t) drag.regionIdx];
        AudioRegion beforeState = afterState;
        beforeState.timelineStart   = drag.origTimelineStart;
        beforeState.lengthInSamples = drag.origLength;
        beforeState.sourceOffset    = drag.origSourceOffset;

        // Skip if nothing actually moved (a click without a drag).
        if (beforeState.timelineStart   != afterState.timelineStart
            || beforeState.lengthInSamples != afterState.lengthInSamples
            || beforeState.sourceOffset    != afterState.sourceOffset)
        {
            auto& um = engine.getUndoManager();
            um.beginNewTransaction (drag.op == RegionOp::Move ? "Move region"
                                                               : "Trim region");
            um.perform (new RegionEditAction (session, engine,
                                                drag.track, drag.regionIdx,
                                                beforeState, afterState));
        }
    }

    drag = {};
    rebuildPlaybackIfStopped();
    repaint();
}

void TapeStrip::mouseMove (const juce::MouseEvent& e)
{
    // Cursor feedback so the user can tell where edges/body are without
    // clicking blindly.
    if (hitTestMarker (e.x, e.y) >= 0)
    {
        // Markers are click-to-seek and drag-to-reposition - "dragging
        // hand" reads as both clickable and draggable.
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }

    if (const auto bh = hitTestBracket (e.x, e.y); bh != BracketHit::None)
    {
        // Endpoint pills get a horizontal-resize cursor; bar drags get
        // a "moving" hand. Same pattern as region trim vs region move.
        const bool isBar = (bh == BracketHit::LoopBar || bh == BracketHit::PunchBar);
        setMouseCursor (isBar ? juce::MouseCursor::DraggingHandCursor
                              : juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    const auto hit = hitTestRegion (e.x, e.y);
    switch (hit.op)
    {
        case RegionOp::TrimStart:
        case RegionOp::TrimEnd:
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            break;
        case RegionOp::Move:
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            break;
        case RegionOp::TakeBadge:
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            break;
        case RegionOp::None:
            setMouseCursor (juce::MouseCursor::NormalCursor);
            break;
    }
}

void TapeStrip::showRegionContextMenu (const RegionHit& hit, juce::Point<int> screenPos)
{
    auto& transport = engine.getTransport();
    const auto playhead = transport.getPlayhead();

    const auto& region = session.track (hit.track).regions[(size_t) hit.regionIdx];
    const auto regionEnd = region.timelineStart + region.lengthInSamples;
    const bool playheadInside =
        playhead > region.timelineStart && playhead < regionEnd;

    juce::PopupMenu m;
    m.addSectionHeader (juce::String::formatted ("Track %d region %d",
                                                  hit.track + 1, hit.regionIdx + 1));
    m.addItem ("Split at playhead", playheadInside,
                false /*ticked*/,
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 hitCopy = hit, playhead]
                {
                    if (safeThis == nullptr) return;
                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction ("Split region");
                    um.perform (new SplitRegionAction (safeThis->session, safeThis->engine,
                                                         hitCopy.track,
                                                         hitCopy.regionIdx,
                                                         playhead));
                    safeThis->repaint();
                });
    m.addSeparator();
    m.addItem ("Delete region",
                [safeThis = juce::Component::SafePointer<TapeStrip> (this),
                 hitCopy = hit]
                {
                    if (safeThis == nullptr) return;
                    auto& um = safeThis->engine.getUndoManager();
                    um.beginNewTransaction ("Delete region");
                    um.perform (new DeleteRegionAction (safeThis->session, safeThis->engine,
                                                          hitCopy.track,
                                                          hitCopy.regionIdx));
                    safeThis->repaint();
                });

    m.showMenuAsync (juce::PopupMenu::Options()
                        .withTargetScreenArea (
                            juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1)));
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
            // Tick marks span only the upper "tick band" so they don't
            // visually clash with marker flags + loop/punch pills below.
            g.drawVerticalLine (x, (float) ruler.getY() + 6.0f,
                                  (float) ruler.getY() + (float) kRulerTickBandH);
            const int mins = (int) (sec / 60.0);
            const int secs = (int) sec % 60;
            const auto timeLabel = juce::String::formatted ("%d:%02d", mins, secs);
            g.drawText (timeLabel, x + 3, ruler.getY(), 60, kRulerTickBandH - 1,
                         juce::Justification::centredLeft, false);
        }
    }

    // ── Track rows ──
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto row = rowBounds (t);

        // Row background - slightly darker every other row.
        g.setColour (t % 2 == 0 ? juce::Colour (0xff141418) : juce::Colour (0xff101014));
        g.fillRect (row);

        // Track label on the left (color stripe + name from session, falling
        // back to the 1-based track number if no name has been set).
        auto labelRow = juce::Rectangle<int> (label.getX(), row.getY(),
                                                label.getWidth(), row.getHeight());
        g.setColour (session.track (t).colour.withAlpha (0.85f));
        g.fillRect (labelRow.removeFromLeft (3));
        g.setColour (juce::Colour (0xffd0d0d0));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        const auto& trackName = session.track (t).name;
        const juce::String displayLabel = trackName.isNotEmpty() ? trackName
                                                                  : juce::String (t + 1);
        g.drawText (displayLabel, labelRow.withTrimmedLeft (4),
                     juce::Justification::centredLeft, false);

        // Recorded regions for this track.
        const auto& regions = session.track (t).regions;
        for (int ri = 0; ri < (int) regions.size(); ++ri)
        {
            const auto& region = regions[(size_t) ri];
            const int x0 = xForSample (region.timelineStart);
            const int x1 = xForSample (region.timelineStart + region.lengthInSamples);
            if (x1 <= col.getX() || x0 >= col.getRight()) continue;

            auto regionRect = juce::Rectangle<int> (x0, row.getY() + 1,
                                                     juce::jmax (2, x1 - x0),
                                                     row.getHeight() - 2)
                                  .getIntersection (col.withTrimmedTop (1).withTrimmedBottom (1));
            if (regionRect.isEmpty()) continue;

            const bool isSelected = (t == selectedTrack && ri == selectedRegion);

            // Block fill - track color, slightly darker so the row label still pops.
            // Selected regions get a brighter mix so they read as the focused thing.
            auto fillColour = session.track (t).colour.withMultipliedSaturation (0.85f)
                                                       .withMultipliedBrightness (0.65f);
            if (isSelected) fillColour = fillColour.brighter (0.30f);
            g.setColour (fillColour);
            g.fillRoundedRectangle (regionRect.toFloat(), 2.0f);

            // Outline - thicker + white-ish when selected.
            if (isSelected)
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawRoundedRectangle (regionRect.toFloat().reduced (0.5f), 2.0f, 1.6f);
            }
            else
            {
                g.setColour (session.track (t).colour.withAlpha (0.9f));
                g.drawRoundedRectangle (regionRect.toFloat().reduced (0.5f), 2.0f, 0.8f);
            }

            // Tiny waveform-stub stripe along the centre - not real audio, just
            // a visual cue that the block holds something.
            g.setColour (session.track (t).colour.brighter (0.4f).withAlpha (0.6f));
            const float midY = (float) regionRect.getCentreY();
            g.drawHorizontalLine ((int) midY, (float) regionRect.getX() + 2.0f,
                                   (float) regionRect.getRight() - 2.0f);

            // Take-history badge. Shows total take count (current + prior)
            // anchored to the region's top-left when there's at least one
            // prior take. Clicking it rotates to the next prior take; the
            // rotation is hit-tested via hitTestRegion's TakeBadge op.
            if (! region.previousTakes.empty())
            {
                const int badgeW = juce::jmin (regionRect.getWidth(), 16);
                const int badgeH = juce::jmin (regionRect.getHeight(), 10);
                if (badgeW >= 8 && badgeH >= 6)
                {
                    juce::Rectangle<int> badge (regionRect.getX(), regionRect.getY(),
                                                  badgeW, badgeH);
                    g.setColour (juce::Colours::black.withAlpha (0.6f));
                    g.fillRoundedRectangle (badge.toFloat(), 1.5f);
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions (8.0f, juce::Font::bold)));
                    g.drawText (juce::String ((int) region.previousTakes.size() + 1),
                                 badge, juce::Justification::centred, false);
                }
            }
        }
    }

    // ── Vertical separator between labels and tracks ──
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawVerticalLine (col.getX() - 1, 0.0f, (float) getHeight());

    // ── Loop / punch brackets ──
    // Translucent fill across the track area + a solid bar at the top so
    // the region is unambiguous even when the underlying tracks are dense.
    // Optional `pillLabel` draws a small filled label at both endpoints,
    // matching the in/out marker style of pro DAWs.
    auto drawRange = [&] (juce::int64 start, juce::int64 end,
                           juce::Colour colour, bool enabled,
                           const juce::String& pillLabel)
    {
        if (end <= start) return;
        const int x0Raw = xForSample (start);
        const int x1Raw = xForSample (end);
        const int x0 = juce::jmax (col.getX(),     x0Raw);
        const int x1 = juce::jmin (col.getRight(), x1Raw);
        if (x1 <= x0) return;

        // Translucent fill across the track area so the range reads as
        // "this stretch is the loop/punch zone" without competing with
        // recorded regions. Brighter when the toggle's on.
        g.setColour (colour.withAlpha (enabled ? 0.18f : 0.08f));
        g.fillRect (x0, kRulerH, x1 - x0, getHeight() - kRulerH);

        // Solid bracket bar across the bottom of the ruler. Full opacity
        // when enabled; half-opacity when the bounds are set but the
        // toggle is off, so the user can still see where the range will
        // jump to when they re-enable.
        constexpr int kBarH = 4;
        g.setColour (colour.withAlpha (enabled ? 1.0f : 0.55f));
        g.fillRect (x0, ruler.getBottom() - kBarH, x1 - x0, kBarH);

        // Endpoint pills - sit in the pill band of the ruler, above the
        // bracket bar, with rounded "tail" pointing down into the bar so
        // the pill+bar reads as a single bracket shape.
        if (pillLabel.isNotEmpty())
        {
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            const int textW = juce::jmax (40,
                g.getCurrentFont().getStringWidth (pillLabel) + 10);
            const int pillH = kRulerPillBandH - 2;     // small gap above the bar
            const int pillY = ruler.getY() + kRulerTickBandH;

            auto drawPill = [&] (int xCentre)
            {
                int pillX = xCentre - textW / 2;
                pillX = juce::jlimit (col.getX(), col.getRight() - textW, pillX);
                juce::Rectangle<int> r (pillX, pillY, textW, pillH);
                g.setColour (colour.withAlpha (enabled ? 1.0f : 0.7f));
                g.fillRoundedRectangle (r.toFloat(), 3.0f);
                g.setColour (juce::Colours::white);
                g.drawText (pillLabel, r, juce::Justification::centred, false);
            };

            if (x0Raw >= col.getX() && x0Raw <= col.getRight()) drawPill (x0Raw);
            if (x1Raw >= col.getX() && x1Raw <= col.getRight()
                && std::abs (x1Raw - x0Raw) > textW + 8)
            {
                drawPill (x1Raw);
            }
        }
    };

    // ── In-flight ruler selection ──
    // Painted under loop/punch so a freshly-finished drag's highlight
    // doesn't overpaint the result the user just chose. Neutral grey so
    // it doesn't misleadingly look like a committed loop or punch range.
    if (rulerSelection.active)
    {
        const auto sa = juce::jmin (rulerSelection.originSample,
                                      rulerSelection.currentSample);
        const auto sb = juce::jmax (rulerSelection.originSample,
                                      rulerSelection.currentSample);
        const int x0 = juce::jmax (col.getX(),     xForSample (sa));
        const int x1 = juce::jmin (col.getRight(), xForSample (sb));
        if (x1 > x0)
        {
            g.setColour (juce::Colour (0xffd0d0d8).withAlpha (0.18f));
            g.fillRect (x0, kRulerH, x1 - x0, getHeight() - kRulerH);
            g.setColour (juce::Colour (0xffd0d0d8).withAlpha (0.85f));
            g.fillRect (x0, ruler.getBottom() - 4, x1 - x0, 4);
        }
    }

    auto& transport = engine.getTransport();
    // Loop in green (visually distinct from punch's red), punch in red -
    // matches the colour language the user expects from pro DAWs.
    drawRange (transport.getLoopStart(),  transport.getLoopEnd(),
                juce::Colour (0xff3aa860), transport.isLoopEnabled(), "Loop");
    drawRange (transport.getPunchIn(),    transport.getPunchOut(),
                juce::Colour (0xffd05a5a), transport.isPunchEnabled(), "Punch");

    // ── Markers ──
    // Flag in the ruler's pill band + vertical guideline through tracks.
    // Drawn after loop/punch so a marker that lands exactly on a loop/punch
    // boundary is still readable on top.
    {
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        for (const auto& marker : session.getMarkers())
        {
            const int x = xForSample (marker.timelineSamples);
            if (x < col.getX() - 80 || x > col.getRight()) continue;

            // Guideline through the track area.
            g.setColour (marker.colour.withAlpha (0.35f));
            g.drawVerticalLine (x, (float) col.getY(), (float) getHeight());

            // Flag positioned in the pill band of the ruler so it sits at
            // the same y as loop/punch pills - one consistent "ruler
            // overlay" zone.
            const int textW  = g.getCurrentFont().getStringWidth (marker.name) + 10;
            const int flagW  = juce::jlimit (32, 160, textW);
            const int flagH  = kRulerPillBandH - 2;
            const int flagX  = juce::jmin (x, getWidth() - flagW - 2);
            const int flagY  = ruler.getY() + kRulerTickBandH;
            juce::Rectangle<int> flag (flagX, flagY, flagW, flagH);

            g.setColour (marker.colour);
            g.fillRoundedRectangle (flag.toFloat(), 2.0f);
            g.setColour (juce::Colour (0xff181820));
            g.drawText (marker.name, flag.reduced (4, 0),
                         juce::Justification::centredLeft, true);
        }
    }

    // ── Playhead line ──
    const auto playhead = engine.getTransport().getPlayhead();
    const int phX = xForSample (playhead);
    if (phX >= col.getX() && phX <= col.getRight())
    {
        g.setColour (juce::Colour (0xffe04040));
        g.drawVerticalLine (phX, 0.0f, (float) getHeight());
    }
}

TapeStrip::BracketHit TapeStrip::hitTestBracket (int x, int y) const noexcept
{
    auto ruler = rulerBounds();
    if (! ruler.contains (x, y)) return BracketHit::None;

    // Pill band sits below the tick band; bar sits in the bottom 4px of
    // the ruler. Outside both → no hit.
    const int pillTop  = ruler.getY() + kRulerTickBandH;
    const int barTop   = ruler.getBottom() - 4;
    const bool inPills = (y >= pillTop && y < barTop);
    const bool inBar   = (y >= barTop  && y < ruler.getBottom());
    if (! inPills && ! inBar) return BracketHit::None;

    auto& transport = engine.getTransport();

    auto pillBounds = [this] (juce::int64 sample, const juce::String& label)
    {
        // Same width math the painter uses; exposes a forgiving 6px hit
        // gutter on each side of the pill so users don't have to land
        // pixel-perfect on the label.
        auto col = tracksColumnBounds();
        const int xMid = xForSample (sample);
        const int textW = juce::jmax (40, label.length() * 7 + 14);
        int pillX = juce::jlimit (col.getX(), col.getRight() - textW,
                                    xMid - textW / 2);
        return juce::Rectangle<int> (pillX - 6, 0, textW + 12, 0);
    };

    auto inXRange = [] (int xx, juce::Rectangle<int> r)
    { return xx >= r.getX() && xx <= r.getRight(); };

    // Loop pills + bar (checked first so "loop" wins on overlap with
    // punch when both happen to share an endpoint).
    if (transport.getLoopEnd() > transport.getLoopStart())
    {
        if (inPills)
        {
            if (inXRange (x, pillBounds (transport.getLoopStart(), "Loop")))
                return BracketHit::LoopIn;
            if (inXRange (x, pillBounds (transport.getLoopEnd(),   "Loop")))
                return BracketHit::LoopOut;
        }
        if (inBar)
        {
            const int x0 = xForSample (transport.getLoopStart());
            const int x1 = xForSample (transport.getLoopEnd());
            if (x >= x0 && x <= x1) return BracketHit::LoopBar;
        }
    }

    if (transport.getPunchOut() > transport.getPunchIn())
    {
        if (inPills)
        {
            if (inXRange (x, pillBounds (transport.getPunchIn(),  "Punch")))
                return BracketHit::PunchIn;
            if (inXRange (x, pillBounds (transport.getPunchOut(), "Punch")))
                return BracketHit::PunchOut;
        }
        if (inBar)
        {
            const int x0 = xForSample (transport.getPunchIn());
            const int x1 = xForSample (transport.getPunchOut());
            if (x >= x0 && x <= x1) return BracketHit::PunchBar;
        }
    }
    return BracketHit::None;
}

int TapeStrip::hitTestMarker (int x, int y) const noexcept
{
    auto ruler = rulerBounds();
    if (! ruler.contains (x, y)) return -1;
    // Flags sit in the pill band of the ruler (below the tick band).
    if (y < ruler.getY() + kRulerTickBandH) return -1;

    const auto& markers = session.getMarkers();
    // Iterate back-to-front so a later (rightmost) marker wins on overlap,
    // matching the painter's left-to-right draw order.
    for (int i = (int) markers.size() - 1; i >= 0; --i)
    {
        const int mx = xForSample (markers[(size_t) i].timelineSamples);
        // Same width math the painter uses, just without a Graphics
        // context. Slightly looser bound (8 px/char) so hit-testing is
        // forgiving on the right edge of the flag.
        const int approxFlagW = juce::jlimit (28, 160,
            (int) markers[(size_t) i].name.length() * 8 + 12);
        const int flagX = juce::jmin (mx, getWidth() - approxFlagW - 2);
        if (x >= flagX && x <= flagX + approxFlagW) return i;
    }
    return -1;
}

bool TapeStrip::copySelectedRegion()
{
    if (selectedTrack < 0 || selectedRegion < 0) return false;
    const auto& regs = session.track (selectedTrack).regions;
    if (selectedRegion >= (int) regs.size()) return false;

    auto& clip = engine.getRegionClipboard();
    clip.region      = regs[(size_t) selectedRegion];
    clip.sourceTrack = selectedTrack;
    clip.hasContent  = true;
    return true;
}

bool TapeStrip::cutSelectedRegion()
{
    // Copy first (non-undoable side effect on the clipboard), then push a
    // DeleteRegionAction so Cmd+Z brings the region back. The clipboard
    // keeps the cut content even after undo - same as Logic / Pro Tools.
    if (! copySelectedRegion()) return false;
    return deleteSelectedRegion();
}

bool TapeStrip::pasteAtPlayhead()
{
    auto& clip = engine.getRegionClipboard();
    if (! clip.hasContent) return false;

    const int targetTrack = (clip.sourceTrack >= 0 && clip.sourceTrack < Session::kNumTracks)
                              ? clip.sourceTrack : 0;

    AudioRegion pasted = clip.region;
    pasted.timelineStart = engine.getTransport().getPlayhead();

    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Paste region");
    um.perform (new PasteRegionAction (session, engine, targetTrack, pasted));

    // Select the freshly-pasted region (now at the back of the target's
    // region list) so a follow-up Delete or another paste targets it.
    selectedTrack  = targetTrack;
    selectedRegion = (int) session.track (targetTrack).regions.size() - 1;
    repaint();
    return true;
}

bool TapeStrip::deleteSelectedRegion()
{
    if (selectedTrack < 0 || selectedRegion < 0) return false;
    const auto& regs = session.track (selectedTrack).regions;
    if (selectedRegion >= (int) regs.size()) return false;

    auto& um = engine.getUndoManager();
    um.beginNewTransaction ("Delete region");
    um.perform (new DeleteRegionAction (session, engine,
                                          selectedTrack, selectedRegion));

    selectedTrack  = -1;
    selectedRegion = -1;
    repaint();
    return true;
}
} // namespace focal
