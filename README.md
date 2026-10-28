# Jitters

A small cat that lives on an ESP32 Cheap Yellow Display and can be handed to the laptop over serial, then returned.

The serial line is 115200 baud. That rate is still unconfirmed. The only board port is the QinHeng CH340 at `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`. Do not open `/dev/ttyACM0` or `/dev/ttyACM1`.

## Desktop app

From `desktop/`:

```sh
npm install
npm run build
npm test
npm start
```

`npm start` opens a frameless port picker. Connect to the CH340, then hand the cat over. While the cat is on the laptop it can be hidden or returned to the panel. The laptop cat stays in its window. It still blinks and, less often, opens its mouth. Those frames stay local and do not take keyboard focus.

On the panel the tabby walks the 320×480 screen on its own, with the same blink and mouth. Invaders is only on the display. Tap PLAY along the bottom edge to start. LEFT, FIRE, and RIGHT are buttons on that same edge. After a round, tap WALK to bring the walking cat back. The resistive controller is the XPT2046 on GPIO 33, sharing the display SPI bus. The laptop does not start or play the game.

The panel keeps the 20 MHz SPI clock that already drew a stable picture. Faces are decoded into sprites once, then pushed about every 32 ms. Only the sliver the picture left behind is cleared, instead of redrawing the whole screen from the PNG.

## Firmware

PlatformIO, env `cyd`. The board profile is an ESP32-D0WD-V3 with 4MB flash and no PSRAM. The panel is an ST7796, 320x480, on the pins in `firmware/src/hardware/hardware_config.h`.

From `firmware/`:

```sh
pio run -e cyd
pio run -e cyd -t upload
```

`upload_port` in `platformio.ini` is already the CH340 by-id path. Upload speed is 115200.
