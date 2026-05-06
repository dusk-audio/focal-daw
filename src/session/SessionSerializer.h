#pragma once

#include <juce_core/juce_core.h>
#include "Session.h"

namespace adhdaw
{
// Serialise / restore a Session to/from JSON on disk. Save uses an atomic
// write pattern (write to .tmp, move into place) so a crash mid-save never
// produces a partial session.json. Load is best-effort - missing fields fall
// back to the Session's defaults so older session files still open.
class SessionSerializer
{
public:
    static bool save (const Session& session, const juce::File& target);
    static bool load (Session& session,       const juce::File& source);
};
} // namespace adhdaw
