// Opt into JUCE's exposed-X11 surface so juce::XWindowSystem (and its
// shared ::Display* connection) is reachable. The define MUST appear
// before juce_gui_basics is included, which happens transitively via
// PlatformWindowing.h - so it sits at the very top of this file.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include "PlatformWindowing.h"

#include <cstdio>

namespace focal::platform
{
namespace
{
// JUCE owns a single ::Display* connection to the X server which we
// reuse here. Opening a fresh XOpenDisplay(nullptr) connection works
// but creates a parallel client-id whose user-time tracking is
// separate from JUCE's - some Mutter versions key focus-stealing
// prevention per connection, so a USER_TIME we set on a fresh
// connection isn't seen by Mutter's policy check against JUCE's
// connection. Reusing JUCE's display avoids that entire class of
// subtle, version-specific failure.
::Display* juceDisplay()
{
    auto* sys = juce::XWindowSystem::getInstanceWithoutCreating();
    return sys != nullptr ? sys->getDisplay() : nullptr;
}

// Return the native X window of any visible top-level OTHER than the
// one departing, or None when none exists (genuine final shutdown).
::Window pickSiblingFocusTarget (juce::Component& departing)
{
    auto* departingPeer = departing.getPeer();
    if (departingPeer == nullptr) return None;

    const int n = juce::TopLevelWindow::getNumTopLevelWindows();
    for (int i = 0; i < n; ++i)
    {
        auto* tlw = juce::TopLevelWindow::getTopLevelWindow (i);
        if (tlw == nullptr || ! tlw->isVisible()) continue;
        auto* peer = tlw->getPeer();
        if (peer == nullptr || peer == departingPeer) continue;
        return (::Window) (uintptr_t) peer->getNativeHandle();
    }
    return None;
}
} // namespace

void bringWindowToFront (juce::ComponentPeer& peer)
{
    auto* d = juceDisplay();
    if (d == nullptr) return;

    const auto win = (::Window) (uintptr_t) peer.getNativeHandle();
    const auto userTimeAtom    = ::XInternAtom (d, "_NET_WM_USER_TIME",   False);
    const auto changeStateAtom = ::XInternAtom (d, "WM_CHANGE_STATE",     False);
    const auto activeWinAtom   = ::XInternAtom (d, "_NET_ACTIVE_WINDOW",  False);

    // Max-int timestamp so user_time >= every prior user-input
    // timestamp the WM has on file. Mutter's focus-stealing-prevention
    // policy compares timestamps; a higher value reads as "this is
    // the most recent user gesture", which the policy honours.
    unsigned long t = 0x7FFFFFFFUL;
    ::XChangeProperty (d, win, userTimeAtom, XA_CARDINAL, 32,
                        PropModeReplace,
                        reinterpret_cast<unsigned char*> (&t), 1);

    // ICCCM "deminimise" request - the only standard way to ask the WM
    // to take a window out of the Iconic state from the client side.
    ::XEvent demin {};
    demin.xclient.type         = ClientMessage;
    demin.xclient.window       = win;
    demin.xclient.message_type = changeStateAtom;
    demin.xclient.format       = 32;
    demin.xclient.data.l[0]    = 1;  // NormalState
    ::XSendEvent (d, DefaultRootWindow (d), False,
                   SubstructureRedirectMask | SubstructureNotifyMask,
                   &demin);

    // EWMH activate. data.l[0] = 2 = "source: pager / taskbar" -
    // higher trust level than a regular client request, so Mutter
    // honours it even on its strictest focus policy.
    ::XEvent act {};
    act.xclient.type         = ClientMessage;
    act.xclient.window       = win;
    act.xclient.message_type = activeWinAtom;
    act.xclient.format       = 32;
    act.xclient.data.l[0]    = 2;
    act.xclient.data.l[1]    = (long) t;
    ::XSendEvent (d, DefaultRootWindow (d), False,
                   SubstructureRedirectMask | SubstructureNotifyMask,
                   &act);

    ::XFlush (d);
}

void flushWindowOperations()
{
    if (auto* d = juceDisplay())
        ::XSync (d, False);
}

void prepareNativePeerForChildAttach (juce::ComponentPeer&)
{
    // Currently no Linux-specific prep needed beyond what JUCE does
    // internally - left as a hook for the XEmbed-timing fix in
    // ChannelStripComponent::PluginEditorWindow if we want to
    // consolidate it here later.
}

void prepareForTopLevelDestruction (juce::Component& topLevel)
{
    juce::Component::unfocusAllComponents();
    topLevel.giveAwayKeyboardFocus();

    if (auto* d = juceDisplay())
    {
        const auto sibling = pickSiblingFocusTarget (topLevel);
        if (sibling != None)
        {
            // Pointing focus at a live sibling keeps mutter's wayland-
            // side focus_window coherent across the upcoming UnmapNotify;
            // XSync round-trips the X protocol but does NOT wait for
            // mutter to drain its event queue, so a None target can
            // race the unmap and trip meta_window_unmanage.
            ::XSetInputFocus (d, sibling, RevertToParent, CurrentTime);
            std::fprintf (stderr,
                          "[Focal/X] focus -> sibling 0x%lx\n",
                          (unsigned long) sibling);
        }
        else
        {
            ::XSetInputFocus (d, None, RevertToNone, CurrentTime);
            std::fprintf (stderr, "[Focal/X] focus -> None (no sibling)\n");
        }
        std::fflush (stderr);

        if (auto* peer = topLevel.getPeer())
        {
            const auto win = (::Window) (uintptr_t) peer->getNativeHandle();
            ::XWithdrawWindow (d, win, DefaultScreen (d));
        }

        ::XSync (d, False);
    }

    flushWindowOperations();
}

void clearXInputFocus()
{
    if (auto* d = juceDisplay())
    {
        ::XSetInputFocus (d, None, RevertToNone, CurrentTime);
        ::XSync (d, False);
    }
}
} // namespace focal::platform
