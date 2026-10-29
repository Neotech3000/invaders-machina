#pragma once

#include <cstdint>

// One cropped friend head. The magenta background is not part of the face.
struct FriendFace {
  const uint8_t* png;
  uint32_t length;
  int width;
  int height;
};

const FriendFace& friend_face(int index);
int friend_face_count();
