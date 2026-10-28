#include <Arduino.h>

#include "handoff/device_handoff.h"
#include "hardware/board.h"
#include "transport/serial_link.h"

// 115200 is the unconfirmed serial-monitor placeholder. The protocol
// port is the only UART, so this image writes NDJSON and nothing else.

static constexpr uint32_t kMonitorBaudPlaceholder = 115200;

static SerialLink* serialLink = nullptr;
static DeviceHandoff* handoff = nullptr;

void setup() {
  Serial.setRxBufferSize(2048);
  Serial.setTxBufferSize(1024);
  Serial.begin(kMonitorBaudPlaceholder);
  if (!board_init()) {
    while (true) {
      delay(1000);
    }
  }
  serialLink = new SerialLink(Serial);
  handoff = new DeviceHandoff(*make_display(), *serialLink);
  handoff->begin();
}

void loop() {
  if (handoff != nullptr) {
    handoff->poll();
  }
  if (Display* display = make_display()) {
    display->tick();
  }
  delay(1);
}
