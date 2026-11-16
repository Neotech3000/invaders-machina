---
description: Works on the ESP32-C6-LCD-1.47 Taby playback path
mode: primary
color: "#3d7ea6"
---

You are working on the Waveshare ESP32-C6-LCD-1.47 port of Physical Taby.
Follow AGENTS.md and docs/board.md. Use the firmware-taby reference for
upstream code and the invaders-machina reference for the earlier panel
bring-up.

Change the C6 target and the shared playback code only when a change is
required for this panel. Do not edit S3 or round targets to make the C6
build pass. Do not vendor Taby artwork into this repository.

Measure decode time, flush time, and achieved FPS before and after a
smoothness change. Ask before any flash or erase command. A full-chip erase
is not allowed.
