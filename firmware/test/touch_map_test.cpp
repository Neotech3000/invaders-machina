#include "../src/hardware/touch_map.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;

static void expect(bool ok, const char* name) {
  if (ok) {
    return;
  }
  std::fprintf(stderr, "FAIL %s\n", name);
  failures += 1;
}

int main() {
  const int width = 320;
  const int height = 480;
  const int bar = height - 72;

  int sx = 0;
  int sy = 0;
  // Raw Y at the top of the glass. The vertical flip of this point sits in the
  // bottom bar; that flip must not win.
  touch_to_screen(2000, 200, width, height, sx, sy);
  expect(sy >= 0 && sy < bar, "top tap stays above the bar");
  expect((height - 1) - sy >= bar, "flipped top would have been the bar");

  touch_to_screen(2000, 3900, width, height, sx, sy);
  expect(sy >= bar && sy < height, "bottom tap hits the bar");

  // High raw X is the left of the glass once the axis is unmirrored.
  touch_to_screen(3900, 3900, width, height, sx, sy);
  expect(sx >= 0 && sx < width / 3, "left of the glass is the left third");
  expect(touch_bar_slot(sx, width) == 0, "left third is slot 0");

  touch_to_screen(200, 3900, width, height, sx, sy);
  expect(sx >= (2 * width) / 3, "right of the glass is the right third");
  expect(touch_bar_slot(sx, width) == 2, "right third is slot 2");

  touch_to_screen(2050, 3900, width, height, sx, sy);
  expect(touch_bar_slot(sx, width) == 1, "middle of the glass is slot 1");

  int gap_x = 0;
  int gap_w = 0;
  touch_bar_rect(0, width, gap_x, gap_w);
  expect(touch_bar_slot(gap_x + gap_w, width) == -1, "gap between buttons is not a hit");
  expect(touch_bar_slot(gap_x + gap_w - 1, width) == 0, "last pixel of the left button hits");

  int right_x = 0;
  int right_w = 0;
  touch_bar_rect(2, width, right_x, right_w);
  expect(right_x + right_w == width, "right button reaches the edge");
  expect(touch_bar_slot(width - 1, width) == 2, "right edge is slot 2");

  if (failures != 0) {
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
  }
  std::printf("touch map ok\n");
  return 0;
}
