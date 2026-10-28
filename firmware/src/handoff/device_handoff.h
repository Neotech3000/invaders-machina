#pragma once

#include "../expression/display.h"
#include "../transport/link.h"

#include <stddef.h>
#include <stdint.h>

// Device handoff. States: local, linked, await_ready, away, return_shown.
// Includes no board header. Touch does not call into this.

class DeviceHandoff {
 public:
  DeviceHandoff(Display& display, Link& link);
  void begin();
  void poll();

 private:
  enum class State { Local, Linked, AwaitReady, Away, ReturnShown };

  struct Slot {
    bool used = false;
    bool replay = false;
    char id[33] = {};
    char line[256] = {};
  };

  static void onLineThunk(const char* line, void* ctx);
  void onLine(const char* line);
  void linkDown();
  void helloTimedOut();
  void sendHello();
  void sendPing();
  void sendAbort(const char* reason);
  void sendError(const char* ref, const char* code, const char* message);
  void sendRaw(const char* line);
  void rememberReplay(Slot* slot, const char* line);
  void makeId(char* out, size_t cap);
  void clearDedup();
  Slot* findSlot(const char* id);
  Slot* insertSlot(const char* id);
  bool deviceKnows(const char* expression) const;
  bool peerKnows(const char* expression) const;
  void noteMalformed();
  void enterLinked();

  Display& display_;
  Link& link_;
  State state_ = State::Local;
  Slot ring_[32];
  uint8_t ringNext_ = 0;
  uint32_t seq_ = 1;
  uint32_t helloSentAt_ = 0;
  uint32_t lastRx_ = 0;
  uint32_t lastPing_ = 0;
  uint32_t readyAt_ = 0;
  uint32_t commitAt_ = 0;
  uint32_t returnAt_ = 0;
  bool helloOutstanding_ = false;
  bool readyArmed_ = false;
  bool commitArmed_ = false;
  bool returnArmed_ = false;
  bool peerIdle_ = false;
  int malformed_ = 0;
  char accepted_[16] = {};
  char commitId_[33] = {};
  char commitLine_[256] = {};
  char shownId_[33] = {};
  char shownLine_[256] = {};
  char scratch_[384] = {};
};
