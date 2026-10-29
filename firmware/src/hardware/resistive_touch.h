#pragma once

#include <stdint.h>

// One raw XPT2046 conversion. pressure is high while a finger is down.
struct TouchRaw {
  int x = 0;
  int y = 0;
  int z1 = 0;
  int z2 = 0;
  int pressure = 0;
};

enum class TouchWire : uint8_t { None = 0, Shared = 1, Split = 2 };

// Samples the shared-SPI and separate-SPI wirings and keeps the live one.
void touch_begin();

TouchWire touch_wire();
const char* touch_wire_name();

// Fills raw and screen coordinates. True only while a finger is down.
bool touch_read(TouchRaw& raw, int& sx, int& sy);

// Plain-text samples for the first few seconds after boot, then silence.
void touch_log(const TouchRaw& raw, int sx, int sy, const char* button, bool on_button);
