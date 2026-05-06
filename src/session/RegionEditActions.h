#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "Session.h"

namespace adhdaw
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
} // namespace adhdaw
