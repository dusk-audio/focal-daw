// Opt into JUCE's exposed-X11 surface so juce::XWindowSystem (and its
// shared ::Display* connection) is reachable. The define MUST appear
// before juce_gui_basics is included, which happens transitively via
// PlatformWindowing.h - so it sits at the very top of this file.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include "PlatformWindowing.h"

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
    // Two layers of focus need to come off the window before destroy:
    //
    //   1. JUCE's internal component-tree focus model (unfocusAll +
    //      giveAwayKeyboardFocus). Without this, JUCE-level focus
    //      callbacks fire on a half-destroyed widget tree.
    //   2. The X protocol's input focus, tracked separately by the
    //      compositor. Mutter's focus_window only updates when it
    //      observes a FocusOut event on the X server; JUCE's calls
    //      above DON'T issue XSetInputFocus, so the compositor
    //      keeps recording our window as focused. When setVisible
    //      (false) then unmaps the toplevel, mutter aborts at
    //      meta_window_unmanage with "focus_window != window" and
    //      takes the Wayland session down with it.
    //
    // The XSetInputFocus call hands focus back to the root pointer
    // (PointerRoot + RevertToPointerRoot), which is the safest target
    // - no specific window has to exist for it. The XSync that
    // follows is load-bearing: it round-trips with the X server so
    // mutter actually processes the FocusOut before we proceed to
    // unmap or destroy.
    juce::Component::unfocusAllComponents();
    topLevel.giveAwayKeyboardFocus();

    if (auto* d = juceDisplay())
    {
        ::XSetInputFocus (d, PointerRoot, RevertToPointerRoot, CurrentTime);
        ::XSync (d, False);
    }

    flushWindowOperations();
}
} // namespace focal::platform
