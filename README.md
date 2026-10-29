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

`npm start` opens a frameless port picker. The CH340 connects on its own when that by-id port is present. Disconnect stays disconnected until the cable is unplugged. The picker never opens an unrelated serial device. While the cat is on the laptop it can be hidden or returned to the panel. Click the laptop cat to pet it: the eyes widen for a moment. The laptop cat stays in its window. It still blinks and, less often, opens its mouth. Those frames stay local and do not take keyboard focus. The picker shows where the cat is (display, handoff, or laptop) and one primary action for that step.

On the panel the tabby picks a new heading and speed, sometimes pauses and glances, and eases toward a tap on empty glass. That tap does not open a game. One or two friends (black, gray, or orange; the broken calico cell is left out) sometimes walk in from the side, change pace, pause, and leave again. PLAY is the bottom key and opens home. Home cards are Invaders Machina, Tetris, Snake, Calendar, the theme, Care, and Custom. Care shows hunger and mood; FEED fills hunger and CARE lifts mood, and both fall while Jitters is on the panel. DARK NEON, LIGHT, BLUE, and AMBER are stored on the panel and kept across reboot. Invaders Machina rolls 300 levels and starts with three lives. A boss room arrives every 5, with a health bar and one of five patterns: layered shields, extra drones, a hatch that only takes damage while it reads OPEN, a spray, or a laser you can walk off. Beating a boss has a 1 in 5 chance to open a raid. Clear that raid's goons before its boss can be hurt. A raid win can add a drone that flies with the ship and keeps doing so after a reboot. Any kill can drop a heart or a level for the equipped gun, and those gun levels stay on the panel. A boss kill can drop the next locked gun. The cannon is free. Twin, wave, rail, nova, and homing join shotgun, assault, and beam as locked guns. The beam fires for about three seconds and then rests. Aliens still shoot back; a hit costs a life and restarts that level. Holding FIRE repeats. Custom, from home or from the pause screen, sets the laser color and the equipped gun. Unlocks stay on the panel. BACK during Invaders Machina pauses; Resume, Custom, and Home are on that screen. Tetris and Snake still use BACK to return home. Tetris shows SCORE at the top left, and that number rises when a line clears. The fight scrolls a two-speed starfield. Planets and black holes drift past, and each level adds one backdrop: asteroids, a planet horizon, nebula bands, or debris. Calendar shows the build date and "calendar not connected". While the cat is on the laptop the panel says so, and while the handoff is starting it says "To the laptop". The resistive controller is the XPT2046 on the display SPI bus (CLK 14, MOSI 13, MISO 12, CS 33). GPIO 36 is the interrupt and is not required to read a tap. Both touch axes are mirrored, and a point outside a drawn rectangle is not pulled onto a button. The laptop does not start or play the game.

The panel keeps the 20 MHz SPI clock that already drew a stable picture. Walk frames are 8 ms and the game steps every 16 ms. Faces are decoded into sprites once, then pushed on each step. Only the sliver the picture left behind is cleared, instead of redrawing the whole screen from the PNG.

## Firmware

PlatformIO, env `cyd`. The board profile is an ESP32-D0WD-V3 with 4MB flash and no PSRAM. The panel is an ST7796, 320x480, on the pins in `firmware/src/hardware/hardware_config.h`.

From `firmware/`:

```sh
pio run -e cyd
pio run -e cyd -t upload
```

`upload_port` in `platformio.ini` is already the CH340 by-id path. Upload speed is 115200.
