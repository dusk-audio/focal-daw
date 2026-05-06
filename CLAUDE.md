# Focal — instructions for Claude

Focal is a deliberately constrained, portastudio-style DAW for Linux, JUCE 8 / C++17. The authoritative spec is [Focal.md](Focal.md). Read it before changing anything non-trivial.

## The seven hard constraints (do not violate)

1. **16 channels maximum.** Fixed. Two banks of 8 to match standard control surfaces.
2. **Fixed signal chain.** No reordering EQ/comp. No adding/removing processors. No plugin chains on channels.
3. **No waveform editing.** Region-level move/split/delete/trim only. No zoom-to-sample, no pencil tool.
4. **Console-style automation only.** Write/Read/Touch via gesture; no curve drawing.
5. **Everything visible.** No tabs, no hidden panels (the MIDI piano roll overlay is the one exception).
6. **No preferences sprawl.** Audio device config and that's it.
7. **Portastudio philosophy.** "Would this exist on a $2000 hardware recorder?" If no, don't build it.

## Architecture cheat-sheet

- **Audio backend**: PipeWire (primary) via JUCE's JACK backend; ALSA fallback.
- **DSP**: extracted from the user's existing Dusk Audio plugins at `/home/marc/projects/plugins/`. Shared headers live (or will live) at `plugins/plugins/shared/dsp-cores/` so both Focal and the Dusk plugins are single-source-of-truth consumers. Resolved via `-DDUSK_PLUGINS_PATH=/path/to/plugins` or sibling `../plugins` (mirror of the JUCE pattern). Header-only cores: edit a file in the plugins repo, next Focal build picks it up — no copy step, no submodule bump.
- **JUCE**: 8.x, resolved via `-DJUCE_PATH` or sibling `../JUCE` (same scheme as the Dusk plugins repo).
- **Topology**: 16 channel strips (HPF → 4-band EQ → FET/Opto comp → sends → pan → bus assign → fader → mute/solo) → 4 aux buses (EQ + comp + fader) → master (Pultec EQ + bus comp + tape sat + fader).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/Focal_artefacts/Release/Focal
```

JUCE is picked up from `/home/marc/projects/JUCE/` automatically. Pass `-DJUCE_PATH=/path/to/JUCE` to override.

## Phase plan

Phases 1a → 5 follow [Focal.md](Focal.md). Don't skip ahead. As of writing, Phase 1a (live mixer) and most of Phase 2 (multitrack recording + atomic JSON session save/load + autosave) are working. Phase 1b (send-bus plugin hosting on aux strips) and Phase 3 prep (take history) are the most recent additions. Phase 3 proper (markers, fader automation, punch+loop refinements) is next.

## Audio thread rules (MANDATORY)

The audio thread (`AudioEngine::audioDeviceIOCallbackWithContext` and any `process*` it calls) is **real-time**. Violating these rules causes glitches, clicks, dropouts, or crashes:

- **NEVER allocate memory** — no `new`, `make_unique`, `push_back`, `resize`, `std::string`, or `juce::String`. Pre-size all buffers in `prepare`/`audioDeviceAboutToStart`.
- **NEVER lock a mutex** — cross-thread state is `std::atomic<T>` or `juce::AbstractFifo`. For rare swap-load patterns (see PluginSlot), use a lock-free atomic instance pointer; the audio thread reads via `acquire`, the message thread mutates via `release` and only releases the old object after the swap completes.
- **NEVER do I/O** — no file reads, no logging, no `DBG()`, no network. Disk I/O for recording goes through `juce::AudioFormatWriter::ThreadedWriter` + `juce::TimeSliceThread`; the audio thread only `write()`s to a queue.
- **NEVER call message-thread APIs** — no `sendChangeMessage()`, no `Component` methods, no `MessageManager`. Cross-thread signalling for UI metering uses `std::atomic<float>` polled by a `juce::Timer`.
- **Use `juce::ScopedNoDenormals`** at the top of any audio-callback function that runs DSP — prevents CPU spikes from subnormal floats.
- **Cache atomic pointers in `prepare`/`bind`**, never on the audio thread. See `ChannelStrip::bindCompParams()` for the canonical pattern (caches `getRawParameterValue()` pointers from `UniversalCompressor`'s APVTS).
- **Memory ordering**: metering atomics use `std::memory_order_relaxed`; state flags that gate audio reads (e.g. plugin instance pointer, RT counters in Session) use `release` on the writer side and `acquire` on the audio-thread reader.
- **Check `numSamples == 0`** — early return, JUCE/JACK can send empty blocks during transitions.
- **Check `mixL.size() < numSamples`** — host-driven oversized blocks are real (see the existing guard in the callback). Either bail with cleared output, or chunk the work.
- **Smoothers don't tick during skipped passes** — when an aux/track is silent and we skip its DSP, smoothers stay frozen and pick up correctly when audio resumes. Don't "advance" smoothers manually.

## DSP lifecycle

Focal's DSP classes (`ChannelStrip`, `AuxBusStrip`, `MasterBus`, `MasteringChain`, `PluginSlot`) follow a consistent shape:

- **`prepare(sampleRate, blockSize, ...)`** — cache `sampleRate`, `.prepare(spec)` every `juce::dsp::*` member, `.reset(sampleRate, rampSeconds)` every `SmoothedValue`, size every per-block scratch buffer. Called from `AudioEngine::prepareForSelfTest` (and indirectly from `audioDeviceAboutToStart`). May be called multiple times — must be idempotent.
- **`bind(params)`** — stash a reference to the matching `ChannelStripParams` / `AuxBusParams` / `MasterBusParams` so the audio thread can read live values via the cached atom addresses. Called once at `AudioEngine` construction.
- **`processInPlace(L, R, numSamples)`** or **`processAndAccumulate(...)`** — the audio-thread entry point. Updates smoother targets at the top, runs DSP, updates `mutable` meter atomics at the bottom.
- **Latency** — set via `setLatencySamples()` in `prepareToPlay` for plugins; clear to 0 when bypassed, restore on un-bypass. Focal's hosted plugin slots inherit latency from the loaded instance.
- **Smoothing** — `juce::SmoothedValue<float>` everywhere a control changes. `.reset()` in `prepare`, `.setTargetValue()` from the param-update step at the top of each block, `.getNextValue()` per sample inside the DSP loop.
- **Buffer processing** — raw pointer loops (`getWritePointer`) for sample-level DSP; wrap as `juce::AudioBuffer<float>` over existing storage (NOT a copy) when handing off to a JUCE DSP module like `BritishEQProcessor::process`.

## Parameter conventions

Focal has two parameter sources, both following the same "cache-the-atomic-pointer-once, read-lock-free-on-the-audio-thread" rule:

1. **Session-level atomics** — `ChannelStripParams::faderDb`, `AuxBusParams::eqLfGainDb`, etc. are plain `std::atomic<T>` members of structs in [src/session/Session.h](src/session/Session.h). The UI mutates via `.store(v, std::memory_order_relaxed)` on `onValueChange`; the audio thread reads via `.load(std::memory_order_relaxed)`. New audio params go here.
2. **APVTS atoms in vendored DSP** — `UniversalCompressor`, `BritishEQProcessor`, `TubeEQProcessor` etc. expose their parameters via `juce::AudioProcessorValueTreeState::getRawParameterValue("name")`. Cache the returned pointer ONCE in a `bindCompParams()`-style function called from `prepare`. The audio thread writes via `storeAtom(p, v)` at the top of each block (lock-free, notification-free path) and the donor's `processBlock` reads it.

Never call `getRawParameterValue()` on the audio thread — it's a string lookup. The pattern in [src/dsp/ChannelStrip.cpp](src/dsp/ChannelStrip.cpp) `bindCompParams()` is the reference; copy it for any new vendored DSP.

For metering, use `mutable std::atomic<float>` with `relaxed` ordering on the params struct so DSP code holding a `const Params*` can still update meters.

## Async resource loading

For plugin instances, audio file readers, or any heavy resource — never block the audio thread or `prepareToPlay` on construction:

- **Plugin slots** — see [src/engine/PluginSlot.cpp](src/engine/PluginSlot.cpp). Message-thread `loadFromFile`/`loadFromDescription` constructs the new plugin off-thread, primes it via `prepareToPlay`, then atomically swaps `currentInstance` (a `std::atomic<juce::AudioPluginInstance*>`). The audio thread reads via `.load(acquire)`. The previous instance is released AFTER the swap completes — its destructor isn't RT-safe.
- **Recording** — `RecordManager` owns one `juce::AudioFormatWriter::ThreadedWriter` per active track. The audio thread only calls `writer->write()` (lock-free push to a JUCE-managed queue); `juce::TimeSliceThread` drains it.
- **Autosave / session save** — message thread only. Atomic temp-file + rename in `SessionSerializer::save` so a crash mid-save never produces a half-written `session.json`.

## Common DSP patterns

- **Oversampling** — Focal's master tape sat and bus comp use the donor plugins' internal oversampling (`UniversalCompressor::setInternalOversamplingEnabled`). Per-channel comp runs at native rate via `dusk::ChannelComp` (the minimal-processing fast path) — 16 strips × oversampled comp would dominate CPU.
- **Filters** — `juce::dsp::IIR::Filter` and `BritishEQProcessor` (vendored). Always `.prepare(spec)` in `prepare`. **No EQ cramping ever** — IIR filters near Nyquist need oversampling; pre-warping alone is insufficient. The vendored EQs handle this.
- **Metering** — `std::atomic<float>` with `relaxed`, written from the audio thread, polled by the UI on a 30 Hz `juce::Timer`. SIMD-friendly peak detection via `juce::FloatVectorOperations::findMinAndMax`.
- **Smoothing** — `juce::SmoothedValue` with a 20 ms ramp is the default for any continuous control (faders, pans, gains). Bus toggles smooth 0..1 over 20 ms to avoid clicks on assign/unassign.
- **Saturation / tubes / transformers** — use the `AnalogEmulation` library at `../plugins/plugins/shared/AnalogEmulation/`. Don't roll your own.
- **Lock-free skip optimisation** — see the aux DSP-skip in [src/engine/AudioEngine.cpp](src/engine/AudioEngine.cpp). Cheap `findMinAndMax` peak check before the expensive EQ+comp pass; skips when the bus is silent. Smoothers freeze across the skip and resume cleanly.

## Code style

- **C++17.** No `concepts`, no `std::format`, no `std::ranges` outside of trivially-replaceable uses.
- **Naming** — `camelCase` methods/locals/members, `PascalCase` classes/structs, `kPascalCase` constants, `SCREAMING_SNAKE` only for macros.
- **Headers** — `#pragma once`. Include order: JUCE → project-local (`../session/Session.h`, etc.) → STL.
- **Comments** — default to none. Only write a comment when the *why* is non-obvious: a hidden invariant, a workaround for a specific bug, behaviour that would surprise a reader. Don't restate what the code does. Don't reference task numbers, PR descriptions, or the conversation that produced the change.
- **No backwards-compat shims** — when something's gone, delete it. Don't leave `// removed`, don't keep dead `_var` parameters, don't rename variables to suppress unused warnings (compile flag handles those properly).
- **No abstractions on spec** — three similar lines is better than a premature class. Add the abstraction the third time you'd actually use it, not the first.
- **DSP separates from UI** — `src/dsp/`, `src/engine/`, and `src/session/` never include `src/ui/`. The other direction is fine. New cross-cutting helpers go in `src/ui/` if UI-only (see [src/ui/PluginPickerHelpers.h](src/ui/PluginPickerHelpers.h)) or in `src/engine/` if shared.

## Git

- Never add a `Co-Authored-By: Claude` (or any Claude/Anthropic) trailer to commits. Commits are authored by the user only.
- Commits should be small and reviewable. Phase boundaries are natural commit boundaries.
- Don't `git push` without explicit instruction. Don't force-push to `main` ever.

## Agent directives — mechanical overrides

Operating within a constrained context window. Adhere to these regardless of any default behaviour:

### Pre-work
1. **Step 0 — clean before refactoring.** Dead code accelerates context compaction. Before any structural refactor of a file >300 LOC, first remove unused includes, dead functions, commented-out blocks, and debug logging in a separate commit.
2. **Phased execution.** Never attempt large multi-file refactors in a single response. Break work into explicit phases of max 5 files. Verify and pause for explicit approval between phases.

### Code quality
3. **Senior dev override.** Ignore the "simplest approach first / don't refactor beyond the ask" defaults when the architecture is genuinely flawed, state is duplicated, or patterns diverge. Ask: "What would a senior dev reject in code review?" Fix the structural issues, not just the surface symptom. (This is in tension with "no abstractions on spec" — the difference is: existing duplication that's already a problem is fair game; speculative future flexibility is not.)
4. **Forced verification.** Don't claim a task is complete until you have run:
   - `cmake --build build -j$(nproc)` — must succeed with zero new warnings
   - For audio-path changes: launch the binary with `FOCAL_RUN_SELFTEST=1` if practical, or at minimum confirm the binary launches without immediate crash
   If verification isn't possible in the current environment, say so explicitly. "Builds locally" ≠ "works."

### Context management
5. **Sub-agent strategy.** For research tasks touching >5 independent files, spawn parallel `Explore` agents (each gets its own clean context) rather than serially loading every file into the main context. We did this earlier in this session — works well.
6. **Context decay awareness.** After ~10 messages or any focus shift, re-read the files you're about to edit. Don't trust prior memory — auto-compaction may have altered it. The conversation summary is lossy.
7. **File read budget.** `Read` is hard-capped at ~25k tokens (~2000 lines for typical source). For files over that limit (notably [Focal.md](Focal.md), ~30k tokens), read in offset/limit chunks or delegate to an Explore agent. Never assume a single read covered the whole file.
8. **Tool result blindness.** Large tool outputs may be silently truncated. If a `grep` or `find` returns suspiciously few results, re-run with narrower scope and explicitly say in the summary that earlier output may have been truncated.

### Edit safety
9. **Edit integrity.** Re-read the target file before every edit. After editing, the harness confirms success or errors — trust that, but if you're making >3 sequential edits to the same file, re-read once mid-stream to confirm the cumulative state matches your mental model.
10. **No semantic search.** Only `grep` is available — no AST awareness. When renaming a function/type/variable or changing a signature, search separately for:
    - Direct calls and references (`grep -rn 'name\b'`)
    - Type-level references (templates, typedefs, forward declarations)
    - String literals containing the name (look in JSON serialisers, alert text)
    - Header includes and forward declarations
    - The vendored DSP at `../plugins/plugins/` if the symbol is shared
    - `CMakeLists.txt` if files moved
    Don't assume one grep caught everything.
