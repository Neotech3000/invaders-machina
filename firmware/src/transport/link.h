#pragma once

// Newline-framed byte stream. The state machine never opens a port.

class Link {
 public:
  virtual ~Link() = default;
  virtual void send(const char* line) = 0;
  using LineFn = void (*)(const char* line, void* ctx);
  virtual void setLineHandler(LineFn fn, void* ctx) = 0;
  virtual void poll() = 0;
};
