# C6 LCD OpenCode

GitHub denied creating `Neotech3000/c6-lcd-opencode` from this session
(`createRepository` is not available to the token). The files are published
on branch `cursor/c6-lcd-opencode-c320` of invaders-machina until that
repository can be created. Move this directory out when it has its own remote.

OpenCode workspace for the Waveshare ESP32-C6-LCD-1.47 panel used alongside
[invaders-machina](https://github.com/Neotech3000/invaders-machina). The
animation work follows [Physical Taby](https://github.com/TRIIIS-LABS/firmware-taby)
without copying its character artwork into this tree.

Open this directory in OpenCode and run:

```text
/smooth
```

That command loads the `c6-playback` skill. It diagnoses GIF timing on the
C6 target and keeps the source rate of about 20 FPS. It does not flash the
board. A flash still needs an explicit yes after the C6 bundle has been
built and inspected.

`AGENTS.md` is the project briefing. `docs/board.md` is the pin and SPI
record from the panel already on the bench. Upstream firmware and the
earlier panel branch are OpenCode references, not submodules.
