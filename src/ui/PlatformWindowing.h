#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace focal::platform
{
// Cross-platform window-management primitives. Per-platform
// implementations live in PlatformWindowing_{Linux,Mac,Windows}.{cpp,mm}.
// Callsites stay platform-agnostic; only this header is included.
//
// Why a dedicated module rather than scattered #if guards: every
// "Linux fix" we've needed (focus-stealing-prevention defeat, X server
// flush before window destruction) has a Mac and Windows analogue
// (NSApp activate, AllowSetForegroundWindow / SetForegroundWindow on
// Win, RunLoop drain on Mac). Keeping the contract uniform makes the
// fix slot obvious when the equivalent bug surfaces on the other
// platforms - and the user develops on macOS but only smoke-tests on
// Linux, so the platform asymmetry of past bug reports does NOT mean
// the other platforms are bug-free.

// Bring the given window's native peer to the foreground and grant it
// focus. Used after creating a fresh top-level window (main window,
// plugin editor) so the WM doesn't bury it under existing windows or
// open it iconified.
//
// Linux: _NET_WM_USER_TIME = max-int + WM_CHANGE_STATE NormalState +
//        _NET_ACTIVE_WINDOW (source = pager) - the ICCCM/EWMH dance
//        that defeats Mutter's focus-stealing-prevention policy.
// macOS: [NSApp activateIgnoringOtherApps:YES] then makeKeyAndOrderFront.
// Win:   AllowSetForegroundWindow + SetForegroundWindow.
//
// Must be called on the message thread, AFTER the peer exists (i.e.
// after addToDesktop / setVisible(true)). No-op if peer is null.
void bringWindowToFront (juce::ComponentPeer& peer);

// Block until the windowing system has finished processing every
// queued operation from this process. Used to space out window-
// destruction events during shutdown so the compositor isn't asked to
// process N native-window destroys + the main window destroy back to
// back - which on Mutter+XWayland has crashed the compositor and
// taken the GNOME session with it.
//
// Linux: XSync on JUCE's display.
// macOS: drain a single NSRunLoop iteration.
// Win:   PeekMessage pump.
//
// Returns when the operation completes; deterministic, not time-based.
// Cheap on a clean compositor (microseconds).
void flushWindowOperations();

// Linux/XEmbed-only today. JUCE's VST3 editor on Linux uses
// XEmbedComponent whose host X11 window is initially parented to root
// and reparented into the host peer only when componentVisibilityChanged
// fires. Calling this before attaching a plugin editor as content of
// a freshly-mapped DocumentWindow gives the parent peer time to be
// fully realised so the reparent lands on a mapped window.
//
// Empty stub on macOS / Windows.
void prepareNativePeerForChildAttach (juce::ComponentPeer& parentPeer);
} // namespace focal::platform
