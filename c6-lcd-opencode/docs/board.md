# ESP32-C6-LCD-1.47

Waveshare non-touch board. The module is an LBS147TC-IF15, 172 by 320,
ST7789V3, 4-wire SPI. One mechanical drawing also prints a GC9307N mark.
Waveshare's demos use the ST7789 sequence. The controller keeps its own
pixel memory, so an old picture survives reset until a write arrives.

| Signal | GPIO | Notes |
| --- | --- | --- |
| LCD data, MOSI | 6 | Shared with microSD. Wakes as JTAG TCK. |
| LCD clock, SCLK | 7 | Shared with microSD. Wakes as JTAG TDO. |
| LCD chip select | 14 | Active low. |
| LCD data/command | 15 | |
| LCD reset | 21 | Active low. Not the chip reset. |
| Backlight | 22 | MOSFET, active high. Stay at or below 50%. |
| SD MISO | 5 | |
| SD chip select | 4 | Hold high while the LCD owns the bus. |
| RGB LED | 8 | One WS2812. Not the display. |
| USB D- / D+ | 12 / 13 | Programming port. Do not reuse. |

The column offset is 34 pixels. The chip's dedicated SPI function is the
other way around from the board: function 2 makes GPIO6 the clock and GPIO7
the data. Route SPI through the GPIO matrix, with data on 6 and clock on 7.
Writing the GPIO register does nothing until those pads are switched from
the JTAG function to GPIO.

Waveshare's schematic labels BOOT on GPIO9. The owner's prompt says BOOT is
GPIO0. Trust the target file in the firmware checkout when those two
disagree, and do not retarget a working board on the prompt alone.

The RGB LED moving only proves the sketch runs. It does not prove the LCD
received a clock.

USB identity for this panel is vendor `303a`, product `1001`. There is no
USB-UART bridge on the schematic.
