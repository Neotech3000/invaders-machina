# Invaders Machina

A space shooter for the ESP32 Cheap Yellow Display (CYD, ST7796 320×480 panel, XPT2046 touch).

The panel boots into an attract demo where a CPU plays the game. Tap PLAY to open the menu. The top-left BACK key leaves the menu and returns to the demo.

## Menu

- **PLAY**: continue from your current level.
- **CHECKPOINTS**: jump back to any checkpoint you have reached. A checkpoint is saved every 20 levels.
- **CUSTOM**: ship, guns, and laser colour. Forged guns are listed here.
- **SHOP**: themes, ships, laser colours, guns, timed power-ups, and weapon upgrades. Coins dropped in a level are the currency.
- **Theme**: tap the theme card to switch the one you own.
- **DIVINE MACHINES**: the Olympian powers you have unlocked. Equip one, or more after buying Multi.
- **FORGE**: combine two guns. The new gun keeps its own level and attributes.
- **RESTART GAME** (bottom): asks "Are you sure?" before resetting progress.

## Play

Hearts in the top right are lives. You start with 3. Losing a life makes the ship blink and the level stays as it was.

Enemies are standard aliens, Assault troopers, and Cannon units. A cannon shell bursts on impact. On this panel that blast is about 3 mm.

Every 5th level is a boss. Beating a normal boss can start a raid. The raid chance is 20% through level 24. From level 25 it rises by 5 percentage points at levels 25, 35, 45, and every 10 levels after that, up to 100%. After the first raid boss is beaten, other levels have a 40% chance of a mini boss.

Every 25 levels a god descends: Zeus, Hera, Poseidon, Demeter, Athena, Ares, Apollo, Artemis, Aphrodite, Hephaestus, Hermes, then Dionysus, and then the cycle repeats. The arena shows that god's pillars and mark. The first god has 4 goon waves, each led by a mini boss. Each later god adds 2 waves. After the waves, the god drops in with three health bars stacked across the screen. Emptying a bar changes the god's fighting style. A raid boss fights beside the god. Defeating the god unlocks that god's Divine Machine.

From level 30 every enemy has +1 life, then +1 more every 20 levels, capped at +3.

## Divine Machines

Each machine is the power of one Olympian. Beating that god awards it. Equip machines from the Divine Machines page. The shop's Multi upgrade costs $300 and equips one more machine per purchase.

Hera's Double puts a second ship on the mirrored side of the playfield. It stays inside the screen and fires the same weapon as the player. Demeter's Healing Ray grants +2 temporary life, then +1, for 3.5 seconds, on a 5 second cooldown.

## Shop and forge

The shop POWER tab sells a shield, a spare heart, and five short boosts: Rush, Fury, Wide, Veil, and Salvo. The same boosts can also drop during a level. The UPGRD tab levels each owned gun, thrusters, and Multi.

Forge joins two guns into a new one. Each forged gun is upgraded on its own and saved with the rest of the gear.

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
pio run -e cyd -t upload --upload-port /dev/ttyUSB0
```

The upload port is the QinHeng CH340 at `/dev/ttyUSB0`. Do not point it at an unrelated serial device.

## Layout

- `firmware/src/hardware/board.cpp`: the whole game (attract, menu, levels, gods, shop, forge, HUD).
- `firmware/src/hardware/touch_map.h`: measured touch mapping. Do not change it without re-measuring.
- `firmware/src/hardware/hardware_config.h`: panel pinout.
- `firmware/test/touch_map_test.cpp`: touch mapping check.
- `web/index.html`: the same game in the browser.
