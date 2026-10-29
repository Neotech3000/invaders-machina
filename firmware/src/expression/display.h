#pragma once

// Display surface used by the device app. No board headers.

class Display {
 public:
  virtual ~Display() = default;
  // Advance the attract demo, the menu, or the fight. Must return quickly so
  // the touch sampler stays responsive.
  virtual void tick() {}
};
