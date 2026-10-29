#include <Arduino.h>

#include "hardware/board.h"

// Invaders Machina is a self-contained cabinet: the panel boots straight into
// the attract demo and never waits on a host. No serial protocol, no handoff.

void setup() {
  Serial.begin(115200);
  if (!board_init()) {
    while (true) {
      delay(1000);
    }
  }
}

void loop() {
  if (Display* display = make_display()) {
    display->tick();
  }
  delay(1);
}
