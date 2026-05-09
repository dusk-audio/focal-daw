#include "PlatformWindowing.h"

// Stubs for now. The Mac/Win equivalents of the Linux fixes haven't
// surfaced yet because the user smoke-tests on Linux only - but the
// fix slots need to exist so when (not if) the bugs appear, the
// scope of the change is one platform-impl file rather than a
// cross-cutting refactor. See PlatformWindowing.h for the eventual
// per-platform implementations:
//   bringWindowToFront     -> [NSApp activateIgnoringOtherApps:YES]
//                              + makeKeyAndOrderFront on the peer's
//                              NSWindow (peer.getNativeHandle() returns
//                              the NSView; .window pulls the window).
//   flushWindowOperations  -> drain a single NSRunLoop iteration with
//                              dateLimit = +1 ms.
//   prepareNativePeer...   -> no-op (NSView reparenting is
//                              synchronous, no race window).

namespace focal::platform
{
void bringWindowToFront (juce::ComponentPeer&)             {}
void flushWindowOperations()                                {}
void prepareNativePeerForChildAttach (juce::ComponentPeer&) {}
} // namespace focal::platform
