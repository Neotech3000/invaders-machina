---
name: C6 playback
description: Diagnose and smooth GIF playback on the ESP32-C6-LCD-1.47 Taby target
---

## When to use

Use this skill for `idle_01_loop`, SPIFFS GIF streaming, the 40 MHz SPI
clock, or a report of C6 frames per second. Do not use it to redesign the
cabinet game in invaders-machina.

## Workflow

1. Read `AGENTS.md` and `docs/board.md`.
2. Open the `firmware-taby` reference. Find `firmware/targets/c6-lcd-1.47`.
   If it is missing, say so before creating it, and base the new target on
   `round-1.32` plus the pin table in `docs/board.md`.
3. Find the GIF stream path, the LVGL flush callback, and the source frame
   delay for `idle_01_loop`.
4. Add bounded diagnostics that record decode time, flush time, and achieved
   FPS. Keep them free of personal data and turn them off in a release
   bundle unless the owner asks to keep them.
5. Compare those numbers with the source timing, about 19 to 20 FPS. Keep
   that rate when the panel can meet it. Name any frame that is still dropped
   and why.
6. Review a 40 MHz SPI clock against Waveshare's official ESP32-C6-LCD-1.47
   example before changing it. Write down the old and new measurements.
7. Run `python tools/check.py` and `python -m unittest discover -s tests -v`.
   Build with ESP-IDF 5.4.2 through `python tools/build.py c6-lcd-1.47` when
   the target exists. Package with `python tools/package_release.py` and
   inspect the bundle. Do not flash it.
8. Finish with measured FPS, RAM and flash use, the files changed, and what
   the glass has not yet confirmed.
