#include "device_handoff.h"

#include "../protocol/envelope.h"
#include "../protocol/protocol_constants.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

static_assert(kDedupSize == 32, "dedup ring length");

namespace {

bool isType(const Envelope& msg, const char* type) {
  return strcmp(msg.type, type) == 0;
}

bool same(const char* a, const char* b) { return strcmp(a, b) == 0; }

}  // namespace

DeviceHandoff::DeviceHandoff(Display& display, Link& link)
    : display_(display), link_(link) {
  link_.setLineHandler(&DeviceHandoff::onLineThunk, this);
}

void DeviceHandoff::onLineThunk(const char* line, void* ctx) {
  static_cast<DeviceHandoff*>(ctx)->onLine(line);
}

void DeviceHandoff::begin() {
  display_.show(kExprIdle);
  sendHello();
}

void DeviceHandoff::poll() {
  link_.poll();
  uint32_t now = millis();
  if (state_ == State::Local) {
    if (helloOutstanding_ && now - helloSentAt_ >= kHelloMs) {
      helloTimedOut();
    }
    return;
  }
  if (now - lastRx_ >= kSilenceMs) {
    linkDown();
    return;
  }
  if (now - lastPing_ >= kPingIntervalMs) {
    sendPing();
    lastPing_ = now;
  }
  if (state_ == State::AwaitReady && readyArmed_ && now - readyAt_ >= kReadyMs) {
    readyArmed_ = false;
    sendAbort("timeout");
    display_.setPresence("linked");
    state_ = State::Linked;
    return;
  }
  if (state_ == State::Away && commitArmed_ && now - commitAt_ >= kCommitMs) {
    commitArmed_ = false;
    display_.setPresence("linked");
    display_.show(kExprIdle);
    sendAbort("timeout");
    state_ = State::Linked;
    return;
  }
  if (state_ == State::ReturnShown && returnArmed_ && now - returnAt_ >= kReturnMs) {
    returnArmed_ = false;
    display_.setPresence("away");
    display_.hide();
    sendAbort("return_timeout");
    commitArmed_ = false;
    state_ = State::Away;
  }
}

void DeviceHandoff::makeId(char* out, size_t cap) {
  snprintf(out, cap, "e-%08lx", static_cast<unsigned long>(seq_++));
}

void DeviceHandoff::sendRaw(const char* line) { link_.send(line); }

void DeviceHandoff::rememberReplay(Slot* slot, const char* line) {
  if (slot == nullptr) {
    return;
  }
  slot->replay = true;
  strncpy(slot->line, line, sizeof slot->line - 1);
  slot->line[sizeof slot->line - 1] = '\0';
}

void DeviceHandoff::clearDedup() {
  for (Slot& slot : ring_) {
    slot = Slot{};
  }
  ringNext_ = 0;
}

DeviceHandoff::Slot* DeviceHandoff::findSlot(const char* id) {
  for (Slot& slot : ring_) {
    if (slot.used && same(slot.id, id)) {
      return &slot;
    }
  }
  return nullptr;
}

DeviceHandoff::Slot* DeviceHandoff::insertSlot(const char* id) {
  Slot& slot = ring_[ringNext_];
  ringNext_ = static_cast<uint8_t>((ringNext_ + 1) % kDedupSize);
  slot = Slot{};
  slot.used = true;
  strncpy(slot.id, id, sizeof slot.id - 1);
  slot.id[sizeof slot.id - 1] = '\0';
  return &slot;
}

bool DeviceHandoff::deviceKnows(const char* expression) const {
  return same(expression, kExprIdle);
}

bool DeviceHandoff::peerKnows(const char* expression) const {
  return same(expression, kExprIdle) && peerIdle_;
}

void DeviceHandoff::sendHello() {
  char id[16];
  makeId(id, sizeof id);
  if (!formatHello(scratch_, sizeof scratch_, id)) {
    return;
  }
  sendRaw(scratch_);
  helloOutstanding_ = true;
  helloSentAt_ = millis();
}

void DeviceHandoff::sendPing() {
  char id[16];
  makeId(id, sizeof id);
  if (!formatPing(scratch_, sizeof scratch_, id)) {
    return;
  }
  sendRaw(scratch_);
}

void DeviceHandoff::sendAbort(const char* reason) {
  char id[16];
  makeId(id, sizeof id);
  if (!formatAbort(scratch_, sizeof scratch_, id, reason)) {
    return;
  }
  sendRaw(scratch_);
}

void DeviceHandoff::sendError(const char* ref, const char* code, const char* message) {
  char id[16];
  makeId(id, sizeof id);
  if (!formatError(scratch_, sizeof scratch_, id, ref, code, message)) {
    return;
  }
  sendRaw(scratch_);
}

void DeviceHandoff::helloTimedOut() {
  helloOutstanding_ = false;
  peerIdle_ = false;
  clearDedup();
  sendHello();
}

void DeviceHandoff::linkDown() {
  readyArmed_ = false;
  commitArmed_ = false;
  returnArmed_ = false;
  helloOutstanding_ = false;
  peerIdle_ = false;
  accepted_[0] = '\0';
  commitId_[0] = '\0';
  commitLine_[0] = '\0';
  shownId_[0] = '\0';
  shownLine_[0] = '\0';
  malformed_ = 0;
  clearDedup();
  display_.setPresence("solo");
  display_.show(kExprIdle);
  state_ = State::Local;
  sendHello();
}

void DeviceHandoff::enterLinked() {
  state_ = State::Linked;
  helloOutstanding_ = false;
  readyArmed_ = false;
  lastRx_ = millis();
  lastPing_ = millis();
  display_.setPresence("linked");
}

void DeviceHandoff::noteMalformed() {
  malformed_ += 1;
  sendError(nullptr, "bad_json", "malformed line");
  if (malformed_ >= kMalformedLimit) {
    linkDown();
  }
}

void DeviceHandoff::onLine(const char* line) {
  Envelope msg;
  if (!parseEnvelope(line, msg)) {
    noteMalformed();
    return;
  }
  if (msg.v != kProtoVersion) {
    sendError(msg.id, "bad_version", "unsupported version");
    linkDown();
    return;
  }
  if (msg.id[0] == 'e' && msg.id[1] == '-') {
    return;
  }
  malformed_ = 0;
  lastRx_ = millis();

  if (Slot* existing = findSlot(msg.id)) {
    if (existing->replay && existing->line[0] != '\0') {
      sendRaw(existing->line);
    }
    return;
  }
  Slot* slot = insertSlot(msg.id);

  if (isType(msg, "hello")) {
    bool desktop = same(msg.role, "desktop") && msg.hasProto && msg.proto == kProtoVersion &&
                   same(msg.owner, "device");
    char id[16];
    makeId(id, sizeof id);
    const char* why = desktop ? nullptr : "rejected";
    if (!formatHelloAck(scratch_, sizeof scratch_, id, desktop, why)) {
      return;
    }
    sendRaw(scratch_);
    if (!desktop) {
      linkDown();
      return;
    }
    peerIdle_ = msg.exprIdle;
    return;
  }

  if (isType(msg, "hello.ack")) {
    if (!msg.hasOk || !msg.ok || (msg.hasProto && msg.proto != kProtoVersion)) {
      linkDown();
      return;
    }
    if (state_ == State::Local && helloOutstanding_) {
      enterLinked();
    }
    return;
  }

  if (isType(msg, "ping")) {
    char id[16];
    makeId(id, sizeof id);
    if (formatPong(scratch_, sizeof scratch_, id, msg.id)) {
      sendRaw(scratch_);
    }
    return;
  }

  if (isType(msg, "pong") || isType(msg, "error")) {
    return;
  }

  if (isType(msg, "session.end")) {
    linkDown();
    return;
  }

  if (state_ == State::Local) {
    return;
  }

  if (isType(msg, "handoff.request")) {
    if (!msg.hasExpression || !deviceKnows(msg.expression) || !peerKnows(msg.expression)) {
      sendError(msg.id, "bad_expression", "unknown expression");
      return;
    }
    if (state_ == State::ReturnShown) {
      sendAbort("busy");
      return;
    }
    if (state_ == State::Away) {
      if (commitLine_[0] != '\0') {
        rememberReplay(slot, commitLine_);
        sendRaw(commitLine_);
      }
      return;
    }
    if (state_ == State::AwaitReady) {
      char id[16];
      makeId(id, sizeof id);
      if (formatAccept(scratch_, sizeof scratch_, id, accepted_)) {
        sendRaw(scratch_);
      }
      return;
    }
    if (state_ == State::Linked) {
      strncpy(accepted_, msg.expression, sizeof accepted_ - 1);
      accepted_[sizeof accepted_ - 1] = '\0';
      char id[16];
      makeId(id, sizeof id);
      if (!formatAccept(scratch_, sizeof scratch_, id, accepted_)) {
        return;
      }
      sendRaw(scratch_);
      readyArmed_ = true;
      readyAt_ = millis();
      display_.setPresence("leaving");
      state_ = State::AwaitReady;
    }
    return;
  }

  if (isType(msg, "handoff.ready")) {
    if (state_ == State::AwaitReady) {
      if (!msg.hasExpression || !same(msg.expression, accepted_)) {
        sendError(msg.id, "bad_expression", "expression mismatch");
        return;
      }
      display_.setPresence("away");
      display_.hide();
      makeId(commitId_, sizeof commitId_);
      if (!formatCommit(commitLine_, sizeof commitLine_, commitId_, accepted_)) {
        return;
      }
      rememberReplay(slot, commitLine_);
      sendRaw(commitLine_);
      readyArmed_ = false;
      commitArmed_ = true;
      commitAt_ = millis();
      state_ = State::Away;
      return;
    }
    if (state_ == State::Away && commitLine_[0] != '\0') {
      rememberReplay(slot, commitLine_);
      sendRaw(commitLine_);
      return;
    }
    sendError(msg.id, "bad_type", "unexpected ready");
    return;
  }

  if (isType(msg, "handoff.ack")) {
    if (state_ == State::Away && commitArmed_ && msg.ref[0] != '\0' &&
        same(msg.ref, commitId_)) {
      commitArmed_ = false;
    } else if (state_ == State::ReturnShown && returnArmed_ && msg.ref[0] != '\0' &&
               same(msg.ref, shownId_)) {
      returnArmed_ = false;
      state_ = State::Linked;
    }
    return;
  }

  if (isType(msg, "handoff.decline")) {
    if (state_ == State::AwaitReady) {
      readyArmed_ = false;
      sendAbort("rejected");
      display_.setPresence("linked");
      state_ = State::Linked;
      return;
    }
    sendError(msg.id, "bad_type", "unexpected decline");
    return;
  }

  if (isType(msg, "handoff.return")) {
    if (state_ == State::Away) {
      display_.setPresence("linked");
      display_.show(kExprIdle);
      makeId(shownId_, sizeof shownId_);
      if (!formatShown(shownLine_, sizeof shownLine_, shownId_, kExprIdle)) {
        return;
      }
      rememberReplay(slot, shownLine_);
      sendRaw(shownLine_);
      commitArmed_ = false;
      returnArmed_ = true;
      returnAt_ = millis();
      state_ = State::ReturnShown;
      return;
    }
    if (state_ == State::ReturnShown && shownLine_[0] != '\0') {
      rememberReplay(slot, shownLine_);
      sendRaw(shownLine_);
      return;
    }
    sendError(msg.id, "bad_type", "unexpected return");
    return;
  }

  sendError(msg.id, "bad_type", "unexpected type");
}
