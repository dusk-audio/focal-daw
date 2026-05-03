# ADH DAW — instructions for Claude

ADH DAW is a deliberately constrained, portastudio-style DAW for Linux, JUCE 8 / C++17. The authoritative spec is [ADHDaw.md](ADHDaw.md). Read it before changing anything non-trivial.

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
- **DSP**: extracted from the user's existing Dusk Audio plugins at `/home/marc/projects/plugins/`. Shared headers live (or will live) at `plugins/plugins/shared/dsp-cores/` so both ADH DAW and the Dusk plugins are single-source-of-truth consumers.
- **JUCE**: 8.x, resolved via `-DJUCE_PATH` or sibling `../JUCE` (same scheme as the Dusk plugins repo).
- **Topology**: 16 channel strips (HPF → 4-band EQ → FET/Opto comp → sends → pan → bus assign → fader → mute/solo) → 4 aux buses (EQ + comp + fader) → master (Pultec EQ + bus comp + tape sat + fader).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/ADHDaw_artefacts/Release/ADHDaw
```

JUCE is picked up from `/home/marc/projects/JUCE/` automatically. Pass `-DJUCE_PATH=/path/to/JUCE` to override.

## Phase plan

Phase 1a (current): live mixer, no recording, no plugin hosting. Phases 1b → 5 follow [ADHDaw.md](ADHDaw.md). Don't skip ahead.

## Coding rules

- Real-time audio thread is sacred: no allocation, no locks, no logging in the audio callback. Cross-thread state via `std::atomic` or `juce::AbstractFifo` only.
- Prefer JUCE primitives (`juce::AudioBuffer<float>`, `juce::dsp::*`, `SmoothedValue`) over hand-rolled DSP plumbing.
- Default to writing no comments; only comment the *why* when it's non-obvious.
- Don't add features, validation, or abstractions beyond what the task at hand needs.
