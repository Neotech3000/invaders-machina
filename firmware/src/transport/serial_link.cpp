#include "serial_link.h"

#include "../protocol/protocol_constants.h"

void SerialLink::poll() {
  while (serial_.available() > 0) {
    int raw = serial_.read();
    if (raw < 0) {
      return;
    }
    char c = static_cast<char>(raw);
    if (c == '\n') {
      if (overflow_) {
        overflow_ = false;
        len_ = 0;
        continue;
      }
      if (len_ > 0 && buf_[len_ - 1] == '\r') {
        buf_[len_ - 1] = '\0';
        len_ -= 1;
      } else {
        buf_[len_] = '\0';
      }
      if (len_ > 0 && fn_ != nullptr) {
        fn_(buf_, ctx_);
      }
      len_ = 0;
      continue;
    }
    if (overflow_ || len_ >= kMaxLineBytes) {
      overflow_ = true;
      len_ = 0;
      continue;
    }
    buf_[len_++] = c;
  }
}
