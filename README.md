# Jitters

Jitters is a small cat that lives on a 3.5 inch ESP32 display. The cat walks on the glass. Tap **PLAY** to open a home screen. From there you can play **Invaders Machina** (300 levels), **Tetris**, and **Snake**, open **Care**, switch a colour theme, or change the ship and guns. A desktop window on the same computer can take the cat over the USB cable and send it back.

The calendar card shows the date the firmware was built and the words "calendar not connected".

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
| ArduinoJson | 7.2.x | the USB messages |
| Node.js | 22 LTS (22.x) | the desktop window only |

Do not change `platform = espressif32@6.13.0` to a bare `espressif32`. In late 2026 that name installs platform 8.x, which uses a newer Arduino core this project does not build with.

## Install on Windows

1. Install **Python 3.12** from [python.org](https://www.python.org/downloads/windows/). On the first installer page, tick **Add python.exe to PATH**.
2. Install **Node.js 22 LTS** from [nodejs.org](https://nodejs.org/) if you want the desktop cat. Skip this if you only want the games on the panel.
3. Open **PowerShell** and run:

```powershell
py -m pip install --upgrade pip
py -m pip install "platformio==6.2.0"
py -m platformio --version
```

If `pio` is not recognized later, use `py -m platformio` in place of `pio` in every command below.

4. Plug the display into a USB port on the computer itself, not into an unpowered hub.
5. Open **Device Manager** and expand **Ports (COM & LPT)**. You want a line like **USB-SERIAL CH340 (COM5)**. The number at the end is your port.
6. If the device is missing, or it shows a yellow mark under **Other devices**, install the CH340/CH341 driver from the chip maker WCH (search the WCH site for `CH341SER`). Unplug the board, install the driver, plug it back in, and read the COM number again.

## Install on macOS

1. Install Python 3.12 if `python3 --version` is not 3.11 or 3.12. The installer from [python.org](https://www.python.org/downloads/macos/) is enough.
2. Install Node.js 22 LTS from [nodejs.org](https://nodejs.org/) if you want the desktop cat.
3. Install the command-line tools once, so Node can build the serial addon if it has no prebuilt copy:

```sh
xcode-select --install
```

4. Install PlatformIO for your user:

```sh
python3 -m pip install --user "platformio==6.2.0"
```

If the shell cannot find `pio`, add this to `~/.zshrc` and open a new terminal:

```sh
export PATH="$HOME/Library/Python/3.12/bin:$PATH"
```

The `3.12` folder matches the Python you installed. `python3 -m platformio` always works even when `pio` is not on `PATH`.

5. Plug in the board and list the call-out port. Use a name that starts with `/dev/cu.`, not `/dev/tty.`:

```sh
ls /dev/cu.wchusbserial* /dev/cu.usbserial*
```

macOS often loads a CH340 driver by itself. If nothing appears, install the CH34x driver package published by WCH for macOS, unplug the board, and plug it in again.

## Install on Linux

These commands are for Debian, Ubuntu, and close relatives. On Fedora, install the same programs with `dnf` (`python3`, `python3-pip`, `git`, and, for the desktop app, `libudev-devel` plus a C compiler).

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

For the desktop cat, also install Node.js 22 and the serial-port build packages:

```sh
sudo apt install -y build-essential python3 libudev-dev
```

Install Node.js 22 from [nodejs.org](https://nodejs.org/) or with NodeSource. `node -v` should print `v22.` something.

## Get the code

With Git:

```sh
git clone https://github.com/Neotech3000/invaders-machina.git
cd invaders-machina
git checkout master
```

Without Git: on GitHub, switch the branch menu to **master**, then **Code → Download ZIP**. Unzip it and open a terminal in that folder.

## Build the firmware

From the `firmware` folder:

```sh
cd firmware
pio run -e cyd
```

The first run downloads the toolchain, Arduino-ESP32 2.0.17, LovyanGFX, and ArduinoJson. That takes several minutes and needs a network. Later builds are shorter. A good build ends with a line like:

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
- A cat walks on the screen. **PLAY** is the bottom key.
- Tap **PLAY**. The home screen title is **JITTERS**, with cards for Invaders Machina, Tetris, Snake, Calendar, a theme, Care, and Custom.

To watch the serial log, from `firmware`:

```sh
pio device monitor -e cyd
```

The speed is **115200**. Leave with **Ctrl+C**. A healthy boot prints a `FRAME walk_ms=` line. If you set a port in `platformio.local.ini`, the monitor uses that port. Otherwise add `--port` and your port name.

## Desktop cat

This part is optional. The games on the panel do not need it.

From the `desktop` folder:

```sh
cd desktop
npm install
npm run build
npm test
npm start
```

`npm test` should print that the handoff tests passed and `ports ok`.

`npm start` opens a port picker.

- **Simulator** talks to a fake panel. Use it when the board is not plugged in.
- A real port is listed with the name the operating system gave it. The app connects by itself when it can see exactly one CH340, or exactly one serial port of any kind. If several ports are present, click the display's port and press the connect button.
- To force a port, copy `port.local.json.example` to `port.local.json` (git-ignored) and set `port` to the same name you used for flashing. `REPLACE_WITH_YOUR_PORT` means "do not force one".

Bring the cat to the laptop, click it to pet it, hide the window, or send it back. The panel says when the cat is leaving and when it is on the computer. Disconnect stays disconnected until that port disappears.

On Linux the window uses X11 so the cat can be transparent. On a Wayland-only session the window can come up as a black box. Sign in with an X11 or XWayland session (Ubuntu's default) and start the app again.

## What the panel does

- The cat picks a direction, walks, sometimes pauses, and moves toward a tap on empty glass. That tap does not open a game. One or two other cats sometimes cross the screen.
- **PLAY** opens home. **WALK** on the home screen returns to the cat.
- **INVADERS MACHINA** is 300 levels. You start with 3 lives. Every 5th level is a boss. Beating a normal boss can start a raid (1 chance in 5). Bosses include shield, shut, spray, laser, and drone patterns. Custom has nine guns and twelve laser colours. Guns level up while you play.
- **TETRIS** and **SNAKE** are on their own cards. Tetris shows a score.
- **CARE** has **FEED** (fills hunger) and **CARE** (lifts mood). Both fall while the cat is on the panel.
- Themes are **DARK NEON**, **LIGHT**, **BLUE**, and **AMBER**. The theme, the equipped gun, and the laser colour are stored on the panel and kept across reboot.
- While the cat is on the laptop the panel says so. While the handoff starts it says the cat is heading to the laptop.

## Layout

- `firmware/src/hardware/board.cpp` — cat, home screen, and games.
- `firmware/src/hardware/hardware_config.h` — panel pins.
- `firmware/src/hardware/touch_map.h` — touch mapping. Leave it unless you remeasure the glass.
- `firmware/platformio.ini` — the build. Ports stay out of this file.
- `firmware/platformio.local.ini.example` — copy to `platformio.local.ini` to pin a flash port.
- `firmware/port.example` — copy to `port.local` for `firmware/scripts/handoff_once.py`.
- `desktop/` — the laptop window.
- `desktop/port.local.json.example` — copy to `port.local.json` to pin the desktop port.

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

**`npm install` fails while building `serialport`.** Install Node.js 22, and on Linux also `build-essential` and `libudev-dev`. Delete `desktop/node_modules` and run `npm install` again.

**The desktop window is a black square on Linux.** Start the session under X11 or XWayland. The app forces the X11 window path on Linux and does not do that on Windows or macOS.

**The cat never auto-connects.** More than one serial port is plugged in, and none of them is a single CH340. Choose the port in the picker, or set it in `desktop/port.local.json`.
