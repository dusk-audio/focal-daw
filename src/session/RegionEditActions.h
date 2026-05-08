#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "Session.h"

namespace focal
{
class AudioEngine;

// Replaces a single AudioRegion's fields with new values. Used for the move
// and trim drags, all of which collapse to "region X is now Y". perform()
// applies the new state; undo() restores the original.
class RegionEditAction final : public juce::UndoableAction
{
public:
    RegionEditAction (Session& session, AudioEngine& engine,
                       int trackIdx, int regionIdx,
                       const AudioRegion& before, const AudioRegion& after);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    AudioRegion beforeState;
    AudioRegion afterState;
};

// Splits a region at a timeline-sample position into two regions. The
// original shrinks to [start, splitAt); a new region is inserted at idx+1
// covering [splitAt, originalEnd) with sourceOffset adjusted so the audio
// is continuous across the split.
class SplitRegionAction final : public juce::UndoableAction
{
public:
    SplitRegionAction (Session& session, AudioEngine& engine,
                        int trackIdx, int regionIdx,
                        juce::int64 splitAtTimelineSample);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 2; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    juce::int64 splitAt;
    AudioRegion originalState;  // captured at first perform; restored on undo
};

// Inserts a copy of a region at the end of a track's region list. perform()
// records the index it was inserted at so undo() can erase the same slot.
class PasteRegionAction final : public juce::UndoableAction
{
public:
    PasteRegionAction (Session& session, AudioEngine& engine,
                        int trackIdx, const AudioRegion& region);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    AudioRegion regionToInsert;
    int insertedAt = -1;
};

// Removes a region; undo re-inserts it at its original index.
class DeleteRegionAction final : public juce::UndoableAction
{
public:
    DeleteRegionAction (Session& session, AudioEngine& engine,
                         int trackIdx, int regionIdx);

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 1; }

private:
    Session& session;
    AudioEngine& engine;
    int trackIdx;
    int regionIdx;
    AudioRegion removed;
    bool        haveRemoved = false;
};

// Clones a source track's full per-strip state onto a destination slot:
// name, colour, mode, channel-strip params (fader/pan/EQ/comp/sends),
// recording surface settings, regions, MIDI regions, plugin instance
// (description + state). Undo restores the destination's previous state
// captured at first perform().
//
// Plugin replay: copying pluginDescriptionXml / pluginStateBase64 onto
// the destination Track isn't enough on its own - the live PluginSlot
// owned by AudioEngine has to re-instantiate. perform() reads source's
// live slot via getDescriptionXmlForSave / getStateBase64ForSave and
// asks the destination slot to restoreFromSavedState. Undo replays the
// captured before-state through the same path so the dest slot returns
// to whatever plugin (if any) was loaded before.
class CloneTrackAction final : public juce::UndoableAction
{
public:
    CloneTrackAction (Session& session, AudioEngine& engine,
                       int sourceTrackIdx, int destTrackIdx);
    // Out-of-line so the unique_ptr<Impl> deleters are instantiated in the
    // .cpp where Impl is complete, not at every user-site that includes
    // this header with Impl forward-declared.
    ~CloneTrackAction() override;

    bool perform() override;
    bool undo()    override;
    int  getSizeInUnits() override { return 4; }   // medium weight

    // Public so the .cpp's anonymous helpers can take it as a parameter.
    // The struct itself is opaque to callers - they only see unique_ptr<Impl>.
    struct Impl;

private:
    Session& session;
    AudioEngine& engine;
    int srcIdx;
    int dstIdx;

    // Snapshots captured at first perform() so undo / redo are
    // self-contained.
    std::unique_ptr<Impl> beforeState;
    std::unique_ptr<Impl> afterState;
};
} // namespace focal
