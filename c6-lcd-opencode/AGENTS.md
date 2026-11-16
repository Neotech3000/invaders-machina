# C6 LCD OpenCode project

This repository is the OpenCode workspace for the Waveshare ESP32-C6-LCD-1.47
that is also used by Neotech3000/invaders-machina. It is not the cabinet game.
Do not copy the game into this tree, and do not replace Taby with an unrelated
demo.

Upstream code lives in TRIIIS-LABS/firmware-taby. OpenCode loads that checkout
through the `firmware-taby` reference. The experimental target described by
the owner is `firmware/targets/c6-lcd-1.47`. Public upstream currently ships
only `firmware/targets/round-1.32`. If the C6 target is absent, add it beside
the existing target. Leave every S3 and round target unchanged.

## Hardware

Read `docs/board.md` before changing pins, the SPI clock, or partitions.
GPIO12 and GPIO13 are native USB. Leave them alone. A serial tool may open
only USB vendor `303a` product `1001`. Never open an XR glasses port.

The microSD card, including a 50 GB card, is storage. It is not RAM. Do not
move frequently read animation frames onto it. Keep its chip select high
while the LCD uses the shared SPI pins. A missing card must still boot.

Keep the backlight at or below 50 percent.

## Playback task

`idle_01_loop` is the accepted USB animation. Assets are 120 by 224, one
shared 64-color palette, and they keep the source timing of about 19 to 20
FPS. Do not drop every other frame to make the loop look faster.

C6 playback streams GIF data from the SPIFFS partition instead of loading a
whole GIF into RAM. Wi-Fi provisioning, touch UI, and Rive stay disabled on
this target. USB control stays the default.

Before raising the SPI clock above 40 MHz, compare that rate with Waveshare's
ESP32-C6-LCD-1.47 example and the ST7789V3 write-cycle limit. Record the old
and new frame-decode time, LVGL flush time, and achieved FPS. Preserve source
timing where the panel can meet it, and report any frame that still cannot.

## Flash and memory

The attached prompt calls the chip an ESP32-C6FH8 with 8 MB flash. Waveshare's
non-touch schematic and listing describe an ESP32-C6FH4 with 4 MB. Read the
chip ID before choosing a partition table. Do not assume 8 MB.

When an 8 MB part is confirmed, the app partition is 4 MB and the SPIFFS
asset partition is 3.75 MiB. The filesystem image must fit in that partition.

Do not erase NVS or factory data, and do not run a full-chip erase. Build and
inspect a C6 bundle before asking the owner to flash. Flashing requires an
explicit yes. This cloud session has no USB bus, so a command run here cannot
reach the panel.

## Checks

From a firmware-taby checkout that contains the C6 target:

```sh
python tools/check.py
python -m unittest discover -s tests -v
python tools/build.py c6-lcd-1.47
python tools/package_release.py c6-lcd-1.47
```

Use ESP-IDF 5.4.2 for the build. Do not claim the glass shows the new frames
unless the owner confirms it or a capture is attached.

Taby character artwork stays under the Taby Artwork Terms. Do not copy it
into this repository or present it as a new character.
