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
} // namespace focal::platform
