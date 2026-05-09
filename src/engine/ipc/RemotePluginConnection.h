#pragma once

#include "PluginIpc.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace focal::ipc
{
// Parent-side connection to a focal-plugin-host child process. Owns:
//   • the SHM file descriptor (memfd_create) and its mmap.
//   • the child PID + fork/exec + waitpid lifecycle.
//   • the futex round-trip that drives processBlockSync.
//
// Phase 1 surface is intentionally minimal: just construct, run a stub
// echo round-trip, tear down. Plugin loading, state save/restore, editor
// embedding, and crash recovery come in subsequent phases — they're
// stubbed here so callers in Phase 2+ have stable signatures.
//
// Threading rules:
//   • construct / connect / disconnect       — message thread only.
//   • processBlockSync                       — audio thread, RT-safe
//     (no allocations, no syscalls beyond the futex pair).
//   • isCrashed                              — any thread, atomic load.
class RemotePluginConnection
{
public:
    RemotePluginConnection() = default;
    ~RemotePluginConnection();

    RemotePluginConnection (const RemotePluginConnection&) = delete;
    RemotePluginConnection& operator= (const RemotePluginConnection&) = delete;

    // Fork + exec the child binary at `hostExecutablePath`, pass it the
    // memfd via SCM_RIGHTS over a socketpair, wait for the child's
    // "ready" handshake. Returns false on any failure with `errorOut`
    // populated. Idempotent: a second connect() on a live connection is
    // a no-op.
    //
    // `extraArg` selects the host's mode: "--ipc-stub" runs the
    // dependency-light Phase-1 echo loop (used by the IPC self-test),
    // "--ipc-host" runs the full JUCE-backed plugin host (Phase 2+).
    bool connect (const std::string& hostExecutablePath,
                   const std::string& extraArg,
                   std::string& errorOut);

    // --- Control plane (Phase 2 - message-thread only) -------------------
    // Each of these does a synchronous request/reply over the control
    // socket. Not RT-safe; not callable from the audio thread.

    // Heartbeat round-trip. Returns true if the child responds within
    // `timeoutMs`. Used by the supervisor heartbeat in Phase 4.
    bool ping (int timeoutMs, std::string& errorOut);

    // Load a plugin from its `juce::PluginDescription::createXml()`
    // string at the given sample rate / block size. Fills `numInOut`
    // with the plugin's reported channel layout and `latencyOut` with
    // its reported latency in samples.
    bool loadPlugin (const std::string& pluginDescriptionXml,
                      double sampleRate, int blockSize,
                      int& numInOut, int& numOutOut, int& latencyOut,
                      std::string& errorOut);

    // Re-prepare the loaded plugin (sample-rate or block-size change).
    bool prepareToPlay (double sampleRate, int blockSize,
                         std::string& errorOut);

    // Unload the current plugin. Idempotent - a release on an empty
    // host succeeds silently.
    bool release (std::string& errorOut);

    // Plugin-state save / restore. The blob travels through the SHM
    // staging area, not the socket, to avoid exhausting the socket
    // buffer for heavy plugins.
    bool getState (std::vector<std::uint8_t>& blobOut, std::string& errorOut);
    bool setState (const std::uint8_t* data, std::size_t size,
                    std::string& errorOut);

    // Send the audio + MIDI buffers in shared memory, signal the child,
    // wait up to `timeoutNs` nanoseconds for the reply. On timeout the
    // connection's `crashed` flag is set and the function returns false
    // (caller should engage bypass). On success the audio output buffer
    // in SHM is filled and ready to read.
    //
    // Audio data is `numIn` channel pointers each `numSamples` long;
    // they're memcpy'd into the SHM input region. After successful
    // return, the caller can read from `audioOutChannel(...)` for
    // `numOut` channels.
    //
    // RT-safe: only memcpy + futex syscalls, no allocation.
    bool processBlockSync (const float* const* inChannels, int numIn,
                            int numSamples, long long timeoutNs) noexcept;

    // Read the i'th output channel from SHM. Valid pointer for the life
    // of the connection. Audio thread calls this after processBlockSync
    // returns true.
    const float* readOutChannel (int chan) const noexcept
    {
        return audioOutChannel (mappedShm, chan);
    }

    // True once the child has been declared dead (timeout, crash, or
    // explicit disconnect). Sticky for the life of the connection.
    bool isCrashed() const noexcept { return crashed.load (std::memory_order_acquire); }

    // Tear down. Idempotent. Sends SIGTERM to the child, waits briefly,
    // then SIGKILL. Unmaps SHM, closes fds.
    void disconnect();

    // Total round-trip blocks completed since connect() returned true.
    // Used by the self-test to assert progress; not RT-relevant.
    std::uint64_t getRoundTripCount() const noexcept
    {
        return roundTrips.load (std::memory_order_relaxed);
    }

private:
    int shmFd        { -1 };
    void* mappedShm  { nullptr };
    int   childPid   { -1 };
    int   socketFd   { -1 };  // for SCM_RIGHTS handoff (closed after connect)

    std::uint32_t   localSeq { 0 };
    std::atomic<std::uint64_t> roundTrips { 0 };
    std::atomic<bool> crashed { false };
};
} // namespace focal::ipc
