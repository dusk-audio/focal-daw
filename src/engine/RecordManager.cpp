#include "RecordManager.h"
#include <unordered_map>

namespace focal
{
RecordManager::RecordManager (Session& s) : session (s)
{
    diskThread.startThread();
}

RecordManager::~RecordManager()
{
    if (active.load (std::memory_order_relaxed))
        stopRecording (0);
    diskThread.stopThread (2000);
}

bool RecordManager::startRecording (double sampleRate, juce::int64 startSample)
{
    if (active.load (std::memory_order_relaxed))
        return true;
    if (! session.anyTrackArmed())
        return false;

    auto audioDir = session.getAudioDirectory();
    if (! audioDir.exists())
        audioDir.createDirectory();

    recordStartSample = startSample;
    recordSampleRate  = sampleRate;

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        if (! session.track (t).recordArmed.load (std::memory_order_relaxed))
            continue;

        // MIDI tracks: spin up the MIDI capture FIFO and skip the WAV
        // writer entirely. The audio thread will push events into the
        // FIFO via writeMidiBlock; stopRecording drains it into a
        // MidiRegion and pushes onto track.midiRegions.
        if (session.track (t).mode.load (std::memory_order_relaxed)
            == (int) Track::Mode::Midi)
        {
            auto cap = std::make_unique<PerTrackMidi>();
            cap->fifo.reset();
            midiCaptures[(size_t) t] = std::move (cap);
            continue;
        }

        auto trackName = juce::String::formatted ("track%02d_%s.wav", t + 1,
                                                   stamp.toRawUTF8());
        juce::File outFile = audioDir.getChildFile (trackName);
        outFile.deleteFile();

        auto* fileStream = outFile.createOutputStream().release();
        if (fileStream == nullptr)
            continue;

        // 24-bit WAV per the spec. Channel count follows the track's mode:
        // 1 for Mono / Midi (MIDI tracks don't audio-record yet), 2 for
        // Stereo. The writer's channel count is captured here so writeInput-
        // Block builds a matching channel-pointer array on the audio thread.
        const int trackChannels =
            session.track (t).mode.load (std::memory_order_relaxed)
                == (int) Track::Mode::Stereo ? 2 : 1;

        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (fileStream, sampleRate, (unsigned int) trackChannels,
                                  24, {}, 0));
        if (writer == nullptr)
        {
            delete fileStream;
            continue;
        }

        auto perTrack = std::make_unique<PerTrackWriter>();
        perTrack->file = outFile;
        perTrack->numChannels = trackChannels;
        perTrack->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
            writer.release(), diskThread, 32768);
        writers[(size_t) t] = std::move (perTrack);
    }

    active.store (true, std::memory_order_release);
    return true;
}

void RecordManager::stopRecording (juce::int64 endSample)
{
    if (! active.load (std::memory_order_relaxed))
        return;

    active.store (false, std::memory_order_release);

    // Drain any per-track MIDI captures into MidiRegions BEFORE the writer
    // teardown loop below - audio + MIDI commit phases are independent so
    // ordering doesn't matter, but doing MIDI first keeps the two paths
    // visibly separate and the failure cases isolated.
    const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
    const juce::int64 totalSamples = juce::jmax<juce::int64> (1, endSample - recordStartSample);
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& cap = midiCaptures[(size_t) t];
        if (cap == nullptr) continue;

        // Drain the lock-free FIFO into a flat vector so we can sort by
        // sample-position before pairing Note On/Off events. Per JUCE's
        // contract events arrive in sample order within a single block,
        // but sample positions across blocks are monotonic so the FIFO
        // order is already correct - we still copy into a vector to allow
        // the linear note-pairing pass below.
        const int avail = cap->fifo.getNumReady();
        std::vector<PerTrackMidi::RawEvent> drained;
        drained.reserve ((size_t) avail);
        int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
        cap->fifo.prepareToRead (avail, s1, sz1, s2, sz2);
        for (int i = 0; i < sz1; ++i) drained.push_back (cap->events[(size_t) (s1 + i)]);
        for (int i = 0; i < sz2; ++i) drained.push_back (cap->events[(size_t) (s2 + i)]);
        cap->fifo.finishedRead (sz1 + sz2);

        if (drained.empty())
        {
            cap.reset();
            continue;
        }

        MidiRegion region;
        region.timelineStart   = recordStartSample;
        region.lengthInSamples = totalSamples;
        region.lengthInTicks   = samplesToTicks (totalSamples, recordSampleRate, bpm);

        // Pair Note On / Note Off into MidiNote entries. Pending map keyed
        // on (channel, noteNumber) so concurrent notes on different keys
        // don't collide. Vel-0 Note On counts as Note Off (running-status
        // controllers use this convention to save bandwidth).
        struct PendingNote { juce::int64 startSample; int velocity; };
        std::unordered_map<int, PendingNote> pending;
        auto noteKey = [] (int ch, int note) { return ch * 256 + note; };

        for (const auto& ev : drained)
        {
            // Drop events captured before the take's logical start (count-in
            // pre-roll fires the audio callback but the take begins at
            // recordStartSample = activeRecordStart).
            if (ev.samplePos < 0) continue;
            if (ev.samplePos >= totalSamples) continue;

            const int channel = (ev.status & 0x0F) + 1;     // 1..16
            const int statusType = ev.status & 0xF0;

            if (statusType == 0x90 && ev.data2 > 0)         // Note On
            {
                pending[noteKey (channel, ev.data1)] = { ev.samplePos, ev.data2 };
            }
            else if (statusType == 0x80                      // Note Off
                     || (statusType == 0x90 && ev.data2 == 0))
            {
                const auto k = noteKey (channel, ev.data1);
                auto it = pending.find (k);
                if (it == pending.end()) continue;
                MidiNote n;
                n.channel    = channel;
                n.noteNumber = ev.data1;
                n.velocity   = it->second.velocity;
                n.startTick  = samplesToTicks (it->second.startSample, recordSampleRate, bpm);
                const auto offTick = samplesToTicks (ev.samplePos, recordSampleRate, bpm);
                n.lengthInTicks = juce::jmax<juce::int64> (1, offTick - n.startTick);
                region.notes.push_back (n);
                pending.erase (it);
            }
            else if (statusType == 0xB0)                     // CC
            {
                MidiCc c;
                c.channel    = channel;
                c.controller = ev.data1;
                c.value      = ev.data2;
                c.atTick     = samplesToTicks (ev.samplePos, recordSampleRate, bpm);
                region.ccs.push_back (c);
            }
            // Other channel-voice messages (pitch bend, aftertouch,
            // program) are dropped for now - the model holds notes + CCs
            // only. Phase 4c can extend MidiCc with a status discriminant
            // or add dedicated event vectors when the piano roll surfaces
            // them.
        }

        // Hanging notes - any Note On still in `pending` had no matching
        // Note Off in the captured stream. Truncate them to the end of
        // the region so the saved data has no dangling state. Real DAWs
        // also do this on punch-out / stop.
        for (const auto& [key, pn] : pending)
        {
            MidiNote n;
            n.channel    = (key / 256);
            n.noteNumber = (key % 256);
            n.velocity   = pn.velocity;
            n.startTick  = samplesToTicks (pn.startSample, recordSampleRate, bpm);
            n.lengthInTicks = juce::jmax<juce::int64> (1,
                region.lengthInTicks - n.startTick);
            region.notes.push_back (n);
        }

        if (region.notes.empty() && region.ccs.empty())
        {
            cap.reset();
            continue;
        }

        // Take-history capture, mirrors AudioRegion's fully-contained
        // overdub absorption below. Any existing MIDI region whose
        // timeline range sits fully inside the new take's range gets
        // moved into the new region's previousTakes (with its own
        // deeper history forwarded so an overdub-of-an-overdub doesn't
        // lose grandparent takes). Partial overlaps are intentionally
        // NOT absorbed - the user can still see / cycle to the older
        // takes via the badge UI; partial-overlap merging would need
        // a tick-domain split routine that's out of scope here.
        const juce::int64 newStart = region.timelineStart;
        const juce::int64 newEnd   = newStart + region.lengthInSamples;
        auto& mregs = session.track (t).midiRegions;
        for (auto it = mregs.begin(); it != mregs.end(); )
        {
            const auto exStart = it->timelineStart;
            const auto exEnd   = it->timelineStart + it->lengthInSamples;
            const bool fullyContained = exStart >= newStart && exEnd <= newEnd;
            if (! fullyContained) { ++it; continue; }

            MidiTakeRef ref;
            ref.lengthInTicks = it->lengthInTicks;
            ref.notes = std::move (it->notes);
            ref.ccs   = std::move (it->ccs);
            region.previousTakes.push_back (std::move (ref));

            for (auto& deeper : it->previousTakes)
                region.previousTakes.push_back (std::move (deeper));

            it = mregs.erase (it);
        }

        mregs.push_back (std::move (region));

        cap.reset();
    }

    // Tear down writers (this flushes the threaded queues and closes the
    // WAV files), then commit a Region for each.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& slot = writers[(size_t) t];
        if (slot == nullptr) continue;

        const auto frames = slot->framesWritten;
        slot->writer.reset();  // closes the file

        if (frames > 0)
        {
            AudioRegion region;
            region.file = slot->file;
            region.timelineStart = recordStartSample;
            region.lengthInSamples = frames;
            region.sourceOffset = 0;
            region.numChannels = slot->numChannels;

            // Take-history capture: any existing region whose timeline range
            // is FULLY CONTAINED within the new take's range gets absorbed
            // into previousTakes. The user can then cycle through them via
            // the badge UI without losing access to earlier takes.
            //
            // Partial overlaps (e.g. punch-in over the middle of a longer
            // take) are intentionally NOT absorbed - the longer region stays
            // visible on either side of the new take, and the painter just
            // draws the new region on top inside the punch range. Phase 3
            // proper will handle splitting a partially-overlapping region
            // into outer fragments + a new take cycle slot.
            const juce::int64 newStart = region.timelineStart;
            const juce::int64 newEnd   = newStart + region.lengthInSamples;
            auto& regs = session.track (t).regions;

            // Crossfade length: 10 ms per side. Short enough that the user
            // doesn't perceive it as a fade, long enough to mask boundary
            // discontinuities between takes. Bound by half the new take's
            // length so a punch shorter than 20 ms still gets symmetric
            // ramps without the in/out fades overlapping each other.
            const juce::int64 fadeSamplesNominal = (juce::int64) (recordSampleRate * 0.010);
            const juce::int64 fadeSamples = juce::jmax<juce::int64> (
                0, juce::jmin (fadeSamplesNominal, region.lengthInSamples / 2));

            // Pass 1 — fully-contained takes get absorbed into the new
            // region's previousTakes (no audio overlap, just history).
            // Partial overlaps fall through to Pass 2 below.
            std::vector<AudioRegion> spawnedFragments;
            for (auto it = regs.begin(); it != regs.end(); )
            {
                const auto exStart = it->timelineStart;
                const auto exEnd   = it->timelineStart + it->lengthInSamples;
                const bool fullyContained = exStart >= newStart && exEnd <= newEnd;
                if (! fullyContained) { ++it; continue; }

                TakeRef ref;
                ref.file            = it->file;
                ref.sourceOffset    = it->sourceOffset;
                ref.lengthInSamples = it->lengthInSamples;
                region.previousTakes.push_back (std::move (ref));

                // Carry forward the displaced region's own history so we
                // don't drop deeper takes when overdubbing repeatedly. The
                // newly-displaced take goes first, then the older ones.
                for (auto& deeper : it->previousTakes)
                    region.previousTakes.push_back (std::move (deeper));

                it = regs.erase (it);
            }

            // Pass 2 — partial overlaps get split / trimmed so the new
            // take's edges crossfade against the existing region's audio
            // instead of clicking. Three cases:
            //   • Left overlap  (exStart < newStart, exEnd inside punch):
            //     trim ex to [exStart, newStart + fade], fadeOut at end.
            //   • Right overlap (exStart inside punch, exEnd > newEnd):
            //     trim ex to [newEnd - fade, exEnd] + advance sourceOffset.
            //   • Span (ex wraps both ends): produce two fragments — left
            //     half + right half — sharing the original source file.
            // Fades are matched on the new region by hasOverlapL / R below.
            bool hasOverlapL = false, hasOverlapR = false;
            for (auto it = regs.begin(); it != regs.end(); )
            {
                const auto exStart = it->timelineStart;
                const auto exEnd   = it->timelineStart + it->lengthInSamples;
                const bool overlaps = ! (exEnd <= newStart || exStart >= newEnd);
                if (! overlaps) { ++it; continue; }

                const bool spansLeft  = exStart < newStart;
                const bool spansRight = exEnd   > newEnd;

                if (spansLeft && spansRight)
                {
                    // Span: produce a left fragment + a right fragment from
                    // the same source. Mutate `it` into the left fragment
                    // and queue the right fragment for re-insertion.
                    AudioRegion right = *it;
                    right.timelineStart   = newEnd - fadeSamples;
                    right.sourceOffset    = it->sourceOffset
                                           + (right.timelineStart - it->timelineStart);
                    right.lengthInSamples = exEnd - right.timelineStart;
                    right.fadeInSamples   = fadeSamples;
                    // Right fragment ends at the original exEnd, so any fade-out
                    // the source region carried still applies. Clamp so the new
                    // shorter length still satisfies fadeIn + fadeOut <= length.
                    right.fadeOutSamples  = juce::jmax<juce::int64> (0,
                        juce::jmin (right.fadeOutSamples,
                                     right.lengthInSamples - right.fadeInSamples));
                    right.previousTakes.clear();  // history stays with the left half
                    spawnedFragments.push_back (std::move (right));

                    it->lengthInSamples = (newStart + fadeSamples) - exStart;
                    it->fadeOutSamples  = fadeSamples;
                    hasOverlapL = hasOverlapR = true;
                    ++it;
                }
                else if (spansLeft)
                {
                    // Left overlap only: trim end to newStart + fade.
                    it->lengthInSamples = (newStart + fadeSamples) - exStart;
                    it->fadeOutSamples  = fadeSamples;
                    hasOverlapL = true;
                    ++it;
                }
                else if (spansRight)
                {
                    // Right overlap only: shift start to newEnd - fade.
                    const juce::int64 newLeft = newEnd - fadeSamples;
                    it->sourceOffset    += (newLeft - exStart);
                    it->timelineStart    = newLeft;
                    it->lengthInSamples  = exEnd - newLeft;
                    it->fadeInSamples    = fadeSamples;
                    hasOverlapR = true;
                    ++it;
                }
                else
                {
                    // Should be unreachable — fully-contained was handled
                    // in Pass 1. Defensive ++ to avoid an infinite loop.
                    ++it;
                }
            }
            for (auto& frag : spawnedFragments)
                regs.push_back (std::move (frag));

            if (hasOverlapL) region.fadeInSamples  = fadeSamples;
            if (hasOverlapR) region.fadeOutSamples = fadeSamples;

            regs.push_back (std::move (region));
        }
        else
        {
            slot->file.deleteFile();
        }
        slot.reset();
    }
}

void RecordManager::writeMidiBlock (int trackIndex,
                                     const juce::MidiBuffer& events,
                                     juce::int64 blockStartFromRecord) noexcept
{
    if (! active.load (std::memory_order_acquire)) return;
    if (events.isEmpty()) return;
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks) return;
    auto& cap = midiCaptures[(size_t) trackIndex];
    if (cap == nullptr) return;

    for (const auto meta : events)
    {
        const auto m = meta.getMessage();
        const auto* raw = m.getRawData();
        const int   sz  = m.getRawDataSize();
        if (raw == nullptr || sz < 1) continue;

        // Channel-voice messages we care about for 4b: Note On / Note Off
        // / CC / pitch bend / channel pressure / poly pressure / program.
        // System messages (sysex, clock, transport) are intentionally
        // dropped - they're not part of the per-track musical content.
        const auto status = (juce::uint8) raw[0];
        if (status < 0x80 || status >= 0xF0) continue;

        // Drop events whose absolute take-relative position is negative
        // (count-in pre-roll). They'd be filtered at stopRecording anyway;
        // gating here saves FIFO space and keeps stored samplePos non-negative.
        const auto samplePos = blockStartFromRecord + meta.samplePosition;
        if (samplePos < 0) continue;

        int needed = 1;
        if (cap->fifo.getFreeSpace() < needed) continue;  // FIFO full → drop this event, try next
        int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
        cap->fifo.prepareToWrite (needed, s1, sz1, s2, sz2);
        if (sz1 + sz2 < needed) { cap->fifo.finishedWrite (sz1 + sz2); continue; }
        auto& slot = cap->events[(size_t) s1];
        slot.samplePos = samplePos;
        slot.status = status;
        slot.data1  = sz >= 2 ? (juce::uint8) raw[1] : 0;
        slot.data2  = sz >= 3 ? (juce::uint8) raw[2] : 0;
        cap->fifo.finishedWrite (needed);
    }
}

void RecordManager::writeInputBlock (int trackIndex,
                                     const float* L,
                                     const float* R,
                                     int numSamples) noexcept
{
    if (! active.load (std::memory_order_acquire)) return;
    if (numSamples == 0) return;
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks) return;
    auto& slot = writers[(size_t) trackIndex];
    if (slot == nullptr || slot->writer == nullptr || L == nullptr) return;

    // Build the channel-pointer array to match the writer's channel count.
    // ThreadedWriter::write reads exactly numChannels pointers from the
    // array, so each slot it touches must be non-null.
    //   • Mono writer (numChannels == 1): only L is read; R is ignored even
    //     if the caller supplied it (mono-armed track + stereo input is a
    //     caller bug, asserted below).
    //   • Stereo writer (numChannels == 2): if R is null we duplicate L so
    //     the second channel is never a missing pointer.
    jassert (L != nullptr);
    const float* channels[2] = { L, (R != nullptr) ? R : L };
    jassert (channels[0] != nullptr
             && (slot->numChannels < 2 || channels[1] != nullptr));
    if (slot->writer->write (channels, numSamples))
        slot->framesWritten += numSamples;
}
} // namespace focal
