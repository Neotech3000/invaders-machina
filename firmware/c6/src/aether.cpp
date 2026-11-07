#include "aether.h"

#include <math.h>

namespace {

constexpr int kSceneCount = 3;
constexpr int kWipeMs = 900;
constexpr int kHorizon = 198;
constexpr int kSunR = 46;

int16_t g_sin[256];
bool g_ready = false;

struct Star {
  int16_t x;
  int16_t y;
  uint16_t phase;
  uint8_t rate;
};

constexpr int kStarCount = 56;
Star g_stars[kStarCount];

int clamp255(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return value;
}

int floor_mod(int value, int modulo) {
  int remainder = value % modulo;
  if (remainder < 0) {
    remainder += modulo;
  }
  return remainder;
}

int isqrt(int value) {
  if (value <= 0) {
    return 0;
  }
  int current = value;
  int next = (current + 1) >> 1;
  while (next < current) {
    current = next;
    next = (current + value / current) >> 1;
  }
  return current;
}

void prepare() {
  if (g_ready) {
    return;
  }
  for (int i = 0; i < 256; ++i) {
    float angle = (float)i * 6.2831853f / 256.f;
    g_sin[i] = (int16_t)(sinf(angle) * 255.f);
  }
  uint32_t seed = 0xAEE7E7u;
  for (int i = 0; i < kStarCount; ++i) {
    seed = seed * 1664525u + 1013904223u;
    g_stars[i].x = (int16_t)((seed % 760) - 380);
    seed = seed * 1664525u + 1013904223u;
    g_stars[i].y = (int16_t)((seed % 760) - 380);
    seed = seed * 1664525u + 1013904223u;
    g_stars[i].phase = (uint16_t)(seed % 980);
    seed = seed * 1664525u + 1013904223u;
    g_stars[i].rate = (uint8_t)(12 + (seed % 18));
  }
  g_ready = true;
}

int sin256(int angle) { return g_sin[angle & 255]; }

int cos256(int angle) { return sin256(angle + 64); }

void scene_clock(uint32_t time_ms, int* scene, int* next_scene, int* wipe_y) {
  uint32_t local = time_ms % kAetherSceneMs;
  int index = (int)((time_ms / kAetherSceneMs) % kSceneCount);
  *scene = index;
  *next_scene = (index + 1) % kSceneCount;
  if (local >= kAetherSceneMs - kWipeMs) {
    uint32_t into = local - (kAetherSceneMs - kWipeMs);
    *wipe_y = (int)(into * kAetherHeight / kWipeMs);
  } else {
    *wipe_y = -1;
  }
}

AetherRgb rgb(int r, int g, int b) {
  AetherRgb color;
  color.r = (uint8_t)clamp255(r);
  color.g = (uint8_t)clamp255(g);
  color.b = (uint8_t)clamp255(b);
  return color;
}

void blend(AetherRgb* dst, AetherRgb src, int cover) {
  if (cover >= 255) {
    *dst = src;
    return;
  }
  if (cover <= 0) {
    return;
  }
  dst->r = (uint8_t)(dst->r + ((int)src.r - dst->r) * cover / 255);
  dst->g = (uint8_t)(dst->g + ((int)src.g - dst->g) * cover / 255);
  dst->b = (uint8_t)(dst->b + ((int)src.b - dst->b) * cover / 255);
}

struct Band {
  AetherRgb* pixels;
  int y0;
  int rows;
};

void plot(Band& band, int x, int y, AetherRgb color, int cover) {
  if ((unsigned)x >= (unsigned)kAetherWidth) {
    return;
  }
  if (y < band.y0 || y >= band.y0 + band.rows) {
    return;
  }
  blend(&band.pixels[(y - band.y0) * kAetherWidth + x], color, cover);
}

void plot_line(Band& band, int x0, int y0, int x1, int y1, AetherRgb color, int cover) {
  int dx = x1 - x0;
  int dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;
  int steps = adx > ady ? adx : ady;
  if (steps > 18) {
    x0 = x1 - dx * 18 / steps;
    y0 = y1 - dy * 18 / steps;
    steps = 18;
  }
  if (steps < 1) {
    plot(band, x1, y1, color, cover);
    return;
  }
  for (int i = 0; i <= steps; ++i) {
    int x = x0 + dx * i / steps;
    int y = y0 + dy * i / steps;
    int fade = cover * (i + 1) / (steps + 1);
    plot(band, x, y, color, fade);
  }
}

AetherRgb shade_warp(int x, int y, uint32_t time_ms) {
  int pulse = 230 + sin256((int)(time_ms / 12)) / 8;
  if (y < kHorizon) {
    int sky = y * 255 / kHorizon;
    AetherRgb color = rgb(6 + sky * 78 / 255, 2 + sky * 12 / 255, 24 + sky * 36 / 255);
    int dx = x - 86;
    int dy = y - kHorizon;
    int dist2 = dx * dx + dy * dy;
    int glow = kSunR + 34;
    if (dist2 < glow * glow) {
      int gain = (glow * glow - dist2) * pulse / (glow * glow);
      color.r = (uint8_t)clamp255(color.r + gain);
      color.g = (uint8_t)clamp255(color.g + gain * 2 / 5);
      color.b = (uint8_t)clamp255(color.b + gain / 7);
    }
    if (dist2 <= kSunR * kSunR) {
      bool stripe = ((y / 3) & 1) == 0;
      if (dist2 < 12 * 12) {
        color = rgb(255, 244, 214);
      } else if (stripe) {
        color = rgb(255 * pulse / 255, 176 * pulse / 255, 48);
      } else {
        color = rgb(255, 58 * pulse / 230, 72);
      }
    }
    if (y >= kHorizon - 2 && dist2 > kSunR * kSunR) {
      color = rgb(255, 140, 90);
    }
    return color;
  }

  int rel = y - kHorizon;
  if (rel < 1) {
    rel = 1;
  }
  int depth = 4200 / rel;
  int scroll = (int)(time_ms / 5);
  int shift = (x - 86) * 90 / rel;
  bool vertical = floor_mod(shift, 22) == 0;
  bool horizontal = floor_mod(depth + scroll, 34) < 2;
  int fade = rel * 255 / 110;
  if (fade > 255) {
    fade = 255;
  }
  AetherRgb ground = rgb(16 + rel / 6, 3, 22 + rel / 8);
  if (vertical || horizontal) {
    int line = 30 + fade * 3 / 4;
    ground = rgb(70 + line, line / 6, 50 + line / 2);
  }
  int column = x > 86 ? x - 86 : 86 - x;
  if (column < 16) {
    int reflect = (16 - column) * (110 - rel) / 16;
    if (reflect > 0) {
      ground.r = (uint8_t)clamp255(ground.r + reflect);
      ground.g = (uint8_t)clamp255(ground.g + reflect / 3);
      ground.b = (uint8_t)clamp255(ground.b + reflect / 5);
    }
  }
  return ground;
}

AetherRgb shade_core(int x, int y, uint32_t time_ms) {
  uint32_t hash = (uint32_t)x * 374761393u ^ (uint32_t)y * 668265263u;
  AetherRgb color = rgb(3, 1, 8);
  if ((hash & 511u) == 0) {
    color = rgb(28, 24, 48);
  }
  int dx = x - 86;
  int dy = y - 156;
  int dist = isqrt(dx * dx + dy * dy);
  int beat = 210 + sin256((int)(time_ms / 7)) * 45 / 255;
  if (dist < 130) {
    int gain = (130 - dist) * beat / 130;
    color.r = (uint8_t)clamp255(gain);
    color.g = (uint8_t)clamp255(gain * (36 + dist / 2) / 160);
    color.b = (uint8_t)clamp255(18 + gain * dist / 220);
  }
  if (dist < 18) {
    int hot = (18 - dist) * 255 / 18;
    color.r = (uint8_t)clamp255(color.r + hot);
    color.g = (uint8_t)clamp255(color.g + hot * 9 / 10);
    color.b = (uint8_t)clamp255(color.b + hot * 7 / 10);
  }
  for (int ring = 0; ring < 3; ++ring) {
    int radius = 30 + (int)(((time_ms / 14) + (uint32_t)ring * 26) % 84);
    int delta = dist - radius;
    if (delta < 0) {
      delta = -delta;
    }
    if (delta <= 1) {
      color.r = (uint8_t)clamp255(color.r + 70);
      color.g = (uint8_t)clamp255(color.g + 170);
      color.b = 255;
    } else if (delta == 2) {
      color.b = (uint8_t)clamp255(color.b + 90);
    }
  }
  return color;
}

AetherRgb palette(int hue) {
  const uint8_t stops[4][3] = {
      {110, 48, 210},
      {36, 230, 210},
      {90, 255, 110},
      {240, 60, 150},
  };
  int index = (hue & 255) / 64;
  int next = (index + 1) & 3;
  int blend_step = (hue & 63) * 4;
  return rgb(
      stops[index][0] + ((int)stops[next][0] - stops[index][0]) * blend_step / 256,
      stops[index][1] + ((int)stops[next][1] - stops[index][1]) * blend_step / 256,
      stops[index][2] + ((int)stops[next][2] - stops[index][2]) * blend_step / 256);
}

AetherRgb shade_veil(int x, int y, uint32_t time_ms) {
  int wave = sin256(x * 3 + (int)(time_ms / 22));
  int ripple = sin256(y + wave / 3 - (int)(time_ms / 16));
  int lift = sin256(x * 5 - (int)(time_ms / 28));
  int mag = ripple + lift / 2;
  int bright = (mag + 420) * 255 / 840;
  bright = clamp255(bright);
  int curtain = bright - 70;
  if (curtain < 0) {
    curtain = 0;
  }
  // Soften the night between curtains so the bands read as ribbons, not noise.
  if (curtain < 40) {
    curtain = curtain * curtain / 40;
  }
  AetherRgb ink = palette(x * 2 + y / 5 + (int)(time_ms / 35));
  AetherRgb color = rgb(4 + ink.r * curtain / 255, 8 + ink.g * curtain / 255,
                         20 + ink.b * curtain / 280);
  uint32_t hash = (uint32_t)x * 2246822519u ^ (uint32_t)y * 3266489917u;
  if (curtain < 18 && (hash & 1023u) == 0) {
    color = rgb(180, 190, 220);
  }
  return color;
}

AetherRgb shade(int scene, int x, int y, uint32_t time_ms) {
  if (scene == 0) {
    return shade_warp(x, y, time_ms);
  }
  if (scene == 1) {
    return shade_core(x, y, time_ms);
  }
  return shade_veil(x, y, time_ms);
}

void vignette(AetherRgb* color, int x, int y) {
  int edge = x;
  int right = kAetherWidth - 1 - x;
  int bottom = kAetherHeight - 1 - y;
  if (right < edge) {
    edge = right;
  }
  if (y < edge) {
    edge = y;
  }
  if (bottom < edge) {
    edge = bottom;
  }
  if (edge >= 22) {
    return;
  }
  int gain = 90 + edge * 165 / 22;
  color->r = (uint8_t)(color->r * gain / 255);
  color->g = (uint8_t)(color->g * gain / 255);
  color->b = (uint8_t)(color->b * gain / 255);
}

int star_depth(const Star& star, uint32_t time_ms) {
  uint32_t travel = star.phase + (uint32_t)(((uint64_t)time_ms * star.rate) / 18);
  int depth = 1000 - (int)(travel % 960);
  if (depth < 48) {
    depth = 48;
  }
  return depth;
}

void draw_stars(Band& band, uint32_t time_ms) {
  for (int i = 0; i < kStarCount; ++i) {
    const Star& star = g_stars[i];
    int depth = star_depth(star, time_ms);
    int previous = star_depth(star, time_ms > 45 ? time_ms - 45 : 0);
    int x = 86 + star.x * 100 / depth;
    int y = 108 + star.y * 70 / depth;
    if (x < -2 || x >= kAetherWidth + 2 || y < -2 || y >= kHorizon) {
      continue;
    }
    int dx = x - 86;
    int dy = y - kHorizon;
    if (dx * dx + dy * dy <= kSunR * kSunR && y < kHorizon) {
      continue;
    }
    int bright = (1000 - depth) / 3;
    if (bright < 40) {
      bright = 40;
    }
    AetherRgb color = rgb(bright, bright * 9 / 10, clamp255(bright + 50));
    if (previous >= depth) {
      int px = 86 + star.x * 100 / previous;
      int py = 108 + star.y * 70 / previous;
      plot_line(band, px, py, x, y, color, clamp255(bright));
    }
    plot(band, x, y, color, 255);
    if (bright > 150) {
      plot(band, x + 1, y, color, 140);
      plot(band, x, y + 1, color, 140);
    }
  }
}

void draw_ship(Band& band, uint32_t time_ms) {
  int sx = 86 + sin256((int)(time_ms / 11)) / 9;
  int sy = 254;
  for (int y = sy - 11; y <= sy + 7; ++y) {
    int along = y - (sy - 11);
    int half = along * 8 / 18;
    for (int x = sx - half; x <= sx + half; ++x) {
      AetherRgb body = y > sy + 1 ? rgb(46, 64, 96) : rgb(214, 232, 255);
      if (x == sx && y < sy - 4) {
        body = rgb(120, 230, 255);
      }
      plot(band, x, y, body, 255);
    }
  }
  int flame = 5 + (sin256((int)(time_ms / 2)) + 255) / 90;
  for (int i = 1; i <= flame; ++i) {
    AetherRgb fire = rgb(255, 200 - i * 28, 40);
    plot(band, sx, sy + 7 + i, fire, 255);
    plot(band, sx - 1, sy + 7 + i, fire, 150);
    plot(band, sx + 1, sy + 7 + i, fire, 150);
  }
}

void draw_orbiters(Band& band, uint32_t time_ms) {
  for (int i = 0; i < 7; ++i) {
    int angle = (int)(time_ms / 6) + i * (256 / 7);
    int radius = 58 + sin256((int)(time_ms / 13) + i * 28) / 12;
    int x = 86 + cos256(angle) * radius / 255;
    int y = 156 + sin256(angle) * radius / 255;
    for (int tail = 4; tail >= 1; --tail) {
      int behind = angle - tail * 4;
      int tx = 86 + cos256(behind) * radius / 255;
      int ty = 156 + sin256(behind) * radius / 255;
      plot(band, tx, ty, rgb(90, 180, 255), 220 - tail * 40);
    }
    AetherRgb head = rgb(255, 255, 255);
    plot(band, x, y, head, 255);
    plot(band, x + 1, y, head, 200);
    plot(band, x, y + 1, head, 200);
    plot(band, x - 1, y, rgb(140, 220, 255), 160);
  }
}

void draw_sparks(Band& band, uint32_t time_ms) {
  for (int i = 0; i < 22; ++i) {
    int x = (i * 37 + 11) % kAetherWidth;
    x += sin256((int)(time_ms / 18) + i * 19) / 18;
    int y = kAetherHeight - (int)((time_ms / 8 + (uint32_t)i * 73) % (kAetherHeight + 24));
    AetherRgb spark = rgb(230, 255, 240);
    plot(band, x, y, spark, 230);
    plot(band, x, y + 1, rgb(120, 220, 200), 140);
    plot(band, x, y + 2, rgb(60, 120, 140), 80);
  }
}

struct Glyph {
  char letter;
  uint8_t column[5];
};

// 5x7 glyphs, bit 0 is the top row.
constexpr Glyph kFont[] = {
    {'A', {0x7E, 0x09, 0x09, 0x09, 0x7E}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x00}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x41, 0x41, 0x7F, 0x41, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x04, 0x02, 0x7F}},
    {'N', {0x7F, 0x02, 0x04, 0x08, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'V', {0x0F, 0x30, 0x40, 0x30, 0x0F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
};

const Glyph* find_glyph(char letter) {
  for (unsigned i = 0; i < sizeof(kFont) / sizeof(kFont[0]); ++i) {
    if (kFont[i].letter == letter) {
      return &kFont[i];
    }
  }
  return 0;
}

int text_width(const char* text, int scale) {
  int count = 0;
  for (const char* cursor = text; *cursor; ++cursor) {
    ++count;
  }
  if (count == 0) {
    return 0;
  }
  return count * 5 * scale + (count - 1) * scale;
}

void draw_text(Band& band, int x, int y, const char* text, int scale, AetherRgb color, int cover) {
  int cursor_x = x;
  for (const char* letter = text; *letter; ++letter) {
    const Glyph* glyph = find_glyph(*letter);
    if (glyph) {
      for (int column = 0; column < 5; ++column) {
        uint8_t bits = glyph->column[column];
        for (int row = 0; row < 7; ++row) {
          if ((bits & (1 << row)) == 0) {
            continue;
          }
          for (int sy = 0; sy < scale; ++sy) {
            for (int sx = 0; sx < scale; ++sx) {
              int px = cursor_x + column * scale + sx;
              int py = y + row * scale + sy;
              plot(band, px + 1, py + 1, rgb(0, 0, 0), cover * 2 / 3);
              plot(band, px, py, color, cover);
            }
          }
        }
      }
    }
    cursor_x += 6 * scale;
  }
}

void draw_centered(Band& band, int y, const char* text, int scale, AetherRgb color, int cover) {
  int x = (kAetherWidth - text_width(text, scale)) / 2;
  draw_text(band, x, y, text, scale, color, cover);
}

const char* scene_name(int scene) {
  if (scene == 0) {
    return "WARP";
  }
  if (scene == 1) {
    return "CORE";
  }
  return "VEIL";
}

void draw_scene_sprites(Band& band, int scene, uint32_t time_ms) {
  if (scene == 0) {
    draw_stars(band, time_ms);
    draw_ship(band, time_ms);
  } else if (scene == 1) {
    draw_orbiters(band, time_ms);
  } else {
    draw_sparks(band, time_ms);
  }
}

}  // namespace

void aether_render(uint32_t time_ms, int y0, int rows, AetherRgb* pixels) {
  prepare();
  if (rows <= 0 || pixels == 0 || y0 >= kAetherHeight) {
    return;
  }
  if (y0 < 0) {
    rows += y0;
    pixels += (-y0) * kAetherWidth;
    y0 = 0;
  }
  if (y0 + rows > kAetherHeight) {
    rows = kAetherHeight - y0;
  }
  if (rows <= 0) {
    return;
  }

  int scene = 0;
  int next_scene = 0;
  int wipe_y = -1;
  scene_clock(time_ms, &scene, &next_scene, &wipe_y);

  for (int row = 0; row < rows; ++row) {
    int y = y0 + row;
    int which = scene;
    int seam = 0;
    if (wipe_y >= 0) {
      if (y < wipe_y) {
        which = next_scene;
      }
      int distance = y - wipe_y;
      if (distance < 0) {
        distance = -distance;
      }
      if (distance == 0) {
        seam = 255;
      } else if (distance == 1) {
        seam = 120;
      }
    }
    AetherRgb* out = pixels + row * kAetherWidth;
    for (int x = 0; x < kAetherWidth; ++x) {
      AetherRgb color = shade(which, x, y, time_ms);
      if (seam) {
        blend(&color, rgb(255, 250, 240), seam);
      }
      vignette(&color, x, y);
      out[x] = color;
    }
  }

  Band band;
  band.pixels = pixels;
  band.y0 = y0;
  band.rows = rows;

  // Sprites follow the scene on each side of the wipe, so a band that
  // straddles the seam gets both layers and the seam stays bright.
  if (wipe_y >= 0 && wipe_y > y0 && wipe_y < y0 + rows) {
    draw_scene_sprites(band, next_scene, time_ms);
    draw_scene_sprites(band, scene, time_ms);
  } else {
    int which = scene;
    if (wipe_y >= 0 && y0 < wipe_y) {
      which = next_scene;
    }
    draw_scene_sprites(band, which, time_ms);
  }

  int splash = 0;
  if (time_ms < 2600) {
    splash = 255;
  } else if (time_ms < 4200) {
    splash = (int)(4200 - time_ms) * 255 / 1600;
  }
  if (splash > 0) {
    draw_centered(band, 58, "AETHER", 3, rgb(255, 246, 232), splash);
    draw_centered(band, 88, "MACHINA", 1, rgb(180, 210, 255), splash * 4 / 5);
  }

  int label_scene = scene;
  if (wipe_y >= 0 && 300 < wipe_y) {
    label_scene = next_scene;
  }
  draw_centered(band, 302, scene_name(label_scene), 1, rgb(232, 236, 255), 190);
}

AetherLamp aether_lamp(uint32_t time_ms) {
  prepare();
  int scene = 0;
  int next_scene = 0;
  int wipe_y = -1;
  scene_clock(time_ms, &scene, &next_scene, &wipe_y);
  int which = scene;
  if (wipe_y >= kAetherHeight / 2) {
    which = next_scene;
  }
  int breathe = sin256((int)(time_ms / 9));
  AetherLamp lamp;
  if (which == 0) {
    lamp.r = (uint8_t)clamp255(70 + breathe / 8);
    lamp.g = (uint8_t)clamp255(18 + breathe / 20);
    lamp.b = 4;
  } else if (which == 1) {
    int beat = 40 + (breathe + 255) / 6;
    lamp.r = (uint8_t)clamp255(beat + 30);
    lamp.g = (uint8_t)clamp255(beat / 3);
    lamp.b = (uint8_t)clamp255(8 + beat / 8);
  } else {
    lamp.r = (uint8_t)clamp255(12 + breathe / 16);
    lamp.g = (uint8_t)clamp255(48 + breathe / 8);
    lamp.b = (uint8_t)clamp255(28 + breathe / 12);
  }
  return lamp;
}

int aether_scene(uint32_t time_ms) {
  return (int)((time_ms / kAetherSceneMs) % kSceneCount);
}
