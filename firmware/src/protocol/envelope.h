#pragma once

#include <stddef.h>
#include <stdint.h>

// One NDJSON envelope. Fixed buffers so the device does not allocate
// per line. payload strings are copied out of the JSON.

struct Envelope {
  int v = 0;
  char type[32] = {};
  char id[33] = {};
  char role[16] = {};
  char owner[16] = {};
  char expression[16] = {};
  char reason[32] = {};
  char ref[33] = {};
  char code[32] = {};
  int proto = 0;
  bool hasProto = false;
  bool hasOk = false;
  bool ok = false;
  bool hasExpression = false;
  bool exprIdle = false;
};

// Returns false when the line is not one JSON object with v, type, and id.
bool parseEnvelope(const char* line, Envelope& out);

bool formatHello(char* out, size_t cap, const char* id);
bool formatHelloAck(char* out, size_t cap, const char* id, bool ok, const char* reason);
bool formatPing(char* out, size_t cap, const char* id);
bool formatPong(char* out, size_t cap, const char* id, const char* ref);
bool formatAccept(char* out, size_t cap, const char* id, const char* expression);
bool formatCommit(char* out, size_t cap, const char* id, const char* expression);
bool formatShown(char* out, size_t cap, const char* id, const char* expression);
bool formatAbort(char* out, size_t cap, const char* id, const char* reason);
bool formatError(char* out, size_t cap, const char* id, const char* ref, const char* code,
                 const char* message);
bool formatGameResult(char* out, size_t cap, const char* id, bool hit, int score);
