#include "PlatformWindowing.h"

// Stubs for now. Win32 equivalents (eventual implementations):
//   bringWindowToFront     -> AllowSetForegroundWindow(GetCurrentProcessId())
//                              + SetForegroundWindow(hwnd) on the
//                              peer's HWND (peer.getNativeHandle()).
//   flushWindowOperations  -> while (PeekMessage(&msg, ...)) DispatchMessage.
//   prepareNativePeer...   -> no-op (Win32 SetParent is synchronous).

namespace focal::platform
{
void bringWindowToFront (juce::ComponentPeer&)             {}
void flushWindowOperations()                                {}
void prepareNativePeerForChildAttach (juce::ComponentPeer&) {}
void prepareForTopLevelDestruction (juce::Component& topLevel)
{
    // Win32 doesn't have the focused-window-destroy assertion either,
    // but defocusing before destruct is good hygiene and matches the
    // contract callsites expect.
    juce::Component::unfocusAllComponents();
    topLevel.giveAwayKeyboardFocus();
}
} // namespace focal::platform
