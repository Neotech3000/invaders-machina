#include "../src/hardware/touch_map.h"

#include <cstdio>

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

  // Low raw Y is the bottom PLAY key on this panel. The unflipped scale of
  // that sample sits at the top; the mirror is what puts it on the key.
  touch_to_screen(2000, 585, width, height, sx, sy);
  expect(sy >= bar && sy < height, "low raw Y lands on the bottom");
  const int unflipped = touch_clamp_scale(585, height);
  expect(unflipped < bar, "without the Y mirror that sample would be the top");

  touch_to_screen(2000, 3860, width, height, sx, sy);
  expect(sy >= 0 && sy < bar, "high raw Y stays above the bar");

  // High raw X is the left of the glass once X is unmirrored.
  touch_to_screen(3860, 585, width, height, sx, sy);
  expect(sx >= 0 && sx < width / 3, "left of the glass is the left third");

  touch_to_screen(280, 585, width, height, sx, sy);
  expect(sx >= (2 * width) / 3, "right of the glass is the right third");

  // A point is one point. Its vertical flip is not also tested.
  touch_to_screen(2000, 2000, width, height, sx, sy);
  const int flipped = (height - 1) - sy;
  expect(sy != flipped, "the mapping does not return both a point and its flip");

  if (failures != 0) {
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
  }
  std::printf("touch map ok\n");
  return 0;
}
