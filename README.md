# Invaders Machina

A space shooter for the ESP32 Cheap Yellow Display (CYD, ST7796 320×480 panel, XPT2046 touch).

The panel boots into an attract demo where a CPU plays the game. Tap PLAY to open the menu.

## Menu

- **PLAY**: continue from your current level.
- **CHECKPOINTS**: jump back to any checkpoint you have reached (every 20 levels). Checkpoints are saved to flash.
- **CUSTOM**: ship, guns, and laser colour.
- **Theme**: tap to cycle.
- **RESTART GAME** (bottom): asks "Are you sure?" before resetting progress.

## Play

- Hearts in the top right show lives. Losing a life makes the ship blink and keeps the level as it was.
- Enemies: standard aliens, Assault (assault weapons), and Cannon (shells burst on impact with a 3 mm blast radius).
- Every 5th level is a boss. Bosses can turn into a raid. After the first raid boss is beaten, each level has a 40% chance of a mini boss.
- Every 25 levels a god appears: a themed arena, four waves of goons with mini bosses, then the god. The god has three health bars and changes fighting style as each bar empties.

## Play on a laptop (web version)

`web/index.html` is one self-contained file. It runs in any modern browser on Windows, macOS, Linux, ChromeOS, or phones, and needs no install or internet connection.

1. Download or clone this repo.
2. Double-click `web/index.html` (or drag it into Chrome, Edge, Firefox, or Safari).

Controls:

- **Move:** Left/Right arrows or A/D, or move the mouse or trackpad (the ship follows the pointer).
- **Fire:** Space, W or Up arrow, or hold the mouse or trackpad button.
- **Menus:** click, or Up/Down and Enter.
- **Pause / back:** Esc.

Saves are kept in the browser (localStorage) on that machine.

## Build

From `firmware/`:

```sh
pio run -e cyd
pio run -e cyd -t upload
```

The upload port is the QinHeng CH340 at `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`. Do not point it at `/dev/ttyACM*`.

## Layout

- `firmware/src/hardware/board.cpp`: the whole game (attract, menu, levels, gods, HUD).
- `firmware/src/hardware/touch_map.h`: measured touch mapping. Do not change it without re-measuring.
- `firmware/src/hardware/hardware_config.h`: panel pinout.
- `firmware/test/touch_map_test.cpp`: touch mapping check.
