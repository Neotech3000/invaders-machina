#pragma once

#include "link.h"

#include <Arduino.h>

// NDJSON on the default UART (the CH340). No log text is written here.
// Baud is chosen by the caller. 115200 is the unconfirmed placeholder.

class SerialLink : public Link {
 public:
  explicit SerialLink(HardwareSerial& serial) : serial_(serial) {}

  void send(const char* line) override {
    serial_.print(line);
    serial_.print('\n');
    serial_.flush();
  }

  void setLineHandler(LineFn fn, void* ctx) override {
    fn_ = fn;
    ctx_ = ctx;
  }

  void poll() override;

 private:
  HardwareSerial& serial_;
  LineFn fn_ = nullptr;
  void* ctx_ = nullptr;
  char buf_[1025] = {};
  size_t len_ = 0;
  bool overflow_ = false;
};
