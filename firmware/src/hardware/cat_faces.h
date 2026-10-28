#pragma once

#include <cstdint>

// One frame of the orange tabby. blink closes the eyes. mouth opens the mouth a little.
// happy widens the eyes for a short pet reaction.
struct CatFace {
  const uint8_t* png;
  uint32_t length;
  int width;
  int height;
};

enum CatFrame : uint8_t { kCatIdle = 0, kCatBlink = 1, kCatMouth = 2, kCatHappy = 3 };

const CatFace& cat_frame(CatFrame frame);
