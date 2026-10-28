#pragma once

#include <cstdint>

// A cropped face bitmap. idle is the orange tabby. parked is the wink.
struct CatFace {
  const uint8_t* png;
  uint32_t length;
  int width;
  int height;
};

const CatFace& cat_face_for(const char* expression);
