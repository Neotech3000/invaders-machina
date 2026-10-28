#include "envelope.h"

#include "protocol_constants.h"

#include <ArduinoJson.h>
#include <string.h>

namespace {

bool copyField(char* dst, size_t cap, const char* src) {
  if (src == nullptr) {
    dst[0] = '\0';
    return false;
  }
  size_t n = strlen(src);
  if (n >= cap) {
    dst[0] = '\0';
    return false;
  }
  memcpy(dst, src, n + 1);
  return true;
}

bool validId(const char* id) {
  size_t n = strlen(id);
  if (n < 3 || n > 32) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    char c = id[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool emit(JsonDocument& doc, char* out, size_t cap) {
  size_t n = serializeJson(doc, out, cap);
  return n > 0 && n < cap;
}

}  // namespace

bool parseEnvelope(const char* line, Envelope& out) {
  out = Envelope{};
  if (line == nullptr || line[0] == '\0') {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) {
    return false;
  }
  if (!doc.is<JsonObject>()) {
    return false;
  }
  JsonVariant v = doc["v"];
  JsonVariant type = doc["type"];
  JsonVariant id = doc["id"];
  JsonVariant payload = doc["payload"];
  if (!v.is<int>() || !type.is<const char*>() || !id.is<const char*>() ||
      !payload.is<JsonObject>()) {
    return false;
  }
  out.v = v.as<int>();
  if (!copyField(out.type, sizeof out.type, type.as<const char*>())) {
    return false;
  }
  if (!copyField(out.id, sizeof out.id, id.as<const char*>()) || !validId(out.id)) {
    return false;
  }
  JsonObject body = payload.as<JsonObject>();
  if (body["role"].is<const char*>()) {
    copyField(out.role, sizeof out.role, body["role"].as<const char*>());
  }
  if (body["owner"].is<const char*>()) {
    copyField(out.owner, sizeof out.owner, body["owner"].as<const char*>());
  }
  if (body["expression"].is<const char*>()) {
    if (copyField(out.expression, sizeof out.expression, body["expression"].as<const char*>())) {
      out.hasExpression = true;
    }
  }
  if (body["reason"].is<const char*>()) {
    copyField(out.reason, sizeof out.reason, body["reason"].as<const char*>());
  }
  if (body["ref"].is<const char*>()) {
    copyField(out.ref, sizeof out.ref, body["ref"].as<const char*>());
  }
  if (body["code"].is<const char*>()) {
    copyField(out.code, sizeof out.code, body["code"].as<const char*>());
  }
  if (body["proto"].is<int>()) {
    out.hasProto = true;
    out.proto = body["proto"].as<int>();
  }
  if (body["ok"].is<bool>()) {
    out.hasOk = true;
    out.ok = body["ok"].as<bool>();
  }
  if (body["expressions"].is<JsonArray>()) {
    JsonArray names = body["expressions"].as<JsonArray>();
    for (JsonVariant item : names) {
      const char* name = item.as<const char*>();
      if (name == nullptr) {
        continue;
      }
      if (strcmp(name, kExprIdle) == 0) {
        out.exprIdle = true;
      }
    }
  }
  return true;
}

bool formatHello(char* out, size_t cap, const char* id) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "hello";
  doc["id"] = id;
  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["role"] = "device";
  payload["proto"] = kProtoVersion;
  JsonArray expressions = payload["expressions"].to<JsonArray>();
  expressions.add(kExprIdle);
  payload["owner"] = "device";
  return emit(doc, out, cap);
}

bool formatHelloAck(char* out, size_t cap, const char* id, bool ok, const char* reason) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "hello.ack";
  doc["id"] = id;
  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["ok"] = ok;
  payload["proto"] = kProtoVersion;
  if (reason != nullptr && reason[0] != '\0') {
    payload["reason"] = reason;
  }
  return emit(doc, out, cap);
}

bool formatPing(char* out, size_t cap, const char* id) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "ping";
  doc["id"] = id;
  doc["payload"].to<JsonObject>();
  return emit(doc, out, cap);
}

bool formatPong(char* out, size_t cap, const char* id, const char* ref) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "pong";
  doc["id"] = id;
  doc["payload"]["ref"] = ref;
  return emit(doc, out, cap);
}

bool formatAccept(char* out, size_t cap, const char* id, const char* expression) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "handoff.accept";
  doc["id"] = id;
  doc["payload"]["expression"] = expression;
  return emit(doc, out, cap);
}

bool formatCommit(char* out, size_t cap, const char* id, const char* expression) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "handoff.commit";
  doc["id"] = id;
  doc["payload"]["expression"] = expression;
  return emit(doc, out, cap);
}

bool formatShown(char* out, size_t cap, const char* id, const char* expression) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "handoff.shown";
  doc["id"] = id;
  doc["payload"]["expression"] = expression;
  return emit(doc, out, cap);
}

bool formatAbort(char* out, size_t cap, const char* id, const char* reason) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "handoff.abort";
  doc["id"] = id;
  doc["payload"]["reason"] = reason;
  return emit(doc, out, cap);
}

bool formatError(char* out, size_t cap, const char* id, const char* ref, const char* code,
                 const char* message) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "error";
  doc["id"] = id;
  JsonObject payload = doc["payload"].to<JsonObject>();
  if (ref != nullptr && ref[0] != '\0') {
    payload["ref"] = ref;
  }
  payload["code"] = code;
  payload["message"] = message;
  return emit(doc, out, cap);
}

bool formatGameResult(char* out, size_t cap, const char* id, bool hit, int score) {
  JsonDocument doc;
  doc["v"] = kProtoVersion;
  doc["type"] = "game.result";
  doc["id"] = id;
  doc["payload"]["hit"] = hit;
  doc["payload"]["score"] = score;
  return emit(doc, out, cap);
}
