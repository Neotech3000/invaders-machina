#include "aether.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char* message) {
  fprintf(stderr, "fail: %s\n", message);
  return 1;
}

static void render_frame(uint32_t time_ms, AetherRgb* frame) {
  for (int y = 0; y < kAetherHeight; y += 16) {
    int rows = kAetherHeight - y;
    if (rows > 16) {
      rows = 16;
    }
    aether_render(time_ms, y, rows, frame + y * kAetherWidth);
  }
}

static void write_ppm(const char* path, const AetherRgb* frame) {
  FILE* file = fopen(path, "wb");
  if (!file) {
    perror(path);
    return;
  }
  fprintf(file, "P6\n%d %d\n255\n", kAetherWidth, kAetherHeight);
  for (int i = 0; i < kAetherWidth * kAetherHeight; ++i) {
    fputc(frame[i].r, file);
    fputc(frame[i].g, file);
    fputc(frame[i].b, file);
  }
  fclose(file);
}

static int differing(const AetherRgb* a, const AetherRgb* b) {
  int count = 0;
  int pixels = kAetherWidth * kAetherHeight;
  for (int i = 0; i < pixels; ++i) {
    if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) {
      ++count;
    }
  }
  return count;
}

static int mean_channel(const AetherRgb* frame, int channel) {
  long sum = 0;
  int pixels = kAetherWidth * kAetherHeight;
  for (int i = 0; i < pixels; ++i) {
    uint8_t value = channel == 0 ? frame[i].r : channel == 1 ? frame[i].g : frame[i].b;
    sum += value;
  }
  return (int)(sum / pixels);
}

int main(int argc, char** argv) {
  const char* directory = argc > 1 ? argv[1] : 0;
  AetherRgb* warp = (AetherRgb*)malloc(sizeof(AetherRgb) * kAetherWidth * kAetherHeight);
  AetherRgb* later = (AetherRgb*)malloc(sizeof(AetherRgb) * kAetherWidth * kAetherHeight);
  AetherRgb* core = (AetherRgb*)malloc(sizeof(AetherRgb) * kAetherWidth * kAetherHeight);
  AetherRgb* veil = (AetherRgb*)malloc(sizeof(AetherRgb) * kAetherWidth * kAetherHeight);
  if (!warp || !later || !core || !veil) {
    return fail("out of memory");
  }

  render_frame(1200, warp);
  render_frame(1800, later);
  render_frame(16000, core);
  render_frame(30000, veil);

  if (aether_scene(1200) != 0 || aether_scene(16000) != 1 || aether_scene(30000) != 2) {
    return fail("scene clock");
  }
  if (differing(warp, later) < 400) {
    return fail("warp frame did not move");
  }
  if (differing(warp, core) < 1000 || differing(core, veil) < 1000) {
    return fail("scenes look the same");
  }

  AetherRgb sun = warp[170 * kAetherWidth + 86];
  if (sun.r < 180 || sun.r < sun.b + 40) {
    return fail("sun is not warm");
  }
  AetherRgb heart = core[156 * kAetherWidth + 86];
  if (heart.r < 200 || heart.g < 150) {
    return fail("core is dim");
  }
  if (mean_channel(warp, 0) < 20 || mean_channel(veil, 2) < 15) {
    return fail("frame is too dark");
  }

  // Title sits in the sky during the opening.
  int bright = 0;
  for (int y = 50; y < 110; ++y) {
    for (int x = 20; x < 150; ++x) {
      AetherRgb pixel = warp[y * kAetherWidth + x];
      if (pixel.r > 220 && pixel.g > 200) {
        ++bright;
      }
    }
  }
  if (bright < 40) {
    return fail("title did not draw");
  }

  AetherLamp warm = aether_lamp(1200);
  AetherLamp hot = aether_lamp(16000);
  AetherLamp cool = aether_lamp(30000);
  if (!(warm.r > warm.b && hot.r > hot.b && cool.g > cool.r)) {
    return fail("lamp colors");
  }

  if (directory) {
    char path[512];
    snprintf(path, sizeof(path), "%s/warp.ppm", directory);
    write_ppm(path, warp);
    snprintf(path, sizeof(path), "%s/warp-later.ppm", directory);
    write_ppm(path, later);
    snprintf(path, sizeof(path), "%s/core.ppm", directory);
    write_ppm(path, core);
    snprintf(path, sizeof(path), "%s/veil.ppm", directory);
    write_ppm(path, veil);
    render_frame(13600, later);
    snprintf(path, sizeof(path), "%s/wipe.ppm", directory);
    write_ppm(path, later);
    render_frame(6000, later);
    snprintf(path, sizeof(path), "%s/warp-cruise.ppm", directory);
    write_ppm(path, later);
  }

  free(warp);
  free(later);
  free(core);
  free(veil);
  printf("aether render ok\n");
  return 0;
}
