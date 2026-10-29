#pragma once

#include <cstdint>

// Panel values confirmed after the image that drew "idle" on this board.
// Chip: ESP32-D0WD-V3, 4MB flash, no PSRAM. Driver: ST7796 on SPI2.
// Do not replace this pinout.
//
// PlatformIO board id remains esp32dev (ESP32 Dev Module).

static constexpr int kPanelWidth = 320;
static constexpr int kPanelHeight = 480;

static constexpr int kPinSclk = 14;
static constexpr int kPinMosi = 13;
static constexpr int kPinMiso = 12;
static constexpr int kPinDc = 2;
static constexpr int kPinCs = 15;
static constexpr int kPinRst = -1;  // reset tied to EN
static constexpr int kPinBl = 27;
static constexpr bool kBacklightActiveLow = false;  // active high
static constexpr int kPanelRotation = 0;
static constexpr bool kPanelInvert = false;
static constexpr bool kPanelRgbOrder = false;  // BGR, as in the image that drew idle
static constexpr int kSpiHostIsSpi2 = 1;
static constexpr uint32_t kSpiWriteHz = 20000000;

// This board's XPT2046 shares the panel SPI bus: CLK 14, MOSI 13, MISO 12,
// CS 33. IRQ is GPIO 36 and is only reported. A separate bus on CLK 25,
// MOSI 32, MISO 39 was measured and stays at zero, so it is not kept.
static constexpr int kPinTouchCs = 33;
static constexpr int kPinTouchIrq = 36;
static constexpr int kPinTouchSplitClk = 25;
static constexpr int kPinTouchSplitMosi = 32;
static constexpr int kPinTouchSplitMiso = 39;
static constexpr uint32_t kTouchHz = 1000000;

// Serial monitor baud. UNCONFIRMED placeholder. Not a measured link baud.
static constexpr uint32_t kMonitorBaudPlaceholder = 115200;
