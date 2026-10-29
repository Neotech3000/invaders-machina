#pragma once

// Display surface used by the device app. No board headers.

class Display {
 public:
  virtual ~Display() = default;
  virtual void show(const char* expression) = 0;
  virtual void hide() = 0;
  virtual bool isVisible() const = 0;
  // Advance the local face, the walk, or invaders. Must return quickly so the link keeps polling.
  virtual void tick() {}
  // "solo", "linked", "leaving", or "away". The panel uses this for the status line.
  virtual void setPresence(const char* presence) { (void)presence; }
};
