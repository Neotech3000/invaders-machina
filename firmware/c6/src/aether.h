#pragma once

#include <stdint.h>

// Portrait framebuffer for the ESP32-C6-LCD-1.47 panel.
enum { kAetherWidth = 172, kAetherHeight = 320, kAetherSceneMs = 14000 };

struct AetherRgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct AetherLamp {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// Fill `rows` scanlines starting at y0. `pixels` has rows * kAetherWidth entries.
void aether_render(uint32_t time_ms, int y0, int rows, AetherRgb* pixels);

// Acrylic LED color for the same clock. One WS2812, so this is a single pixel.
AetherLamp aether_lamp(uint32_t time_ms);

// 0 warp, 1 core, 2 veil. The wipe still belongs to the scene it started in.
int aether_scene(uint32_t time_ms);
