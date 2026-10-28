#pragma once

// Display surface used by the device app. No board headers.

class Display {
 public:
  virtual ~Display() = default;
  virtual void show(const char* expression) = 0;
  virtual void hide() = 0;
  virtual bool isVisible() const = 0;
  // Advance a local face animation. Must return quickly so the link keeps polling.
  virtual void tick() {}
  // Short wider-eyed reaction. Ignored while the panel is blank.
  virtual void pet() {}
  // Moving-spot game. Taps arrive on the serial link. This board's touch IC is unconfirmed.
  virtual void setSpotGame(bool on) { (void)on; }
  virtual bool spotTap() { return false; }
  virtual int spotScore() const { return 0; }
};
