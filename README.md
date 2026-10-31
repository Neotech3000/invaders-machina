# Invaders Machina

A space shooter for the ESP32-3248S035 display. The panel boots into an attract demo where the CPU plays. Tap **PLAY** to open the menu.

This branch has the god fights, and it adds coins, a shop, ship hulls, and a forge.

## The board

You need this exact display module:

- Name on the listing: **ESP32-3248S035**, often sold as a 3.5 inch Cheap Yellow Display.
- Screen: **320×480**, driver **ST7796**.
- Touch: resistive **XPT2046** (the glass is the dull kind you press, not a phone-style glass).
- USB chip: **CH340** (the USB device name often includes QinHeng or `1a86`).
- Module: **ESP32-D0WD-V3**, **4MB** flash, no extra PSRAM.

The smaller 2.8 inch Cheap Yellow Display (240×320, ILI9341) is a different board. This project will not draw correctly on it.

You also need a **data** USB cable. A charge-only cable will power the panel and then the computer will not see a serial port. The board has two small buttons, usually labeled **BOOT** (or IO0) and **RESET** (or EN).

## Software you install

Install these, then stop. The first firmware build downloads the compiler and the screen library by itself.

| Tool | Version | Used for |
| --- | --- | --- |
| Python | 3.11 or 3.12 | PlatformIO |
| PlatformIO Core | 6.2.0 | build and flash |
| ESP32 platform | 6.13.0 (pinned in `firmware/platformio.ini`) | Arduino-ESP32 2.0.17 |
| LovyanGFX | 1.2.x | the screen |

Do not change `platform = espressif32@6.13.0` to a bare `espressif32`. In late 2026 that name installs platform 8.x, which uses a newer Arduino core this project does not build with.

## Install on Windows

1. Install **Python 3.12** from [python.org](https://www.python.org/downloads/windows/). On the first installer page, tick **Add python.exe to PATH**.
2. Open **PowerShell** and run:

```powershell
py -m pip install --upgrade pip
py -m pip install "platformio==6.2.0"
py -m platformio --version
```

If `pio` is not recognized later, use `py -m platformio` in place of `pio` in every command below.

3. Plug the display into a USB port on the computer itself, not into an unpowered hub.
4. Open **Device Manager** and expand **Ports (COM & LPT)**. You want a line like **USB-SERIAL CH340 (COM5)**. The number at the end is your port.
5. If the device is missing, or it shows a yellow mark under **Other devices**, install the CH340/CH341 driver from the chip maker WCH (search the WCH site for `CH341SER`). Unplug the board, install the driver, plug it back in, and read the COM number again.

## Install on macOS

1. Install Python 3.12 if `python3 --version` is not 3.11 or 3.12. The installer from [python.org](https://www.python.org/downloads/macos/) is enough.
2. Install the command-line tools once, in case a later step needs a compiler:

```sh
xcode-select --install
```

3. Install PlatformIO for your user:

```sh
python3 -m pip install --user "platformio==6.2.0"
```

If the shell cannot find `pio`, add this to `~/.zshrc` and open a new terminal:

```sh
export PATH="$HOME/Library/Python/3.12/bin:$PATH"
```

The `3.12` folder matches the Python you installed. `python3 -m platformio` always works even when `pio` is not on `PATH`.

4. Plug in the board and list the call-out port. Use a name that starts with `/dev/cu.`, not `/dev/tty.`:

```sh
ls /dev/cu.wchusbserial* /dev/cu.usbserial*
```

macOS often loads a CH340 driver by itself. If nothing appears, install the CH34x driver package published by WCH for macOS, unplug the board, and plug it in again.

## Install on Linux

These commands are for Debian, Ubuntu, and close relatives. On Fedora, install the same programs with `dnf` (`python3`, `python3-pip`, `git`, and a C compiler if you later build any helper).

```sh
sudo apt update
sudo apt install -y python3 python3-pip python3-venv git
python3 -m venv "$HOME/.platformio-venv"
"$HOME/.platformio-venv/bin/pip" install "platformio==6.2.0"
echo 'export PATH="$HOME/.platformio-venv/bin:$PATH"' >> "$HOME/.bashrc"
source "$HOME/.bashrc"
pio --version
```

`pio --version` should print `PlatformIO Core, version 6.2.0`.

Let your user open serial ports, then **log out and back in** (a new terminal is not enough):

```sh
sudo usermod -aG dialout "$USER"
```

The CH340 driver is the `ch341` module, which is already in the kernel of current Ubuntu and Debian. After you plug in the board:

```sh
ls /dev/ttyUSB*
```

You should see `/dev/ttyUSB0`, or a higher number if another USB serial device is already connected.

## Get the code

With Git:

```sh
git clone https://github.com/Neotech3000/invaders-machina.git
cd invaders-machina
git checkout feat/machina-shop
```

Without Git: on GitHub, switch the branch menu to **feat/machina-shop**, then **Code → Download ZIP**. Unzip it and open a terminal in that folder.

## Build the firmware

From the `firmware` folder:

```sh
cd firmware
pio run -e cyd
```

The first run downloads the toolchain, Arduino-ESP32 2.0.17, LovyanGFX. That takes several minutes and needs a network. Later builds are shorter. A good build ends with a line like:

```text
========================= [SUCCESS] Took ... =========================
```

## Flash the board

1. Plug the display in with a data cable.
2. Close any serial monitor that already has the port open.
3. From `firmware`:

```sh
pio run -e cyd -t upload
```

PlatformIO picks the port when the display is the only serial device. When the computer has another serial device, copy the example and set both lines to your port:

```sh
cp platformio.local.ini.example platformio.local.ini
```

Edit `platformio.local.ini` and uncomment the `[env:cyd]` block. Set `upload_port` and `monitor_port` to the name you found above (`COM5`, `/dev/cu.wchusbserial110`, or `/dev/ttyUSB0`). That file is git-ignored. Then run the upload command again.

You can also pass the port once, without a file:

```sh
pio run -e cyd -t upload --upload-port COM5
```

The CH340 resets the ESP32 into the bootloader by itself. You do not need to hold a button when that works. If the upload stops with **Failed to connect to ESP32**:

1. Hold **BOOT**.
2. Press and release **RESET**.
3. Release **BOOT**.
4. Run the upload command again right away.

A finished upload prints **Hash of data verified** and then resets the board.

## Check that it worked

- The backlight comes on.
- The panel plays an attract demo on its own. Tap **PLAY** to open the menu.
- The menu shows the cards listed under **How to play** below.

To watch the serial log, from `firmware`:

```sh
pio device monitor -e cyd
```

The speed is **115200**. Leave with **Ctrl+C**. If you set a port in `platformio.local.ini`, the monitor uses that port. Otherwise add `--port` and your port name.

## How to play

- Hearts in the top right are lives. You start with 3. Losing a life makes the ship blink and the level stays as it was.
- Enemies drop coins. Flying through a coin collects it. Coins are saved on the panel.
- Enemies are standard aliens, Assault troopers, and Cannon units. A cannon shell bursts on impact. On this panel that blast is about 3 mm across.
- Every 5th level is a boss. Beating a normal boss can start a raid (1 chance in 5), except on a god level. After the first raid boss is beaten, other levels have a 40% chance of a mini boss.
- Every 25 levels a god appears: Zeus, Hera, Poseidon, Demeter, Athena, Ares, Apollo, Artemis, Aphrodite, Hephaestus, Hermes, then Dionysus, and then the cycle repeats. Four waves of goons follow, each led by a mini boss, and then the god. The god has three health bars and changes fighting style as each bar empties.
- A failed run shows **YOU DIED** and the name of what hit you.
- **PLAY** continues from your current level.
- **CHECKPOINTS** jumps back to a checkpoint you have reached. A checkpoint is saved to flash every 20 levels.
- **CUSTOM** changes the ship, the gun, and the laser colour. Forged guns are listed here.
- **SHOP** spends coins. The tabs are **THEME**, **SHIPS**, **COLOR**, **GUNS**, **POWER**, and **UPGRD**. Ships are Striker, Intercept, Valkyrie, Phantom, Titan, and Seraph. Themes add Midnight, Mint, Rose, and Arcade alongside Dark Neon, Light, Blue, and Amber.
- **HOME SCREEN** returns to the CPU attract demo.
- **FORGE** combines two guns into a new gun with its own level.
- The theme card on the menu switches the theme you own.
- **RESTART GAME** at the bottom asks "Are you sure?" before it clears progress.


## Layout

- `firmware/src/hardware/board.cpp` — attract demo, menu, levels, gods, shop, forge, and HUD.
- `firmware/src/hardware/hardware_config.h` — panel pins.
- `firmware/src/hardware/touch_map.h` — touch mapping. Leave it unless you remeasure the glass.
- `firmware/platformio.ini` — the build. Ports stay out of this file.
- `firmware/platformio.local.ini.example` — copy to `platformio.local.ini` to pin a flash port.

## Troubleshooting

**`pio` is not found.** Use `python3 -m platformio` or `py -m platformio`, and check that the PlatformIO folder is on `PATH`. Open a new terminal after installing.

**`externally-managed-environment` on Linux.** Use the virtualenv in the Linux install section. Do not install PlatformIO into the system Python with `sudo pip`.

**The first build fails on `espressif32` 7 or 8.** `firmware/platformio.ini` must say `platform = espressif32@6.13.0`. Delete `firmware/.pio` and run `pio run -e cyd` again if an older attempt downloaded the wrong platform.

**No serial port appears.** Try another cable (it must carry data), another USB socket, and the CH340 driver. On Linux, run `lsusb` and look for `1a86:7523` or `1a86:5523`. Then run `ls /dev/ttyUSB*`.

**Permission denied on `/dev/ttyUSB0`.** You are not in the `dialout` group yet, or you have not logged out since joining it. `groups` should include `dialout`.

**Failed to connect / wrong chip.** Hold BOOT, tap RESET, release BOOT, and upload again. Confirm Device Manager or `ls` still shows the same port. Close the serial monitor first. A failed upload that mentions a timeout is almost always the driver, the cable, or the buttons, not the game code.

**Upload writes to the wrong device.** Unplug every other USB serial device, or set `upload_port` in `firmware/platformio.local.ini`.

**The picture is snow, shifted, or the wrong size.** The board is not the 3.5 inch ST7796 module this project is written for.

**Touch hits the wrong key.** Do not edit `touch_map.h` to guess. The mapping is for this panel's resistive controller. Press inside the drawn rectangle.

