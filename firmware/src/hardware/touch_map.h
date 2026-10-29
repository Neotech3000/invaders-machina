#pragma once

// One screen mapping for this panel's XPT2046.
// Both axes are mirrored. A finger on the bottom PLAY key reads a low raw Y
// (the boot sample was raw y=585), and the left of the glass is the left of
// the picture. A second orientation is not tried, so a point outside a drawn
// rectangle cannot be pulled onto a button.

static constexpr int kTouchRawLo = 280;
static constexpr int kTouchRawHi = 3860;
static constexpr bool kTouchFlipX = true;
static constexpr bool kTouchFlipY = true;

inline int touch_clamp_scale(int raw, int size) {
  if (size <= 1) {
    return 0;
  }
  if (raw < kTouchRawLo) raw = kTouchRawLo;
  if (raw > kTouchRawHi) raw = kTouchRawHi;
  return (raw - kTouchRawLo) * (size - 1) / (kTouchRawHi - kTouchRawLo);
}

inline void touch_to_screen(int raw_x, int raw_y, int width, int height, int& sx, int& sy) {
  int x = touch_clamp_scale(raw_x, width);
  int y = touch_clamp_scale(raw_y, height);
  if (kTouchFlipX) x = (width - 1) - x;
  if (kTouchFlipY) y = (height - 1) - y;
  sx = x;
  sy = y;
}
