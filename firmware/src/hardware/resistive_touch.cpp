#include "resistive_touch.h"

#include "hardware_config.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>

#include <stdio.h>
#include <string.h>

namespace {

constexpr int kSamples = 5;
constexpr uint32_t kLogWindowMs = 8000;
constexpr int kPress = 400;

TouchWire wire_ = TouchWire::None;
uint32_t boot_ms_ = 0;
uint32_t logged_at_ = 0;
bool select_logged_ = false;

struct Stats {
  TouchRaw last;
  int spread = 10000;
  int analog = 0;
  uint8_t bytes[8] = {};
};

bool in_range(int v) { return v > 100 && v < 4000; }

int pressure_of(int z1, int z2) {
  int p = z1 + 4095 - z2;
  if (p < 0) p = 0;
  if (p > 4095) p = 4095;
  return p;
}

bool analog_sample(const TouchRaw& s) {
  return in_range(s.x) && in_range(s.y) && s.z1 < 4000 && s.z2 > 80;
}

void parse_burst(const uint8_t* data, TouchRaw& out) {
  const uint8_t* d = data + 24;
  out.y = ((d[1] << 8) | d[2]) >> 3;
  out.z1 = ((d[3] << 8) | d[4]) >> 3;
  out.x = ((d[5] << 8) | d[6]) >> 3;
  out.z2 = ((d[7] << 8) | d[8]) >> 3;
  out.x &= 0x0fff;
  out.y &= 0x0fff;
  out.z1 &= 0x0fff;
  out.z2 &= 0x0fff;
  out.pressure = pressure_of(out.z1, out.z2);
}

TouchRaw read_shared() {
  uint8_t data[57];
  memset(data, 0, 8);
  data[0] = 0x91;
  data[2] = 0xB1;
  data[4] = 0xD1;
  data[6] = 0xC1;
  data[56] = 0x80;
  memcpy(data + 8, data, 8);
  memcpy(data + 16, data, 16);
  memcpy(data + 32, data, 24);

  digitalWrite(kPinCs, HIGH);
  digitalWrite(kPinTouchCs, HIGH);
  lgfx::spi::beginTransaction(SPI2_HOST, kTouchHz, 0);
  digitalWrite(kPinTouchCs, LOW);
  lgfx::spi::readBytes(SPI2_HOST, data, 57);
  digitalWrite(kPinTouchCs, HIGH);
  lgfx::spi::endTransaction(SPI2_HOST);

  TouchRaw out;
  parse_burst(data, out);
  return out;
}

uint8_t split_shift(uint8_t out) {
  uint8_t in = 0;
  for (int bit = 0; bit < 8; ++bit) {
    digitalWrite(kPinTouchSplitMosi, (out & 0x80) ? HIGH : LOW);
    out = static_cast<uint8_t>(out << 1);
    delayMicroseconds(2);
    digitalWrite(kPinTouchSplitClk, HIGH);
    delayMicroseconds(2);
    digitalWrite(kPinTouchSplitClk, LOW);
    delayMicroseconds(2);
    in = static_cast<uint8_t>((in << 1) | (digitalRead(kPinTouchSplitMiso) ? 1 : 0));
  }
  return in;
}

uint16_t split_axis(uint8_t cmd) {
  digitalWrite(kPinTouchCs, LOW);
  delayMicroseconds(2);
  split_shift(cmd);
  const uint16_t hi = split_shift(0);
  const uint16_t lo = split_shift(0);
  digitalWrite(kPinTouchCs, HIGH);
  return static_cast<uint16_t>(((hi << 8) | lo) >> 3) & 0x0fff;
}

TouchRaw read_split() {
  TouchRaw out;
  out.y = split_axis(0x91);
  out.z1 = split_axis(0xB1);
  out.x = split_axis(0xD1);
  out.z2 = split_axis(0xC1);
  out.pressure = pressure_of(out.z1, out.z2);
  return out;
}

TouchRaw read_wire(TouchWire wire) {
  if (wire == TouchWire::Split) return read_split();
  return read_shared();
}

Stats measure(TouchWire wire) {
  Stats stats;
  int min_x = 4096;
  int max_x = 0;
  int min_y = 4096;
  int max_y = 0;
  for (int i = 0; i < kSamples; ++i) {
    const TouchRaw sample = read_wire(wire);
    stats.last = sample;
    if (analog_sample(sample)) stats.analog += 1;
    if (sample.x < min_x) min_x = sample.x;
    if (sample.x > max_x) max_x = sample.x;
    if (sample.y < min_y) min_y = sample.y;
    if (sample.y > max_y) max_y = sample.y;
    delay(2);
  }
  stats.spread = (max_x - min_x) + (max_y - min_y);
  return stats;
}

bool usable(const Stats& stats) {
  return stats.analog >= 3 && stats.spread < 1200 && analog_sample(stats.last);
}

// An untouched XPT2046 holds Z1 low and Z2 near full scale. Open X/Y rails
// (0 or 4095) are that same idle chip, not a dead bus.
bool idle_controller(const Stats& stats) {
  return stats.spread < 400 && stats.last.z1 < 300 && stats.last.z2 > 3500;
}

int scale_axis(int raw, int size) {
  const int lo = 280;
  const int hi = 3860;
  if (raw < lo) raw = lo;
  if (raw > hi) raw = hi;
  return (raw - lo) * (size - 1) / (hi - lo);
}

// One mapping for the whole glass. X is mirrored so the left of the panel is
// the left of the picture. Y grows downward. A second orientation is not tried.
void map_screen(const TouchRaw& raw, int& sx, int& sy) {
  sx = (kPanelWidth - 1) - scale_axis(raw.x, kPanelWidth);
  sy = scale_axis(raw.y, kPanelHeight);
}

}  // namespace

void touch_begin() {
  pinMode(kPinTouchCs, OUTPUT);
  digitalWrite(kPinTouchCs, HIGH);
  pinMode(kPinTouchIrq, INPUT);
  pinMode(kPinTouchSplitClk, OUTPUT);
  pinMode(kPinTouchSplitMosi, OUTPUT);
  pinMode(kPinTouchSplitMiso, INPUT);
  digitalWrite(kPinTouchSplitClk, LOW);
  digitalWrite(kPinTouchSplitMosi, LOW);

  const Stats shared = measure(TouchWire::Shared);
  const Stats split = measure(TouchWire::Split);
  const bool shared_ok = usable(shared);
  const bool split_ok = usable(split);
  const bool shared_idle = idle_controller(shared);
  const bool split_idle = idle_controller(split);
  if (shared_ok && split_ok) {
    wire_ = shared.spread <= split.spread ? TouchWire::Shared : TouchWire::Split;
  } else if (shared_ok) {
    wire_ = TouchWire::Shared;
  } else if (split_ok) {
    wire_ = TouchWire::Split;
  } else if (shared_idle) {
    wire_ = TouchWire::Shared;
  } else if (split_idle) {
    wire_ = TouchWire::Split;
  } else if (shared.analog > split.analog && shared.analog > 0) {
    wire_ = TouchWire::Shared;
  } else if (split.analog > 0) {
    wire_ = TouchWire::Split;
  } else {
    wire_ = TouchWire::None;
  }
  boot_ms_ = millis();
  const int irq = digitalRead(kPinTouchIrq);
  Serial.printf("TOUCH shared x=%d y=%d z1=%d z2=%d spread=%d analog=%d idle=%d\n", shared.last.x,
                shared.last.y, shared.last.z1, shared.last.z2, shared.spread, shared.analog,
                shared_idle ? 1 : 0);
  Serial.printf("TOUCH split x=%d y=%d z1=%d z2=%d spread=%d analog=%d idle=%d\n", split.last.x,
                split.last.y, split.last.z1, split.last.z2, split.spread, split.analog,
                split_idle ? 1 : 0);
  Serial.printf("TOUCH select bus=%s irq=%d mirrorx=1\n", touch_wire_name(), irq);
  Serial.flush();
}

TouchWire touch_wire() { return wire_; }

const char* touch_wire_name() {
  if (wire_ == TouchWire::Shared) return "shared";
  if (wire_ == TouchWire::Split) return "split";
  return "none";
}

bool touch_hits(const TouchRaw& raw, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) {
    return false;
  }
  int sx = 0;
  int sy = 0;
  map_screen(raw, sx, sy);
  return sx >= x && sy >= y && sx < x + w && sy < y + h;
}

bool touch_read(TouchRaw& raw, int& sx, int& sy) {
  if (wire_ == TouchWire::None) {
    raw = {};
    sx = 0;
    sy = 0;
    return false;
  }
  raw = read_wire(wire_);
  map_screen(raw, sx, sy);
  return analog_sample(raw) && raw.pressure > kPress;
}

void touch_log(const TouchRaw& raw, int sx, int sy, const char* button, bool on_button) {
  if (boot_ms_ == 0 || millis() - boot_ms_ > kLogWindowMs) {
    return;
  }
  const uint32_t now = millis();
  if (!select_logged_) {
    select_logged_ = true;
    Serial.printf("TOUCH select bus=%s\n", touch_wire_name());
  }
  if (!on_button && now - logged_at_ < 400) {
    return;
  }
  logged_at_ = now;
  const char* name = (button != nullptr && button[0] != '\0') ? button : "none";
  if (on_button) {
    Serial.printf("TOUCH tap control=%s sx=%d sy=%d raw=%d,%d z1=%d z2=%d bus=%s\n", name, sx, sy,
                  raw.x, raw.y, raw.z1, raw.z2, touch_wire_name());
  } else {
    Serial.printf("TOUCH sample bus=%s sx=%d sy=%d raw=%d,%d z1=%d z2=%d control=%s\n",
                  touch_wire_name(), sx, sy, raw.x, raw.y, raw.z1, raw.z2, name);
  }
}
