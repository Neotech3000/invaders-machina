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

`npm start` opens a frameless port picker. The CH340 connects on its own when that by-id port is present. Disconnect stays disconnected until the cable is unplugged. The picker never opens `/dev/ttyACM0` or `/dev/ttyACM1`. While the cat is on the laptop it can be hidden or returned to the panel. Click the laptop cat to pet it: the eyes widen for a moment. The laptop cat stays in its window. It still blinks and, less often, opens its mouth. Those frames stay local and do not take keyboard focus. The picker shows where the cat is (display, handoff, or laptop) and one primary action for that step.

On the panel the tabby walks the 320×480 screen in 1px steps, about every 16 ms, a little slower when lonely, above a bottom bar, with the same blink and mouth. The bar shows a mood word, a five-pip need, and whether the link is solo, laptop, or leaving. Tap the cat to pet it: the eyes widen, the need fills, and the backlight dips. Tap the cat again while it reacts, or tap the bar, to open the home screen. Invaders is the first game. Calendar is the other screen: it shows the build date and "calendar not connected" until a local calendar is authorized. LEFT, FIRE, and RIGHT stay on the bottom edge during a round and light up while held. LEFT moves the ship left and RIGHT moves it right. Invaders runs 100 levels. Every 10th level is a boss. Speed, target count, and boss health scale with the level. After a loss or the last boss, HOME returns to the picker, AGAIN starts over, and WALK returns to the cat. A loss says LANDED. While the cat is on the laptop the panel says so, instead of going black. The resistive controller is the XPT2046 on the display SPI bus (CLK 14, MOSI 13, MISO 12, CS 33). GPIO 36 is the interrupt and is not required to read a tap. Touch uses one mapping: raw X is mirrored so the left of the glass matches the left label, and Y increases downward. A tap at the top is not treated as the bottom bar. The 4px gaps between bar buttons are not hits. The laptop does not start or play the game.

The panel keeps the 20 MHz SPI clock that already drew a stable picture. Faces are decoded into sprites once, then pushed on each step. Only the sliver the picture left behind is cleared, instead of redrawing the whole screen from the PNG.

## Firmware

PlatformIO, env `cyd`. The board profile is an ESP32-D0WD-V3 with 4MB flash and no PSRAM. The panel is an ST7796, 320x480, on the pins in `firmware/src/hardware/hardware_config.h`.

From `firmware/`:

```sh
pio run -e cyd
pio run -e cyd -t upload
```

`upload_port` in `platformio.ini` is already the CH340 by-id path. Upload speed is 115200.
