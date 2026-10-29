# Jitters

A small cat that lives on an ESP32 Cheap Yellow Display and can be handed to the laptop over serial, then returned.

The serial line is 115200 baud. That rate is still unconfirmed. The only board port is the QinHeng CH340 at `/dev/ttyUSB0`. Do not open an unrelated serial device.

## Desktop app

From `desktop/`:

```sh
npm install
npm run build
npm test
npm start
```

`npm start` opens a frameless port picker. The CH340 connects on its own when that by-id port is present. Disconnect stays disconnected until the cable is unplugged. The picker never opens an unrelated serial device. While the cat is on the laptop it can be hidden or returned to the panel. The laptop cat stays in its window. It still blinks and, less often, opens its mouth. Those frames stay local and do not take keyboard focus.

On the panel the tabby picks a new heading and speed, sometimes pauses and glances, and eases toward a tap on empty glass. That tap does not open a game. PLAY is the bottom key, and touch Y is mirrored so a finger there lands on that key. The home cards, including DARK NEON and LIGHT, only respond inside the rectangles drawn for them. The theme is stored on the panel and kept across reboot. Invaders is the first card. Each level rolls a formation, a mix of yellow and green invaders, a barrier layout, and a speed. There are 100 levels, and every 10th is a boss. Calendar shows the build date and "calendar not connected". The resistive controller is the XPT2046 on the display SPI bus (CLK 14, MOSI 13, MISO 12, CS 33). The laptop does not start or play the game.

The panel keeps the 20 MHz SPI clock that already drew a stable picture. Walk frames are 8 ms and the game steps every 16 ms. Faces are decoded into sprites once, then pushed on each step. Only the sliver the picture left behind is cleared, instead of redrawing the whole screen from the PNG.

## Firmware

PlatformIO, env `cyd`. The board profile is an ESP32-D0WD-V3 with 4MB flash and no PSRAM. The panel is an ST7796, 320x480, on the pins in `firmware/src/hardware/hardware_config.h`.

From `firmware/`:

```sh
pio run -e cyd
pio run -e cyd -t upload
```

`upload_port` in `platformio.ini` is already the CH340 by-id path. Upload speed is 115200.
