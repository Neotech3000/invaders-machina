#pragma once

// One screen mapping for the XPT2046. The raw X axis on this panel is mirrored
// relative to the labels, so it is flipped here and every button uses the same
// point. Y is not flipped. Callers must not also test the vertically flipped
// point: that is what turned a tap at the top into a bottom-bar press.

static constexpr bool kTouchFlipX = true;
static constexpr bool kTouchFlipY = false;

inline int touch_clamp_scale(int raw, int size) {
  if (size <= 1) {
    return 0;
  }
  if (raw < 200) raw = 200;
  if (raw > 3900) raw = 3900;
  return (raw - 200) * (size - 1) / (3900 - 200);
}

inline void touch_to_screen(int raw_x, int raw_y, int width, int height, int& sx, int& sy) {
  int x = touch_clamp_scale(raw_x, width);
  int y = touch_clamp_scale(raw_y, height);
  if (kTouchFlipX) x = (width - 1) - x;
  if (kTouchFlipY) y = (height - 1) - y;
  sx = x;
  sy = y;
}

// The drawn slot is slot_w wide, with a 4px gap except on the last slot, which
// runs to the right edge. The gap is not a button.
inline void touch_bar_rect(int index, int width, int& x, int& w) {
  const int slot_w = width / 3;
  x = index * slot_w;
  w = (index == 2) ? width - x : slot_w - 4;
}

inline int touch_bar_slot(int x, int width) {
  if (x < 0 || x >= width) {
    return -1;
  }
  for (int i = 0; i < 3; ++i) {
    int sx = 0;
    int sw = 0;
    touch_bar_rect(i, width, sx, sw);
    if (x >= sx && x < sx + sw) {
      return i;
    }
  }
  return -1;
}
