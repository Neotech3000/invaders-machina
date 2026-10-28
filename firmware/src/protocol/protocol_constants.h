#pragma once

#include <stdint.h>

// Mirrors the plan's v1 numbers. The device image does not parse the
// shared JSON schema. Baud is not a protocol constant.

static constexpr int kProtoVersion = 1;
static constexpr uint32_t kHelloMs = 2000;
static constexpr uint32_t kReadyMs = 1500;
static constexpr uint32_t kCommitMs = 1500;
static constexpr uint32_t kReturnMs = 1500;
static constexpr uint32_t kPingIntervalMs = 1000;
static constexpr uint32_t kSilenceMs = 3000;
static constexpr int kDedupSize = 32;
static constexpr int kMaxLineBytes = 1024;
static constexpr int kMalformedLimit = 5;

static constexpr const char* kExprIdle = "idle";
