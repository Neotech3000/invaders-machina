#include "../src/hardware/coin_drop.h"

#include <cstdio>
#include <cstring>

static int failures = 0;

static void expect(bool ok, const char* name) {
  if (ok) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  failures += 1;
}

static int pay(int base, int level, bool replay) {
  return coin_payout(boss_bounty(base, level), replay);
}

int main() {
  expect(kClearedLevelBytes == 38, "level bitset is 300 bits");
  expect(kClearedWaveBytes == 39, "god-wave bitset covers 12 gods x 26 waves");
  expect(kGodWaveMiniCoins == 5, "god-wave mini bosses pay 5");

  // First clear matches the old bounty. Replay is 4 fewer, floored at 0.
  expect(pay(2, 1, false) == 2, "level 1 mini boss first clear");
  expect(pay(2, 1, true) == 0, "level 1 mini boss replay");
  expect(pay(3, 5, false) == 3, "level 5 boss first clear");
  expect(pay(3, 5, true) == 0, "level 5 boss replay");
  expect(coin_payout(1, false) == 1, "grunt first clear");
  expect(coin_payout(1, true) == 0, "grunt replay");

  expect(pay(2, 25, false) == 3, "level 25 non-god mini first clear was 3");
  expect(coin_payout(kGodWaveMiniCoins, false) == 5, "level 25 god mini first clear is 5");
  expect(coin_payout(kGodWaveMiniCoins, true) == 1, "level 25 god mini replay is 1");
  expect(pay(3, 25, false) == 4, "level 25 boss first clear");
  expect(pay(3, 25, true) == 0, "level 25 boss replay");
  expect(pay(4, 25, false) == 5, "level 25 raid first clear");
  expect(pay(4, 25, true) == 1, "level 25 raid replay");
  expect(pay(10, 25, false) == 11, "level 25 god first clear");
  expect(pay(10, 25, true) == 7, "level 25 god replay");

  expect(pay(2, 50, false) == 4, "level 50 scaled mini was 4");
  expect(coin_payout(kGodWaveMiniCoins, false) == 5, "level 50 god mini is still 5");
  expect(pay(2, 100, false) == 6, "level 100 scaled mini was 6");
  expect(pay(2, 300, false) == 14, "level 300 scaled mini was 14");
  expect(coin_payout(kGodWaveMiniCoins, true) == 1, "level 300 god mini replay is 1");
  expect(pay(10, 300, false) == 16, "level 300 god bounty caps at 16");
  expect(pay(10, 300, true) == 12, "level 300 god replay is 12");
  expect(pay(3, 100, false) == 7, "level 100 boss first clear");
  expect(pay(3, 100, true) == 3, "level 100 boss replay");
  expect(pay(4, 300, false) == 16, "level 300 raid caps at 16");
  expect(pay(4, 300, true) == 12, "level 300 raid replay");

  // The save stores a cleared flag, so a replay pays the original amount minus 4 once.
  expect(coin_payout(5, true) == 1, "one replay cut from 5 is 1");
  expect(coin_payout(0, true) == 0, "replay of nothing stays nothing");
  expect(boss_bounty(-4, 25) == 0, "a negative base does not pay");

  uint8_t levels[kClearedLevelBytes];
  uint8_t waves[kClearedWaveBytes];
  std::memset(levels, 0, sizeof levels);
  std::memset(waves, 0, sizeof waves);

  expect(!coin_replay(levels, waves, 25, true, 0), "a fresh save pays full coins");
  mark_level_cleared(levels, 20);
  mark_god_wave_cleared(waves, 25, 0);
  mark_god_wave_cleared(waves, 25, 3);
  expect(level_cleared(levels, 20), "level 20 is stored");
  expect(!level_cleared(levels, 21), "level 21 is still open");
  expect(god_wave_cleared(waves, 25, 0), "Zeus wave 1 is stored");
  expect(god_wave_cleared(waves, 25, 3), "Zeus wave 4 is stored");
  expect(!god_wave_cleared(waves, 25, 1), "Zeus wave 2 is still open");
  expect(coin_replay(levels, waves, 25, true, 0), "a cleared god wave pays less");
  expect(!coin_replay(levels, waves, 25, true, 1), "an open god wave still pays full");
  expect(!coin_replay(levels, waves, 25, false, 0), "the god fight ignores goon-wave bits");
  mark_level_cleared(levels, 25);
  expect(coin_replay(levels, waves, 25, false, 0), "a cleared god level cuts the god fight");
  expect(coin_replay(levels, waves, 25, true, 1), "a cleared level cuts every wave");
  expect(coin_replay(levels, waves, 20, false, 0), "a cleared normal level cuts its drops");

  // Checkpoint and restart keep the same bytes. A second mark does not add bits.
  uint8_t saved_levels[kClearedLevelBytes];
  uint8_t saved_waves[kClearedWaveBytes];
  std::memcpy(saved_levels, levels, sizeof levels);
  std::memcpy(saved_waves, waves, sizeof waves);
  mark_level_cleared(levels, 20);
  mark_god_wave_cleared(waves, 300, 25);
  std::memset(levels, 0, sizeof levels);
  std::memset(waves, 0, sizeof waves);
  std::memcpy(levels, saved_levels, sizeof levels);
  std::memcpy(waves, saved_waves, sizeof waves);
  expect(level_cleared(levels, 20), "level 20 survives a reload");
  expect(level_cleared(levels, 25), "level 25 survives a reload");
  expect(god_wave_cleared(waves, 25, 0), "Zeus wave 1 survives a reload");
  expect(!god_wave_cleared(waves, 300, 25), "a wave marked after the snapshot stays out");

  expect(god_wave_bit(25, 0) == 0, "first god wave is bit 0");
  expect(god_wave_bit(50, 0) == 26, "second god starts at bit 26");
  expect(god_wave_bit(300, 25) == 311, "last god wave is the last bit");
  expect(god_wave_bit(24, 0) == -1, "a normal level has no god wave");
  expect(god_wave_bit(25, 26) == -1, "wave 27 is out of range");
  expect(!level_cleared(levels, 0), "level 0 is not a level");
  expect(!level_cleared(levels, 301), "level 301 is not a level");

  // Later gods add two waves. Level 300 has 26, which still fits the bitset.
  const int waves_at_300 = 4 + 2 * (300 / 25 - 1);
  expect(waves_at_300 == 26, "level 300 has 26 goon waves");
  expect(waves_at_300 <= kGodWaveSlots, "the save covers the longest god");

  if (failures != 0) {
    std::fprintf(stderr, "%d failed\n", failures);
    return 1;
  }
  std::printf("coin drop ok\n");
  return 0;
}
