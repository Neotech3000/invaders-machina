#pragma once

#include <stdint.h>

// Coin amounts shared by the firmware and the browser port (web/index.html).
// A drop is capped at the number of coins that can be on screen at once.
// Replaying a cleared level, or a cleared god goon wave, pays 4 fewer, floored at 0.

static constexpr int kCoinCap = 16;
static constexpr int kCoinReplayPenalty = 4;
static constexpr int kGodWaveMiniCoins = 5;
static constexpr int kCampaignLevels = 300;
static constexpr int kGodLevelSlots = 12;
static constexpr int kGodWaveSlots = 26;
static constexpr int kClearedLevelBytes = (kCampaignLevels + 7) / 8;
static constexpr int kClearedWaveBytes = (kGodLevelSlots * kGodWaveSlots + 7) / 8;

inline int boss_bounty(int base, int level) {
  if (base < 0) return 0;
  int n = base;
  if (level > 0) n += level / 25;
  if (n > kCoinCap) n = kCoinCap;
  return n;
}

inline int coin_payout(int base, bool replay) {
  int n = base;
  if (n > kCoinCap) n = kCoinCap;
  if (n < 0) n = 0;
  if (replay) n -= kCoinReplayPenalty;
  if (n < 0) n = 0;
  return n;
}

inline bool campaign_level(int level) {
  return level >= 1 && level <= kCampaignLevels;
}

inline bool coin_god_level(int level) {
  return campaign_level(level) && (level % 25) == 0;
}

// Bit index for one goon wave of a god level. Levels are 25, 50, ... 300.
// Wave 0 is the first goon wave. Returns -1 when the pair is not a god wave.
inline int god_wave_bit(int level, int wave) {
  if (!coin_god_level(level)) return -1;
  if (wave < 0 || wave >= kGodWaveSlots) return -1;
  const int god = level / 25 - 1;
  if (god < 0 || god >= kGodLevelSlots) return -1;
  return god * kGodWaveSlots + wave;
}

inline bool bit_test(const uint8_t* bytes, int nbytes, int bit) {
  if (bytes == nullptr || bit < 0) return false;
  const int byte = bit >> 3;
  if (byte >= nbytes) return false;
  return (bytes[byte] & static_cast<uint8_t>(1u << (bit & 7))) != 0;
}

inline void bit_set(uint8_t* bytes, int nbytes, int bit) {
  if (bytes == nullptr || bit < 0) return;
  const int byte = bit >> 3;
  if (byte >= nbytes) return;
  bytes[byte] = static_cast<uint8_t>(bytes[byte] | static_cast<uint8_t>(1u << (bit & 7)));
}

inline bool level_cleared(const uint8_t* levels, int level) {
  if (!campaign_level(level)) return false;
  return bit_test(levels, kClearedLevelBytes, level - 1);
}

inline void mark_level_cleared(uint8_t* levels, int level) {
  if (!campaign_level(level)) return;
  bit_set(levels, kClearedLevelBytes, level - 1);
}

inline bool god_wave_cleared(const uint8_t* waves, int level, int wave) {
  return bit_test(waves, kClearedWaveBytes, god_wave_bit(level, wave));
}

inline void mark_god_wave_cleared(uint8_t* waves, int level, int wave) {
  bit_set(waves, kClearedWaveBytes, god_wave_bit(level, wave));
}

// in_god_wave is true only during the goon waves, not during the god fight.
inline bool coin_replay(const uint8_t* levels, const uint8_t* waves, int level, bool in_god_wave,
                        int god_wave) {
  if (level_cleared(levels, level)) return true;
  if (in_god_wave && god_wave_cleared(waves, level, god_wave)) return true;
  return false;
}
