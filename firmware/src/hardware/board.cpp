#include "board.h"

#include "hardware_config.h"
#include "resistive_touch.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

namespace {

// ---- Forge data ----------------------------------------------------------

// A gun attribute changes how a forged gun shoots. The first entry in each
// gun's table is its signature trait and therefore the most expensive.
enum AttrEffect : uint8_t {
  ATTR_RAPID,
  ATTR_POWER,
  ATTR_WIDE,
  ATTR_TWIN,
  ATTR_PIERCE,
  ATTR_SPREAD,
  ATTR_HOMING,
  ATTR_WAVE,
  ATTR_BURST,
  ATTR_COOL
};

struct AttrDef {
  const char* name;
  uint8_t effect;
};

const AttrDef kGunAttrs[9][5] = {
    {{"HEAVY", ATTR_POWER}, {"RAPID", ATTR_RAPID}, {"WIDE", ATTR_WIDE}, {"TWIN", ATTR_TWIN}, {"PIERCE", ATTR_PIERCE}},
    {{"SPREAD", ATTR_SPREAD}, {"TWIN", ATTR_TWIN}, {"RAPID", ATTR_RAPID}, {"WIDE", ATTR_WIDE}, {"PIERCE", ATTR_PIERCE}},
    {{"RAPID", ATTR_RAPID}, {"POWER", ATTR_POWER}, {"WIDE", ATTR_WIDE}, {"TWIN", ATTR_TWIN}, {"PIERCE", ATTR_PIERCE}},
    {{"BURST", ATTR_BURST}, {"COOL", ATTR_COOL}, {"WIDE", ATTR_WIDE}, {"POWER", ATTR_POWER}, {"PIERCE", ATTR_PIERCE}},
    {{"TWIN", ATTR_TWIN}, {"RAPID", ATTR_RAPID}, {"WIDE", ATTR_WIDE}, {"POWER", ATTR_POWER}, {"PIERCE", ATTR_PIERCE}},
    {{"WAVE", ATTR_WAVE}, {"RAPID", ATTR_RAPID}, {"WIDE", ATTR_WIDE}, {"TWIN", ATTR_TWIN}, {"PIERCE", ATTR_PIERCE}},
    {{"POWER", ATTR_POWER}, {"PIERCE", ATTR_PIERCE}, {"RAPID", ATTR_RAPID}, {"WIDE", ATTR_WIDE}, {"TWIN", ATTR_TWIN}},
    {{"SPREAD", ATTR_SPREAD}, {"TWIN", ATTR_TWIN}, {"RAPID", ATTR_RAPID}, {"WIDE", ATTR_WIDE}, {"PIERCE", ATTR_PIERCE}},
    {{"HOMING", ATTR_HOMING}, {"RAPID", ATTR_RAPID}, {"TWIN", ATTR_TWIN}, {"POWER", ATTR_POWER}, {"PIERCE", ATTR_PIERCE}},
};

// Power-up and upgrade forges create permanent perks instead of a gun.
enum Perk : uint32_t {
  PERK_SHIELD_CAP = 1u << 0,
  PERK_SHIELD_START = 1u << 1,
  PERK_SHIELD_REPAIR = 1u << 2,
  PERK_SHIELD_GLOW = 1u << 3,
  PERK_INVULN = 1u << 4,
  PERK_LIFE = 1u << 5,
  PERK_MAXHP = 1u << 6,
  PERK_HEAL = 1u << 7,
  PERK_REVIVE = 1u << 8,
  PERK_DAMAGE = 1u << 9,
  PERK_RAPID = 1u << 10,
  PERK_WIDE = 1u << 11,
  PERK_TWIN = 1u << 12,
  PERK_PIERCE = 1u << 13,
  PERK_SPEED = 1u << 14,
  PERK_SALVO = 1u << 15,
  PERK_COOL = 1u << 16,
  PERK_BURST = 1u << 17
};

struct PerkDef {
  const char* name;
  uint32_t perk;
};

const PerkDef kPowerAttrs[2][5] = {
    {{"THICK", PERK_SHIELD_CAP}, {"AEGIS", PERK_SHIELD_START}, {"REPAIR", PERK_SHIELD_REPAIR}, {"GLOW", PERK_SHIELD_GLOW}, {"LONGER", PERK_INVULN}},
    {{"VITAL", PERK_LIFE}, {"MAXHP", PERK_MAXHP}, {"HEAL", PERK_HEAL}, {"REVIVE", PERK_REVIVE}, {"ARMOR", PERK_INVULN}},
};

const PerkDef kUpgradeAttrs[2][5] = {
    {{"DAMAGE", PERK_DAMAGE}, {"RAPID", PERK_RAPID}, {"WIDE", PERK_WIDE}, {"TWIN", PERK_TWIN}, {"PIERCE", PERK_PIERCE}},
    {{"SPEED", PERK_SPEED}, {"SALVO", PERK_SALVO}, {"COOL", PERK_COOL}, {"BURST", PERK_BURST}, {"RAPID", PERK_RAPID}},
};

// A finger only counts when it lands inside the rectangle that was just drawn.
class PanelDisplay : public lgfx::LGFX_Device, public Display {
 public:
  PanelDisplay() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = kSpiWriteHz;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = kPinSclk;
      cfg.pin_mosi = kPinMosi;
      cfg.pin_miso = kPinMiso;
      cfg.pin_dc = kPinDc;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = kPinCs;
      cfg.pin_rst = kPinRst;
      cfg.pin_busy = -1;
      cfg.memory_width = kPanelWidth;
      cfg.memory_height = kPanelHeight;
      cfg.panel_width = kPanelWidth;
      cfg.panel_height = kPanelHeight;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = kPanelInvert;
      cfg.rgb_order = kPanelRgbOrder;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = kPinBl;
      cfg.invert = kBacklightActiveLow;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }

  bool begin() {
    ready_ = init();
    if (!ready_) {
      return false;
    }
    setRotation(kPanelRotation);
    setBrightness(255);
    touch_begin();
    prefs_.begin("jitters", false);
    theme_ = prefs_.getUChar("theme", 0) & 7;
    loadGear();
    loadShop();
    loadCampaign();
    makeStars();
    rng_ = esp_random();
    if (rng_ == 0) rng_ = 1;
    fillScreen(palette().bg);
    beginDemo();
    Serial.printf("FRAME game_ms=%lu\n", static_cast<unsigned long>(kStepMs));
    Serial.flush();
    return true;
  }

  void tick() override {
    if (!ready_) return;
    const uint32_t now = millis();
    if (step_at_ != 0 && static_cast<int32_t>(now - step_at_) < static_cast<int32_t>(kStepMs)) {
      return;
    }
    step_at_ = now;
    int tx = 0;
    int ty = 0;
    TouchRaw raw;
    const bool down = touch_read(raw, tx, ty);
    const char* where = controlAt(tx, ty);
    const bool on = down && std::strcmp(where, "none") != 0;
    touch_log(raw, tx, ty, where, on);
    const bool rose = down && !touch_down_;
    touch_down_ = down;

    if (mode_ == Mode::Splash) {
      if (static_cast<int32_t>(now - splash_at_) >= 0) startGodFight();
      return;
    }
    if (!demo_ && (mode_ == Mode::Play || mode_ == Mode::Advance) && rose && on &&
        std::strcmp(where, "BACK") == 0) {
      beginPause();
      return;
    }
    if (demo_) {
      // The cabinet flies itself until someone taps PLAY.
      if (mode_ != Mode::Splash && on && std::strcmp(where, "PLAY") == 0) {
        beginMenu();
        return;
      }
      if (mode_ == Mode::Advance) {
        if (static_cast<int32_t>(now - advance_at_) >= 0) {
          level_ += 1;
          if (level_ > kLevels) level_ = 1;
          startLevel();
        }
        return;
      }
      stepDemo(now);
      return;
    }
    if (mode_ == Mode::Advance) {
      if (static_cast<int32_t>(now - advance_at_) >= 0) {
        level_ += 1;
        startLevel();
      }
      return;
    }
    if (mode_ == Mode::Forge) {
      if (!rose || !on) return;
      if (std::strcmp(where, "FBACK") == 0) {
        forgeBack();
      } else if (std::strcmp(where, "FPAGE") == 0) {
        const int count = forgeItemCount(forge_tab_);
        const int pages = (count + kShopRows - 1) / kShopRows;
        forge_page_ = (forge_page_ + 1) % pages;
        paintForge();
      } else if (std::strncmp(where, "FTAB", 4) == 0) {
        forge_tab_ = where[4] - '0';
        forge_step_ = 0;
        forge_page_ = 0;
        forge_first_ = 0;
        forge_second_ = 0;
        forge_msg_ = "";
        paintForge();
      } else if (std::strncmp(where, "FROW", 4) == 0) {
        forgePick(where[4] - '0');
      } else if (std::strcmp(where, "FYES") == 0) {
        forgeConfirm();
      } else if (std::strcmp(where, "FNO") == 0) {
        forge_step_ = 3;
        forge_msg_ = "";
        paintForge();
      } else if (std::strcmp(where, "FUPG") == 0) {
        forgeUpgradeGun();
      } else if (std::strcmp(where, "FEQUIP") == 0) {
        if (forge_tab_ == 0 && forgeAnyValid()) {
          gun_equip_ = static_cast<uint8_t>(activeForgeGun());
          saveGear();
        }
        beginMenu();
      }
      return;
    }
    if (mode_ == Mode::Divine) {
      if (!rose || !on) return;
      if (std::strcmp(where, "BACK") == 0) beginMenu();
      else if (std::strcmp(where, "PAGE") == 0) {
        div_page_ ^= 1;
        paintDivine();
      } else if (std::strncmp(where, "ROW", 3) == 0) {
        divineToggle(div_page_ * kShopRows + (where[3] - '0'));
      }
      return;
    }
    if (mode_ == Mode::Shop) {
      if (!rose || !on) return;
      if (std::strcmp(where, "BACK") == 0) {
        beginMenu();
      } else if (std::strcmp(where, "PAGE") == 0) {
        const int pages = (shopCount(shop_tab_) + kShopRows - 1) / kShopRows;
        shop_page_ = (shop_page_ + 1) % pages;
        paintShop();
      } else if (std::strncmp(where, "TAB", 3) == 0) {
        shop_tab_ = where[3] - '0';
        shop_page_ = 0;
        shop_msg_ = "";
        paintShop();
      } else if (std::strncmp(where, "ROW", 3) == 0) {
        shopAct(shop_page_ * kShopRows + (where[3] - '0'));
      }
      return;
    }
    if (mode_ == Mode::Menu) {
      if (!rose || !on) return;
      if (std::strcmp(where, "PLAY") == 0) beginCampaign();
      else if (std::strcmp(where, "CHECK") == 0) beginChecks();
      else if (std::strcmp(where, "CUSTOM") == 0) beginCustom(false);
      else if (std::strcmp(where, "SHOP") == 0) beginShop();
      else if (std::strcmp(where, "FORGE") == 0) beginForge();
      else if (std::strcmp(where, "DIVINE") == 0) beginDivine();
      else if (std::strcmp(where, "LEAVE") == 0) beginDemo();
      else if (std::strcmp(where, "THEME") == 0) {
        do {
          theme_ = static_cast<uint8_t>((theme_ + 1) & 7);
        } while (!themeOwned(theme_));
        prefs_.putUChar("theme", theme_);
        beginMenu();
      } else if (std::strcmp(where, "RESTART") == 0) beginConfirm();
      return;
    }
    if (mode_ == Mode::Check) {
      if (!rose || !on) return;
      if (std::strcmp(where, "BACK") == 0) beginMenu();
      else if (std::strncmp(where, "CP", 2) == 0) {
        int index = where[2] >= '0' && where[2] <= '9' ? where[2] - '0' : 0;
        if (where[3] >= '0' && where[3] <= '9') index = index * 10 + (where[3] - '0');
        if (index >= 0 && index < 15 && (camp_ & (1u << index)) != 0) {
          level_ = (index + 1) * 20;
          beginCampaign();
        }
      }
      return;
    }
    if (mode_ == Mode::Confirm) {
      if (!rose || !on) return;
      if (std::strcmp(where, "YES") == 0) restartCampaign();
      else if (std::strcmp(where, "NO") == 0) beginMenu();
      return;
    }
    if (mode_ == Mode::Pause) {
      if (!rose || !on) return;
      if (std::strcmp(where, "RESUME") == 0) resumePlay();
      else if (std::strcmp(where, "CUSTOM") == 0) beginCustom(true);
      else if (std::strcmp(where, "HOME") == 0) beginMenu();
      return;
    }
    if (mode_ == Mode::Custom) {
      if (!rose || !on) return;
      if (std::strcmp(where, "COLOR") == 0) {
        do {
          laser_color_ = static_cast<uint8_t>((laser_color_ + 1) % 12);
        } while (!laserOwned(laser_color_));
        saveGear();
        paintCustom();
      } else if (std::strcmp(where, "SHIP") == 0) {
        do {
          ship_model_ = static_cast<uint8_t>((ship_model_ + 1) % kShipCount);
        } while (!shipOwned(ship_model_));
        saveGear();
        paintCustom();
      } else if (std::strcmp(where, "CPAGE") == 0) {
        custom_page_ ^= 1;
        paintCustom();
      } else if (std::strcmp(where, "BACK") == 0) {
        leaveCustom();
      } else if (std::strncmp(where, "CROW", 4) == 0) {
        const int r = where[4] - '0';
        if (r >= 0 && r < kGunCount) {
          equipGun(custom_page_ == 0 ? r : (kGunCount + r));
        }
      }
      return;
    }
    if (mode_ == Mode::Over) {
      if (rose && on && std::strcmp(where, "HOME") == 0) beginMenu();
      return;
    }
    if (mode_ == Mode::Play) stepGame(down, rose, tx, ty);
  }

 private:
  enum class Mode : uint8_t {
    Demo, Menu, Check, Confirm, Play, Advance, Over, Pause, Custom, Splash, Shop, Forge, Divine
  };

  struct Box {
    int x;
    int y;
    int w;
    int h;
  };
  struct Palette {
    uint16_t bg;
    uint16_t ink;
    uint16_t muted;
    uint16_t accent;
    uint16_t top;
    uint16_t low;
    uint16_t ship;
    uint16_t barrier;
    uint16_t shot;
    uint16_t planet;
    uint16_t button;
    uint16_t buttonOn;
    uint16_t star;
  };
  struct Unit {
    int16_t ox;
    int16_t oy;
    uint8_t kind;
    uint8_t hp;
    uint8_t stun;
    bool alive;
  };
  struct Barrier {
    int x;
    int y;
    uint8_t col[10];
    bool on;
  };
  struct Coin {
    int16_t x = 0;
    int16_t y = 0;
    bool on = false;
  };
  struct Star {
    int16_t x;
    int16_t y;
    uint8_t layer;
  };
  struct Bolt {
    int16_t x = 0;
    int16_t y = 0;
    int8_t vx = 0;
    int8_t vy = -8;
    uint8_t w = 3;
    uint8_t h = 8;
    uint8_t tag = 0;
    uint8_t pierce = 0;
    int8_t last = -1;
    bool on = false;
  };
  struct FoeBolt {
    int16_t x = 0;
    int16_t y = 0;
    int8_t vx = 0;
    int8_t vy = 6;
    uint8_t kind = 0;
    uint8_t w = 3;
    uint8_t h = 8;
    bool on = false;
  };
  struct Scrap {
    int16_t x = 0;
    int16_t y = 0;
    uint8_t w = 8;
    uint8_t h = 6;
  };
  struct ForgedGun {
    uint8_t valid = 0;
    uint8_t base = 0;
    uint8_t donor = 0;
    uint8_t attr_a = 0;
    uint8_t attr_b = 0;
  };
  static constexpr uint32_t kStepMs = 16;
  static constexpr int kLevels = 300;
  static constexpr int kMaxUnits = 40;
  static constexpr int kBolts = 6;
  static constexpr int kFoeBolts = 10;
  static constexpr int kBlastR = 20;
  static constexpr int kGunCount = 9;
  static constexpr int kMaxForged = 9;
  static constexpr int kGunSlots = kGunCount + kMaxForged;
  static constexpr int kGunMaxLv = 5;
  static constexpr int kBoosts = 5;
  static constexpr int kGods = 12;
  static constexpr int kAttrCount = 5;
  static constexpr int kScraps = 6;
  static constexpr int kDroneW = 16;
  static constexpr int kDroneH = 12;
  static constexpr int kStars = 42;
  static constexpr int kMaxBarriers = 3;
  static constexpr int kBarCols = 10;
  static constexpr int kButtonY = 430;
  static constexpr int kShipY = 368;
  static constexpr int kShipW = 32;
  static constexpr int kShipH = 16;
  static constexpr int kAlienW = 18;
  static constexpr int kAlienH = 14;
  static constexpr int kBossW = 44;
  static constexpr int kBossH = 30;
  static constexpr int kMiniW = 30;
  static constexpr int kMiniH = 20;
  static constexpr int kAllyW = 18;
  static constexpr int kAllyH = 16;
  static constexpr int kShotW = 3;
  static constexpr int kShotH = 8;
  static constexpr int kShipStep = 8;
  static constexpr int kCoins = 16;
  static constexpr int kCoinW = 11;
  static constexpr int kShopRows = 6;
  static constexpr int kShipCount = 6;
  static constexpr int kShopTabs = 6;
  static constexpr uint16_t kThemePrice[8] = {0, 0, 0, 0, 60, 80, 100, 120};
  static constexpr uint16_t kShipPrice[kShipCount] = {0, 40, 60, 80, 100, 120};


  void clampForged(ForgedGun& g) {
    if (g.valid == 0) {
      g = {};
      return;
    }
    g.valid = 1;
    if (g.base >= kGunCount) g.base = 0;
    if (g.donor >= kGunCount) g.donor = 0;
    if (g.attr_a >= kAttrCount) g.attr_a = 0;
    if (g.attr_b >= kAttrCount) g.attr_b = 0;
  }

  void loadGear() {
    gun_unlock_ = prefs_.getUChar("guns", 0);
    gun_equip_ = prefs_.getUChar("gun", 0);
    laser_color_ = static_cast<uint8_t>(prefs_.getUChar("laser", 0) % 12);
    ship_model_ = static_cast<uint8_t>(prefs_.getUChar("ship", 0) % kShipCount);
    for (int i = 0; i < kMaxForged; ++i) forged_[i] = {};
    uint8_t blob[kMaxForged * 5] = {};
    if (prefs_.getBytes("forges", blob, sizeof blob) == sizeof blob) {
      for (int i = 0; i < kMaxForged; ++i) {
        ForgedGun& g = forged_[i];
        g.valid = blob[i * 5] != 0 ? 1 : 0;
        g.base = blob[i * 5 + 1];
        g.donor = blob[i * 5 + 2];
        g.attr_a = blob[i * 5 + 3];
        g.attr_b = blob[i * 5 + 4];
        clampForged(g);
      }
    } else {
      // Older builds kept one 5-byte forge. Keep that weapon as the first slot.
      uint8_t forge[5] = {};
      if (prefs_.getBytes("forge", forge, sizeof forge) == sizeof forge && forge[0] != 0) {
        forged_[0].valid = 1;
        forged_[0].base = forge[1];
        forged_[0].donor = forge[2];
        forged_[0].attr_a = forge[3];
        forged_[0].attr_b = forge[4];
        clampForged(forged_[0]);
      }
    }
    for (int i = 0; i < kGunSlots; ++i) gun_lv_[i] = 0;
    // Nine bytes: base guns only. Ten: those plus the single old forge level.
    const size_t ln = prefs_.getBytes("glv", gun_lv_, sizeof gun_lv_);
    if (ln != 9 && ln != 10 && ln != sizeof gun_lv_) {
      for (int i = 0; i < kGunSlots; ++i) gun_lv_[i] = 0;
    }
    for (int i = 0; i < kGunSlots; ++i) {
      if (gun_lv_[i] > kGunMaxLv) gun_lv_[i] = kGunMaxLv;
    }
    if (gun_equip_ >= kGunSlots || !gunUnlocked(gun_equip_)) gun_equip_ = 0;
  }

  void saveGear() {
    prefs_.putUChar("guns", gun_unlock_);
    prefs_.putUChar("gun", gun_equip_);
    prefs_.putUChar("laser", laser_color_);
    prefs_.putUChar("ship", ship_model_);
    uint8_t blob[kMaxForged * 5] = {};
    for (int i = 0; i < kMaxForged; ++i) {
      const ForgedGun& g = forged_[i];
      blob[i * 5] = g.valid ? 1 : 0;
      blob[i * 5 + 1] = g.base;
      blob[i * 5 + 2] = g.donor;
      blob[i * 5 + 3] = g.attr_a;
      blob[i * 5 + 4] = g.attr_b;
    }
    prefs_.putBytes("forges", blob, sizeof blob);
    uint8_t forge[5] = {forged_[0].valid, forged_[0].base, forged_[0].donor, forged_[0].attr_a,
                        forged_[0].attr_b};
    prefs_.putBytes("forge", forge, sizeof forge);
    prefs_.putBytes("glv", gun_lv_, sizeof gun_lv_);
  }

  bool isForgedId(int gun) const { return gun >= kGunCount && gun < kGunSlots; }

  const ForgedGun* forgedAt(int gun) const {
    if (!isForgedId(gun)) return nullptr;
    const ForgedGun& g = forged_[gun - kGunCount];
    return g.valid ? &g : nullptr;
  }

  const ForgedGun* equippedForged() const { return forgedAt(gun_equip_); }

  bool forgeAnyValid() const {
    for (int i = 0; i < kMaxForged; ++i) {
      if (forged_[i].valid) return true;
    }
    return false;
  }

  int firstEmptyForge() const {
    for (int i = 0; i < kMaxForged; ++i) {
      if (!forged_[i].valid) return i;
    }
    return -1;
  }

  int activeForgeGun() const {
    if (forge_focus_ >= 0 && forge_focus_ < kMaxForged && forged_[forge_focus_].valid) {
      return kGunCount + forge_focus_;
    }
    if (equippedForged() != nullptr) return gun_equip_;
    for (int i = kMaxForged - 1; i >= 0; --i) {
      if (forged_[i].valid) return kGunCount + i;
    }
    return kGunCount;
  }

  int shopGunAt(int idx) const {
    if (idx < kGunCount) return idx;
    int seen = kGunCount;
    for (int i = 0; i < kMaxForged; ++i) {
      if (!forged_[i].valid) continue;
      if (seen == idx) return kGunCount + i;
      seen += 1;
    }
    return 0;
  }

  int shopGunCount() const {
    int n = kGunCount;
    for (int i = 0; i < kMaxForged; ++i) {
      if (forged_[i].valid) n += 1;
    }
    return n;
  }

  // Upgrade rows are one per owned gun, then one per forged gun, then thrusters, then Multi.
  // A forged gun never shares a level slot with another weapon.
  int shopUpgradeCount() const {
    int n = 1;
    for (int g = 0; g < kGunCount; ++g) {
      if (gunUnlocked(g)) n += 1;
    }
    for (int i = 0; i < kMaxForged; ++i) {
      if (forged_[i].valid) n += 1;
    }
    return n + 1;
  }

  // -1 is thrusters, -2 is the Multi equip slot. Every other value is a gun id.
  int shopUpgradeGun(int idx) const {
    int seen = 0;
    for (int g = 0; g < kGunCount; ++g) {
      if (!gunUnlocked(g)) continue;
      if (seen == idx) return g;
      seen += 1;
    }
    for (int i = 0; i < kMaxForged; ++i) {
      if (!forged_[i].valid) continue;
      if (seen == idx) return kGunCount + i;
      seen += 1;
    }
    if (idx == seen) return -1;
    return -2;
  }

  void loadCampaign() {
    level_ = static_cast<int>(prefs_.getUShort("lv", 1));
    if (level_ < 1) level_ = 1;
    if (level_ > kLevels) level_ = kLevels;
    camp_ = prefs_.getUShort("cp", 0);
    raid_won_ = prefs_.getUChar("raidok", 0) != 0;
  }

  void saveCampaign() {
    if (demo_) return;
    prefs_.putUShort("lv", static_cast<uint16_t>(level_));
    prefs_.putUShort("cp", camp_);
    prefs_.putUChar("raidok", raid_won_ ? 1 : 0);
  }

  const char* laserName() const { return laserNameOf(laser_color_); }

  const char* laserNameOf(int c) const {
    switch (c % 12) {
      case 1:
        return "GOLD";
      case 2:
        return "GREEN";
      case 3:
        return "CYAN";
      case 4:
        return "WHITE";
      case 5:
        return "MAGENTA";
      case 6:
        return "ORANGE";
      case 7:
        return "BLUE";
      case 8:
        return "LIME";
      case 9:
        return "PINK";
      case 10:
        return "ICE";
      case 11:
        return "VIOLET";
      default:
        return "RED";
    }
  }

  uint16_t laserColor() const { return laserColorOf(laser_color_); }

  uint16_t laserColorOf(int c) const {
    switch (c % 12) {
      case 1:
        return color565(255, 210, 48);
      case 2:
        return color565(48, 230, 96);
      case 3:
        return color565(80, 210, 255);
      case 4:
        return color565(255, 255, 255);
      case 5:
        return color565(255, 80, 220);
      case 6:
        return color565(255, 140, 32);
      case 7:
        return color565(64, 96, 255);
      case 8:
        return color565(170, 255, 48);
      case 9:
        return color565(255, 140, 180);
      case 10:
        return color565(180, 255, 255);
      case 11:
        return color565(170, 80, 255);
      default:
        return color565(255, 64, 56);
    }
  }

  const char* gunToken(int gun) const {
    switch (gun) {
      case 1:
        return "SHOTGUN";
      case 2:
        return "ASSAULT";
      case 3:
        return "BEAM";
      case 4:
        return "TWIN";
      case 5:
        return "WAVE";
      case 6:
        return "RAIL";
      case 7:
        return "NOVA";
      case 8:
        return "HOMING";
      default:
        break;
    }
    if (isForgedId(gun)) {
      static char forged[24];
      const ForgedGun* g = forgedAt(gun);
      if (g == nullptr) return "FORGED";
      snprintf(forged, sizeof forged, "%s+%s", gunToken(g->base), gunToken(g->donor));
      return forged;
    }
    return "CANNON";
  }

  const char* shipNameOf(int s) const {
    switch (s % kShipCount) {
      case 1:
        return "INTERCEPT";
      case 2:
        return "VALKYRIE";
      case 3:
        return "PHANTOM";
      case 4:
        return "TITAN";
      case 5:
        return "SERAPH";
      default:
        return "STRIKER";
    }
  }

  const char* shipName() const { return shipNameOf(ship_model_); }

  int baseGun() const {
    const ForgedGun* g = equippedForged();
    return g != nullptr ? g->base : gun_equip_;
  }

  int nthOtherAttr(int chosen, int n) const {
    int seen = 0;
    for (int a = 0; a < kAttrCount; ++a) {
      if (a == chosen) continue;
      if (seen == n) return a;
      seen += 1;
    }
    return 0;
  }

  int forgedSlotCount() const {
    if (equippedForged() == nullptr) return 0;
    const int lv = gunLv(gun_equip_);
    return lv >= 5 ? 8 : (2 + lv);
  }

  void forgedSlotPair(int slot, int* gun, int* attr) const {
    const ForgedGun* g = equippedForged();
    if (g == nullptr) {
      *gun = 0;
      *attr = 0;
      return;
    }
    const bool donor = (slot & 1) != 0;
    *gun = donor ? g->donor : g->base;
    const int chosen = donor ? g->attr_b : g->attr_a;
    *attr = slot < 2 ? chosen : nthOtherAttr(chosen, (slot - 2) / 2);
  }

  const char* forgedLatestTrait() const {
    const int slots = forgedSlotCount();
    if (slots <= 0) return "";
    int gun = 0;
    int attr = 0;
    forgedSlotPair(slots - 1, &gun, &attr);
    return kGunAttrs[gun][attr].name;
  }

  int forgedCount(uint8_t effect) const {
    if (equippedForged() == nullptr) return 0;
    const int slots = forgedSlotCount();
    int count = 0;
    for (int s = 0; s < slots; ++s) {
      int gun = 0;
      int attr = 0;
      forgedSlotPair(s, &gun, &attr);
      if (kGunAttrs[gun][attr].effect == effect) count += 1;
    }
    return count;
  }

  bool forgedHas(uint8_t effect) const { return forgedCount(effect) > 0; }

  bool perk(uint32_t bit) const {
    return (forge_power_ & bit) != 0 || (forge_up_ & bit) != 0;
  }

  int playerDamage() const {
    int dmg = 1 + forgedCount(ATTR_POWER);
    if (perk(PERK_DAMAGE)) dmg += 1;
    if (boostOn(1)) dmg += 1;
    if (berserk_ticks_ > 0) dmg += 10;
    return dmg;
  }

  int shieldCap() const { return perk(PERK_SHIELD_CAP) ? 13 : 9; }

  int heartSlots() const {
    int slots = lives_ > 3 ? lives_ : 3;
    if (perk(PERK_MAXHP)) slots += 1;
    if (slots > 9) slots = 9;
    return slots;
  }

  int gunLv(int gun) const {
    if (gun < 0 || gun >= kGunSlots) return 0;
    return gun_lv_[gun] > kGunMaxLv ? kGunMaxLv : gun_lv_[gun];
  }

  bool gunUnlocked(int gun) const {
    if (isForgedId(gun)) return forgedAt(gun) != nullptr;
    if (gun <= 0) return true;
    if (gun >= kGunCount) return false;
    return (gun_unlock_ & (1u << (gun - 1))) != 0;
  }

  const char* gunState(int gun) const {
    if (!gunUnlocked(gun)) return "LOCKED";
    if (gun == gun_equip_) return "ON";
    return "READY";
  }

  void gunLine(int gun, char* buf, int n) const {
    if (!gunUnlocked(gun)) {
      snprintf(buf, n, "%s LOCKED", gunToken(gun));
      return;
    }
    snprintf(buf, n, "%s LV%d %s", gunToken(gun), gunLv(gun) + 1,
             gun == gun_equip_ ? "ON" : "READY");
  }

  void equipGun(int gun) {
    if (!gunUnlocked(gun)) {
      paintCustom();
      return;
    }
    gun_equip_ = static_cast<uint8_t>(gun);
    saveGear();
    paintCustom();
  }

  const char* themeName() const { return themeNameOf(theme_); }

  const char* themeNameOf(int t) const {
    switch (t & 7) {
      case 1:
        return "LIGHT";
      case 2:
        return "BLUE";
      case 3:
        return "AMBER";
      case 4:
        return "MIDNIGHT";
      case 5:
        return "MINT";
      case 6:
        return "ROSE";
      case 7:
        return "ARCADE";
      default:
        return "DARK NEON";
    }
  }

  Palette palette() const { return paletteOf(theme_); }

  Palette paletteOf(int t) const {
    switch (t & 7) {
      case 1:
        return {color565(248, 242, 226), color565(28, 22, 16),   color565(90, 68, 48),
                color565(16, 110, 42),    color565(196, 92, 8),   color565(12, 112, 36),
                color565(10, 84, 28),     color565(18, 122, 40),  color565(186, 78, 6),
                color565(28, 78, 168),    color565(255, 252, 244), color565(186, 214, 168),
                color565(70, 54, 36)};
      case 2:
        return {color565(6, 16, 40),    color565(220, 236, 255), color565(120, 150, 190),
                color565(80, 180, 255),  color565(255, 196, 64),  color565(64, 220, 255),
                color565(120, 255, 220), color565(40, 140, 220),  color565(255, 220, 80),
                color565(40, 90, 220),   color565(8, 28, 64),     color565(16, 64, 120),
                color565(180, 200, 230)};
      case 3:
        return {color565(28, 14, 4),     color565(255, 236, 210), color565(190, 140, 90),
                color565(255, 170, 40),   color565(255, 210, 60),  color565(255, 120, 40),
                color565(255, 196, 80),   color565(180, 90, 20),   color565(255, 240, 180),
                color565(120, 60, 20),    color565(48, 24, 8),     color565(120, 64, 16),
                color565(255, 200, 140)};
      case 4:
        return {color565(8, 8, 28),      color565(230, 230, 255), color565(120, 120, 170),
                color565(120, 110, 255), color565(90, 80, 220),   color565(70, 60, 190),
                color565(160, 200, 255), color565(60, 60, 140),   color565(255, 120, 255),
                color565(40, 40, 110),   color565(30, 28, 70),    color565(60, 56, 120),
                color565(200, 200, 255)};
      case 5:
        return {color565(226, 250, 240), color565(16, 60, 48),    color565(90, 140, 120),
                color565(20, 170, 120),  color565(40, 190, 140),  color565(30, 160, 120),
                color565(10, 110, 90),   color565(40, 140, 110),  color565(255, 90, 120),
                color565(160, 230, 200), color565(196, 240, 222), color565(120, 220, 180),
                color565(120, 200, 170)};
      case 6:
        return {color565(40, 8, 24),     color565(255, 226, 236), color565(180, 110, 140),
                color565(255, 80, 150),  color565(255, 110, 170), color565(230, 70, 140),
                color565(255, 200, 220), color565(170, 40, 100),  color565(255, 240, 120),
                color565(110, 30, 70),   color565(80, 16, 50),    color565(140, 30, 84),
                color565(255, 190, 220)};
      case 7:
        return {color565(0, 0, 0),       color565(255, 255, 0),   color565(120, 120, 0),
                color565(0, 255, 255),   color565(255, 0, 255),   color565(0, 200, 255),
                color565(0, 255, 255),   color565(255, 0, 0),     color565(255, 255, 255),
                color565(60, 0, 60),     color565(20, 20, 20),    color565(60, 60, 0),
                color565(255, 255, 255)};
      default:
        return {TFT_BLACK,
                TFT_WHITE,
                color565(150, 160, 150),
                color565(48, 230, 86),
                color565(255, 206, 32),
                color565(46, 228, 72),
                color565(36, 255, 96),
                color565(28, 186, 64),
                color565(255, 214, 48),
                color565(48, 116, 255),
                color565(10, 24, 16),
                color565(18, 78, 42),
                color565(210, 210, 220)};
    }
  }

  Box playBox() const { return {24, kButtonY, kPanelWidth - 48, 44}; }
  Box menuPlayBox() const { return {16, 60, 288, 48}; }
  Box menuCheckBox() const { return {16, 116, 288, 40}; }
  Box menuCustomBox() const { return {16, 164, 288, 40}; }
  Box menuShopBox() const { return {16, 212, 288, 40}; }
  Box menuThemeBox() const { return {16, 260, 288, 40}; }
  Box menuDivineBox() const { return {16, 308, 288, 40}; }
  Box menuForgeBox() const { return {16, 356, 288, 40}; }
  Box forgeTab(int t) const { return {8 + t * 102, 40, 96, 32}; }
  Box forgeRow(int r) const { return {16, 88 + r * 50, 288, 44}; }
  Box forgeAttrRow(int r) const { return {16, 100 + r * 50, 288, 44}; }
  Box forgePageBox() const { return {8, 388, 120, 28}; }
  Box forgeUpBox() const { return {136, 388, 176, 28}; }
  Box forgeYesBox() const { return {32, 280, 256, 46}; }
  Box forgeNoBox() const { return {32, 336, 256, 46}; }
  Box forgeRevealUpBox() const { return {32, 238, 256, 48}; }
  Box forgeEquipBox() const { return {32, 300, 256, 48}; }
  Box shopTab(int t) const { return {4 + t * 52, 40, 50, 34}; }
  Box shopRow(int r) const { return {16, 84 + r * 52, 288, 46}; }
  Box shopPageBox() const { return {8, 396, 120, 28}; }
  Box menuRestartBox() const { return {24, kButtonY, kPanelWidth - 48, 44}; }
  Box checkCell(int index) const {
    const int col = index % 4;
    const int row = index / 4;
    return {12 + col * 76, 48 + row * 70, 70, 56};
  }
  Box yesBox() const { return {32, 190, 256, 52}; }
  Box noBox() const { return {32, 260, 256, 52}; }
  Box colorBox() const { return {16, 46, 140, 36}; }
  Box shipBox() const { return {164, 46, 140, 36}; }
  Box gunBox(int slot) const { return {16, 88 + slot * 34, 288, 32}; }
  Box customForgeBox() const { return {198, 4, 114, 28}; }
  Box forgedListBox(int row) const { return {16, 48 + row * 40, 288, 36}; }
  Box pauseResumeBox() const { return {32, 176, 256, 46}; }
  Box pauseCustomBox() const { return {32, 232, 256, 46}; }
  Box pauseHomeBox() const { return {32, 288, 256, 46}; }
  Box homeBox() const { return {24, kButtonY, kPanelWidth - 48, 44}; }
  Box leftBox() const { return {8, kButtonY, 96, 44}; }
  Box fireBox() const { return {112, kButtonY, 96, 44}; }
  Box rightBox() const { return {216, kButtonY, 96, 44}; }
  Box backBox() const { return {4, 2, 70, 26}; }

  bool contains(const Box& box, int x, int y) const {
    return x >= box.x && y >= box.y && x < box.x + box.w && y < box.y + box.h;
  }

  const char* controlAt(int x, int y) const {
    if (demo_) return contains(playBox(), x, y) ? "PLAY" : "none";
    if (mode_ == Mode::Menu) {
      if (contains(backBox(), x, y)) return "LEAVE";
      if (contains(menuPlayBox(), x, y)) return "PLAY";
      if (contains(menuCheckBox(), x, y)) return "CHECK";
      if (contains(menuCustomBox(), x, y)) return "CUSTOM";
      if (contains(menuShopBox(), x, y)) return "SHOP";
      if (contains(menuThemeBox(), x, y)) return "THEME";
      if (contains(menuDivineBox(), x, y)) return "DIVINE";
      if (contains(menuForgeBox(), x, y)) return "FORGE";
      if (contains(menuRestartBox(), x, y)) return "RESTART";
      return "none";
    }
    if (mode_ == Mode::Divine) {
      if (contains(homeBox(), x, y)) return "BACK";
      if (contains(shopPageBox(), x, y)) return "PAGE";
      for (int r = 0; r < kShopRows; ++r) {
        if (contains(shopRow(r), x, y)) {
          static char row_token[8];
          snprintf(row_token, sizeof row_token, "ROW%d", r);
          return row_token;
        }
      }
      return "none";
    }
    if (mode_ == Mode::Forge) {
      if (forge_step_ == 0 || forge_step_ == 1) {
        if (contains(homeBox(), x, y)) return "FBACK";
        if (contains(forgePageBox(), x, y)) return "FPAGE";
        if (forgeAnyValid() && (forge_tab_ == 0 || forge_tab_ == 2) && contains(forgeUpBox(), x, y)) {
          return "FUPG";
        }
        for (int t = 0; t < 3; ++t) {
          if (contains(forgeTab(t), x, y)) {
            static char tab_token[8];
            snprintf(tab_token, sizeof tab_token, "FTAB%d", t);
            return tab_token;
          }
        }
        for (int r = 0; r < kShopRows; ++r) {
          if (contains(forgeRow(r), x, y)) {
            static char row_token[8];
            snprintf(row_token, sizeof row_token, "FROW%d", r);
            return row_token;
          }
        }
      } else if (forge_step_ == 2 || forge_step_ == 3) {
        if (contains(homeBox(), x, y)) return "FBACK";
        for (int r = 0; r < kAttrCount; ++r) {
          if (contains(forgeAttrRow(r), x, y)) {
            static char row_token[8];
            snprintf(row_token, sizeof row_token, "FROW%d", r);
            return row_token;
          }
        }
      } else if (forge_step_ == 4) {
        if (contains(forgeYesBox(), x, y)) return "FYES";
        if (contains(forgeNoBox(), x, y)) return "FNO";
      } else {
        if (forge_tab_ == 0 && forgeAnyValid() && gunLv(activeForgeGun()) < kGunMaxLv &&
            contains(forgeRevealUpBox(), x, y)) {
          return "FUPG";
        }
        if (contains(forgeEquipBox(), x, y)) return "FEQUIP";
        if (contains(homeBox(), x, y)) return "FBACK";
      }
      return "none";
    }
    if (mode_ == Mode::Shop) {
      if (contains(homeBox(), x, y)) return "BACK";
      if (contains(shopPageBox(), x, y)) return "PAGE";
      for (int t = 0; t < kShopTabs; ++t) {
        if (contains(shopTab(t), x, y)) {
          static char tab_token[8];
          snprintf(tab_token, sizeof tab_token, "TAB%d", t);
          return tab_token;
        }
      }
      for (int r = 0; r < kShopRows; ++r) {
        if (contains(shopRow(r), x, y)) {
          static char row_token[8];
          snprintf(row_token, sizeof row_token, "ROW%d", r);
          return row_token;
        }
      }
      return "none";
    }
    if (mode_ == Mode::Check) {
      if (contains(homeBox(), x, y)) return "BACK";
      for (int i = 0; i < 15; ++i) {
        if (contains(checkCell(i), x, y)) {
          static char token[6];
          snprintf(token, sizeof token, "CP%d", i);
          return token;
        }
      }
      return "none";
    }
    if (mode_ == Mode::Confirm) {
      if (contains(yesBox(), x, y)) return "YES";
      if (contains(noBox(), x, y)) return "NO";
      return "none";
    }
    if (mode_ == Mode::Play || mode_ == Mode::Advance) {
      if (contains(backBox(), x, y)) return "BACK";
    }
    if (mode_ == Mode::Play) {
      if (contains(leftBox(), x, y)) return "LEFT";
      if (contains(fireBox(), x, y)) return "FIRE";
      if (contains(rightBox(), x, y)) return "RIGHT";
      return "none";
    }
    if (mode_ == Mode::Over) return contains(homeBox(), x, y) ? "HOME" : "none";
    if (mode_ == Mode::Pause) {
      if (contains(pauseResumeBox(), x, y)) return "RESUME";
      if (contains(pauseCustomBox(), x, y)) return "CUSTOM";
      if (contains(pauseHomeBox(), x, y)) return "HOME";
      return "none";
    }
    if (mode_ == Mode::Custom) {
      if (custom_page_ == 0) {
        if (contains(customForgeBox(), x, y)) return "CPAGE";
        if (contains(colorBox(), x, y)) return "COLOR";
        if (contains(shipBox(), x, y)) return "SHIP";
        for (int gun = 0; gun < kGunCount; ++gun) {
          if (contains(gunBox(gun), x, y)) {
            static char row_token[8];
            snprintf(row_token, sizeof row_token, "CROW%d", gun);
            return row_token;
          }
        }
      } else {
        int row = 0;
        for (int i = 0; i < kMaxForged; ++i) {
          if (!forged_[i].valid) continue;
          if (contains(forgedListBox(row), x, y)) {
            static char row_token[8];
            snprintf(row_token, sizeof row_token, "CROW%d", i);
            return row_token;
          }
          row += 1;
        }
      }
      if (contains(homeBox(), x, y)) return custom_page_ == 0 ? "BACK" : "CPAGE";
      return "none";
    }
    return "none";
  }

  void logRegions() {
    if (regions_logged_) {
      return;
    }
    regions_logged_ = true;
    const Box boxes[] = {playBox(), menuPlayBox(), menuCheckBox(), menuCustomBox(), menuThemeBox(),
                         menuRestartBox(), leftBox(), fireBox(), rightBox(), backBox()};
    const char* names[] = {"PLAY", "MENU", "CHECK", "CUSTOM", "THEME",
                           "RESTART", "LEFT", "FIRE", "RIGHT", "BACK"};
    for (int i = 0; i < 10; ++i) {
      Serial.printf("TOUCH region %s %d %d %d %d\n", names[i], boxes[i].x, boxes[i].y, boxes[i].w,
                    boxes[i].h);
    }
    Serial.flush();
  }

  uint32_t nextRng() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_;
  }

  void makeStars() {
    uint32_t s = 0x51A7u;
    for (int i = 0; i < kStars; ++i) {
      s = s * 1664525u + 1013904223u;
      stars_[i].x = static_cast<int16_t>(s % kPanelWidth);
      s = s * 1664525u + 1013904223u;
      stars_[i].y = static_cast<int16_t>(28 + (s % 360));
      stars_[i].layer = static_cast<uint8_t>(i & 1);
    }
  }

  void fadeOut() {
    for (int step = 3; step >= 0; --step) {
      setBrightness(48 + step * 52);
      delay(12);
    }
  }

  void fadeIn() {
    for (int step = 0; step <= 4; ++step) {
      setBrightness(48 + step * 42);
      delay(12);
    }
    setBrightness(255);
  }

  void paintStars() {
    const Palette ink = palette();
    for (int i = 0; i < kStars; ++i) {
      drawPixel(stars_[i].x, stars_[i].y, stars_[i].layer ? ink.star : ink.muted);
    }
  }

  void paintPlanet() {
    const uint16_t ink = palette().planet;
    for (int i = 0; i < 12; ++i) {
      const int inset = (i * i) / 4;
      const int y = 404 + i;
      if (y >= kButtonY - 2) {
        break;
      }
      fillRect(inset, y, kPanelWidth - inset * 2, 1, ink);
    }
  }

  void repairBg(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
      return;
    }
    if (x < 0) {
      w += x;
      x = 0;
    }
    if (y < 0) {
      h += y;
      y = 0;
    }
    if (x >= kPanelWidth || y >= kPanelHeight) {
      return;
    }
    if (x + w > kPanelWidth) {
      w = kPanelWidth - x;
    }
    if (y + h > kPanelHeight) {
      h = kPanelHeight - y;
    }
    fillRect(x, y, w, h, palette().bg);
    const uint16_t ink = palette().star;
    for (int i = 0; i < kStars; ++i) {
      if (stars_[i].x >= x && stars_[i].x < x + w && stars_[i].y >= y && stars_[i].y < y + h &&
          !starBlocked(stars_[i].x, stars_[i].y)) {
        drawPixel(stars_[i].x, stars_[i].y, ink);
      }
    }
    if (terrain_ == 1 && y + h > 404 && y < kButtonY && mode_ == Mode::Play) {
      paintPlanet();
    }
  }

  void eraseShipAt(int x) {
    repairBg(shipClearX(x), shipClearY(), shipClearW(), shipClearH());
  }

  void repair(int x, int y, int w, int h) {
    repairBg(x, y, w, h);
    if (god_ != 0 && (mode_ == Mode::Play || mode_ == Mode::Advance)) {
      int cx = x;
      int cy = y;
      int cw = w;
      int ch = h;
      if (cx < 0) {
        cw += cx;
        cx = 0;
      }
      if (cy < 0) {
        ch += cy;
        cy = 0;
      }
      if (cx + cw > kPanelWidth) cw = kPanelWidth - cx;
      if (cy + ch > kPanelHeight) ch = kPanelHeight - cy;
      if (cw > 0 && ch > 0 && cy < kShipY && cy + ch > 36) {
        setClipRect(cx, cy, cw, ch);
        paintGodArena(cy < 160 && cy + ch > 138);
        clearClipRect();
      }
    }
    if (mode_ == Mode::Play || mode_ == Mode::Advance) {
      const int bx = shipClearX(player_x_);
      const int by = shipClearY();
      if (x < bx + shipClearW() && x + w > bx && y < by + shipClearH() && y + h > by) {
        drawShipAt(player_x_);
      }
    }
  }

  void uncover(int ox, int oy, int nx, int ny, int w, int h) {
    if (w <= 0 || h <= 0 || (ox == nx && oy == ny)) {
      return;
    }
    if (ox < nx) {
      repair(ox, oy, nx - ox, h);
    } else if (nx < ox) {
      repair(nx + w, oy, ox - nx, h);
    }
    if (oy < ny) {
      repair(ox, oy, w, ny - oy);
    } else if (ny < oy) {
      repair(ox, ny + h, w, oy - ny);
    }
  }


  void drawKey(const char* text, const Box& box, bool held) {
    const Palette ink = palette();
    fillRect(box.x, box.y, box.w, box.h, held ? ink.buttonOn : ink.button);
    drawRect(box.x, box.y, box.w, box.h, ink.accent);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString(text, box.x + box.w / 2, box.y + box.h / 2);
  }

  void drawCard(const char* title, const char* sub, const Box& box) {
    const Palette ink = palette();
    fillRect(box.x, box.y, box.w, box.h, ink.button);
    drawRect(box.x, box.y, box.w, box.h, ink.accent);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString(title, box.x + box.w / 2, box.y + box.h / 2 - (sub != nullptr ? 8 : 0));
    if (sub != nullptr) {
      setTextColor(ink.muted);
      drawString(sub, box.x + box.w / 2, box.y + box.h / 2 + 12);
    }
  }


  void drawHeart(int x, int y, uint16_t color) {
    fillCircle(x + 3, y + 3, 3, color);
    fillCircle(x + 8, y + 3, 3, color);
    fillTriangle(x, y + 4, x + 11, y + 4, x + 5, y + 10, color);
  }

  // Lives sit in the top-right as heart shapes. Lost lives stay on screen as
  // dim hearts so the count reads at a glance.
  void drawHearts() {
    const Palette ink = palette();
    const int slots = heartSlots();
    const int step = 14;
    int x = kPanelWidth - 8 - slots * step;
    if (x < 84) x = 84;
    for (int i = 0; i < slots; ++i) {
      drawHeart(x + i * step, 6, i < lives_ ? color565(255, 64, 96) : ink.button);
    }
  }

  // ---- Attract demo ------------------------------------------------------

  void beginDemo() {
    demo_ = true;
    // The attract loop borrows level_ for its own flights; the campaign level
    // stays in camp_level_ until PLAY hands the cabinet to the player.
    camp_level_ = level_;
    fadeOut();
    score_ = 0;
    level_ = 1 + static_cast<int>(millis() % 5);
    lives_ = 3;
    god_ = 0;
    god_wave_ = 0;
    god_entrance_ = false;
    raid_ = false;
    mini_ = false;
    invuln_ = 0;
    startLevel();
    fadeIn();
  }

  // A CPU pilot: chase the lowest live unit and hold FIRE once it is lined up.
  void stepDemo(uint32_t now) {
    (void)now;
    int best = -1;
    int best_dy = -1;
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int dy = origin_y_ + units_[i].oy;
      if (dy > best_dy) {
        best_dy = dy;
        best = i;
      }
    }
    if (best < 0) {
      stepGame(false, false, 0, 0);
      return;
    }
    const int want = origin_x_ + units_[best].ox + unitW(units_[best]) / 2;
    const int cx = player_x_ + kShipW / 2;
    if (want < cx - 4) {
      stepGame(true, false, leftBox().x + 4, leftBox().y + 4);
    } else if (want > cx + 4) {
      stepGame(true, false, rightBox().x + 4, rightBox().y + 4);
    } else {
      stepGame(true, false, fireBox().x + 4, fireBox().y + 4);
    }
  }

  // ---- Menu --------------------------------------------------------------

  void paintMenu() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("INVADERS", kPanelWidth / 2, 24);
    drawKey("BACK", backBox(), false);
    setFont(&fonts::Font2);
    setTextColor(ink.accent);
    drawString("MACHINA", kPanelWidth / 2, 48);
    setTextDatum(lgfx::middle_right);
    setTextColor(ink.accent);
    char coins[16];
    snprintf(coins, sizeof coins, "$%u", static_cast<unsigned>(bank_));
    drawString(coins, kPanelWidth - 12, 48);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawCard("PLAY", continueLine(), menuPlayBox());
    drawCard("CHECKPOINTS", checkpointLine(), menuCheckBox());
    drawCard("CUSTOM", "ship, guns, laser", menuCustomBox());
    drawCard("SHOP", "themes, guns, power-ups", menuShopBox());
    drawCard(themeName(), "tap to switch", menuThemeBox());
    drawCard("DIVINE MACHINES", divineMenuLine(), menuDivineBox());
    drawCard("FORGE", "combine attributes", menuForgeBox());
    drawKey("RESTART GAME", menuRestartBox(), false);
  }

  const char* continueLine() const {
    static char buf[20];
    snprintf(buf, sizeof buf, "level %d", level_);
    return buf;
  }

  const char* divineMenuLine() const {
    static char buf[20];
    int open = 0;
    for (int i = 0; i < kGods; ++i) {
      if ((div_unlock_ & (1u << i)) != 0) open += 1;
    }
    snprintf(buf, sizeof buf, "%d unlocked", open);
    return buf;
  }

  const char* checkpointLine() const {
    static char buf[20];
    int open = 0;
    for (int i = 0; i < 15; ++i) {
      if ((camp_ & (1u << i)) != 0) open += 1;
    }
    snprintf(buf, sizeof buf, "%d unlocked", open);
    return buf;
  }

  void beginMenu() {
    if (demo_) level_ = camp_level_;
    demo_ = false;
    clearBoosts();
    saveShop();
    fadeOut();
    mode_ = Mode::Menu;
    shot_ = false;
    clearFoe();
    paintMenu();
    fadeIn();
  }

  // ---- Coins -------------------------------------------------------------

  bool themeOwned(int t) const { return ((themes_owned_ >> (t & 7)) & 1) != 0; }
  bool shipOwned(int s) const { return ((ships_owned_ >> (s % kShipCount)) & 1) != 0; }
  bool laserOwned(int c) const { return ((lasers_owned_ >> (c % 12)) & 1) != 0; }

  void loadShop() {
    bank_ = prefs_.getUShort("coins", 0);
    themes_owned_ = static_cast<uint8_t>(prefs_.getUChar("themes", 0x0F) | 0x0F);
    ships_owned_ = static_cast<uint8_t>(prefs_.getUChar("ships", 0x01) | 0x01);
    lasers_owned_ = static_cast<uint16_t>(prefs_.getUShort("lasers", 0x0001) | 0x0001);
    shield_ = prefs_.getUChar("shield", 0);
    spare_ = prefs_.getUChar("spare", 0);
    thrust_ = prefs_.getUChar("thrust", 0);
    if (thrust_ > 2) thrust_ = 2;
    forge_power_ = prefs_.getUInt("fpw", 0);
    forge_up_ = prefs_.getUInt("fup", 0);
    div_unlock_ = prefs_.getUShort("gods", 0);
    div_equip_ = prefs_.getUShort("dive", 0);
    div_multi_ = prefs_.getUChar("dmul", 0);
    trimDivine();
    for (int i = 0; i < kBoosts; ++i) boost_armed_[i] = 0;
    uint16_t armed[kBoosts] = {};
    if (prefs_.getBytes("bst", armed, sizeof armed) == sizeof armed) {
      for (int i = 0; i < kBoosts; ++i) {
        if (armed[i] == boostLen(i)) boost_armed_[i] = armed[i];
      }
    }
    ship_step_ = kShipStep + thrust_ + (perk(PERK_SPEED) ? 1 : 0);
    if (!themeOwned(theme_)) theme_ = 0;
    if (!shipOwned(ship_model_)) ship_model_ = 0;
    if (!laserOwned(laser_color_)) laser_color_ = 0;
  }

  void saveShop() {
    if (demo_) return;
    prefs_.putUShort("coins", bank_);
    prefs_.putUChar("themes", themes_owned_);
    prefs_.putUChar("ships", ships_owned_);
    prefs_.putUShort("lasers", lasers_owned_);
    prefs_.putUChar("shield", shield_);
    prefs_.putUChar("spare", spare_);
    prefs_.putUChar("thrust", thrust_);
    prefs_.putUInt("fpw", forge_power_);
    prefs_.putUInt("fup", forge_up_);
    prefs_.putBytes("bst", boost_armed_, sizeof boost_armed_);
    prefs_.putUShort("gods", div_unlock_);
    prefs_.putUShort("dive", div_equip_);
    prefs_.putUChar("dmul", div_multi_);
  }

  void drawCoin(int x, int y) {
    fillCircle(x + kCoinW / 2, y + kCoinW / 2, kCoinW / 2, color565(196, 204, 214));
    fillCircle(x + kCoinW / 2, y + kCoinW / 2, kCoinW / 2 - 1, color565(222, 228, 236));
    fillRect(x + 3, y + 3, 3, 3, color565(255, 255, 255));
  }

  void spawnCoin(int x, int y) {
    if (demo_) return;
    if (x < 0) x = 0;
    if (x > kPanelWidth - kCoinW) x = kPanelWidth - kCoinW;
    for (int i = 0; i < kCoins; ++i) {
      if (coins_[i].on) continue;
      coins_[i].on = true;
      coins_[i].x = static_cast<int16_t>(x);
      coins_[i].y = static_cast<int16_t>(y);
      drawCoin(x, y);
      return;
    }
  }

  // A boss scatters its coins over the spot where it fell.
  void dropCoins(int x, int y, int n) {
    for (int i = 0; i < n; ++i) spawnCoin(x + (i - n / 2) * (kCoinW + 2), y);
  }

  void paintCoins() {
    for (int i = 0; i < kCoins; ++i) {
      if (coins_[i].on) drawCoin(coins_[i].x, coins_[i].y);
    }
  }

  void clearCoins() {
    for (int i = 0; i < kCoins; ++i) {
      if (!coins_[i].on) continue;
      repair(coins_[i].x, coins_[i].y, kCoinW, kCoinW);
      coins_[i].on = false;
    }
  }

  void stepCoins() {
    bool collected = false;
    for (int i = 0; i < kCoins; ++i) {
      Coin& coin = coins_[i];
      if (!coin.on) continue;
      const int ox = coin.x;
      const int oy = coin.y;
      const int ny = oy + 2;
      if (overlap(ox, ny, kCoinW, kCoinW, player_x_, kShipY, kShipW, kShipH)) {
        repair(ox, oy, kCoinW, kCoinW);
        drawShipAt(player_x_);
        coin.on = false;
        if (bank_ < 65535) bank_ += 1;
        collected = true;
        continue;
      }
      if (ny >= kButtonY) {
        repair(ox, oy, kCoinW, kCoinW);
        coin.on = false;
        continue;
      }
      coin.y = static_cast<int16_t>(ny);
      drawCoin(ox, ny);
      uncover(ox, oy, ox, ny, kCoinW, kCoinW);
    }
    if (collected) drawHud();
  }

  // ---- Shop --------------------------------------------------------------

  int shopCount(int tab) const {
    switch (tab) {
      case 0:
        return 8;
      case 1:
        return kShipCount;
      case 2:
        return 12;
      case 3:
        return shopGunCount();
      case 4:
        return 2 + kBoosts;
      default:
        return shopUpgradeCount();
    }
  }

  int shopPrice(int tab, int idx) const {
    switch (tab) {
      case 0:
        return kThemePrice[idx];
      case 1:
        return kShipPrice[idx % kShipCount];
      case 2:
        return idx == 0 ? 0 : 15;
      case 3:
        return idx == 0 ? 0 : 90;
      case 4:
        if (idx <= 0) return 10;
        if (idx == 1) return 15;
        return boostPrice(idx - 2);
      default: {
        const int gun = shopUpgradeGun(idx);
        if (gun == -2) return 300;
        if (gun < 0) return 60 * (thrust_ + 1);
        return 50 * (gunLv(gun) + 1);
      }
    }
  }

  bool shopOwned(int tab, int idx) const {
    if (tab == 0) return themeOwned(idx);
    if (tab == 1) return shipOwned(idx);
    if (tab == 2) return laserOwned(idx);
    if (tab == 3) return gunUnlocked(shopGunAt(idx));
    return false;
  }

  bool shopActive(int idx) const {
    if (shop_tab_ == 0) return theme_ == idx;
    if (shop_tab_ == 1) return ship_model_ == idx;
    if (shop_tab_ == 2) return laser_color_ == idx;
    if (shop_tab_ == 3) return gun_equip_ == shopGunAt(idx);
    if (shop_tab_ == 4 && idx >= 2) return boost_armed_[idx - 2] > 0;
    if (shop_tab_ == 5) {
      const int gun = shopUpgradeGun(idx);
      return gun >= 0 && gun == gun_equip_;
    }
    return false;
  }

  const char* shopName(int tab, int idx) const {
    switch (tab) {
      case 0:
        return themeNameOf(idx);
      case 1:
        return shipNameOf(idx);
      case 2:
        return laserNameOf(idx);
      case 3:
        return gunToken(shopGunAt(idx));
      case 4:
        if (idx <= 0) return "SHIELD";
        if (idx == 1) return "SPARE HEART";
        return boostName(idx - 2);
      default: {
        const int gun = shopUpgradeGun(idx);
        if (gun == -2) return "MULTI";
        if (gun < 0) return "THRUSTERS";
        return gunToken(gun);
      }
    }
  }

  void shopStatus(int tab, int idx, char* buf, int n) const {
    if (tab <= 3) {
      if (shopActive(idx)) snprintf(buf, n, "ON");
      else if (shopOwned(tab, idx)) snprintf(buf, n, "OWNED");
      else snprintf(buf, n, "$%d", shopPrice(tab, idx));
      return;
    }
    if (tab == 4) {
      if (idx >= 2) {
        if (boost_armed_[idx - 2] > 0) snprintf(buf, n, "READY");
        else snprintf(buf, n, "$%d", shopPrice(tab, idx));
        return;
      }
      snprintf(buf, n, "$%d x%d", shopPrice(tab, idx), idx == 0 ? shield_ : spare_);
      return;
    }
    const int gun = shopUpgradeGun(idx);
    if (gun == -2) {
      if (divineSlots() >= kGods) snprintf(buf, n, "MAX");
      else snprintf(buf, n, "x%d $300", divineSlots());
      return;
    }
    if (gun < 0) {
      if (thrust_ >= 2) snprintf(buf, n, "MAX");
      else snprintf(buf, n, "LV%d $%d", static_cast<int>(thrust_), shopPrice(tab, idx));
      return;
    }
    if (gunLv(gun) >= kGunMaxLv) snprintf(buf, n, "MAX");
    else snprintf(buf, n, "LV%d $%d", gunLv(gun), shopPrice(tab, idx));
  }

  bool pay(int price) {
    if (bank_ < price) {
      shop_msg_ = "NOT ENOUGH COINS";
      return false;
    }
    bank_ = static_cast<uint16_t>(bank_ - price);
    return true;
  }

  void shopAct(int idx) {
    const int tab = shop_tab_;
    if (idx < 0 || idx >= shopCount(tab)) return;
    shop_msg_ = "";
    if (tab <= 3) {
      const int gun = tab == 3 ? shopGunAt(idx) : idx;
      if (!shopOwned(tab, idx)) {
        if (tab == 3 && !isForgedId(gun) && (gun <= 0 || gun >= kGunCount)) {
          paintShop();
          return;
        }
        if (tab == 3 && isForgedId(gun)) {
          paintShop();
          return;
        }
        if (!pay(shopPrice(tab, idx))) {
          paintShop();
          return;
        }
        if (tab == 0) themes_owned_ = static_cast<uint8_t>(themes_owned_ | (1u << idx));
        else if (tab == 1) ships_owned_ = static_cast<uint8_t>(ships_owned_ | (1u << idx));
        else if (tab == 2) lasers_owned_ = static_cast<uint16_t>(lasers_owned_ | (1u << idx));
        else gun_unlock_ = static_cast<uint8_t>(gun_unlock_ | (1u << (gun - 1)));
        shop_msg_ = "BOUGHT";
      }
      if (tab == 0) {
        theme_ = static_cast<uint8_t>(idx);
        prefs_.putUChar("theme", theme_);
      } else if (tab == 1) {
        ship_model_ = static_cast<uint8_t>(idx);
      } else if (tab == 2) {
        laser_color_ = static_cast<uint8_t>(idx);
      } else {
        gun_equip_ = static_cast<uint8_t>(gun);
      }
      saveGear();
      saveShop();
      paintShop();
      return;
    }
    if (tab == 4) {
      if (!pay(shopPrice(tab, idx))) {
        paintShop();
        return;
      }
      if (idx <= 0) {
        if (shield_ < shieldCap()) shield_ = static_cast<uint8_t>(shield_ + 1);
      } else if (idx == 1) {
        if (spare_ < 9) spare_ = static_cast<uint8_t>(spare_ + 1);
      } else {
        const int boost = idx - 2;
        boost_armed_[boost] = boostLen(boost);
      }
      shop_msg_ = "BOUGHT";
      saveShop();
      paintShop();
      return;
    }
    const int gun = shopUpgradeGun(idx);
    if (gun == -2) {
      if (divineSlots() >= kGods) {
        shop_msg_ = "MAXED";
        paintShop();
        return;
      }
      if (!pay(300)) {
        paintShop();
        return;
      }
      div_multi_ = static_cast<uint8_t>(div_multi_ + 1);
      shop_msg_ = "MULTI";
      saveShop();
      paintShop();
      return;
    }
    if (gun < 0) {
      if (thrust_ >= 2) {
        shop_msg_ = "MAXED";
        paintShop();
        return;
      }
    } else if (gunLv(gun) >= kGunMaxLv) {
      shop_msg_ = "MAXED";
      paintShop();
      return;
    }
    if (!pay(shopPrice(tab, idx))) {
      paintShop();
      return;
    }
    if (gun < 0) {
      thrust_ = static_cast<uint8_t>(thrust_ + 1);
      ship_step_ = kShipStep + thrust_ + (perk(PERK_SPEED) ? 1 : 0);
    } else {
      gun_lv_[gun] = static_cast<uint8_t>(gunLv(gun) + 1);
      saveGear();
    }
    shop_msg_ = "UPGRADED";
    saveShop();
    paintShop();
  }

  void paintShop() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(ink.ink);
    drawString("SHOP", 12, 18);
    setTextDatum(lgfx::middle_right);
    setTextColor(ink.accent);
    char coins[16];
    snprintf(coins, sizeof coins, "$%u", static_cast<unsigned>(bank_));
    drawString(coins, kPanelWidth - 12, 18);
    static const char* const kTabs[kShopTabs] = {"THEME", "SHIPS", "COLOR", "GUNS", "POWER", "UPGRD"};
    for (int t = 0; t < kShopTabs; ++t) {
      drawKey(kTabs[t], shopTab(t), t == shop_tab_);
    }
    const int count = shopCount(shop_tab_);
    const int pages = (count + kShopRows - 1) / kShopRows;
    if (shop_page_ >= pages) shop_page_ = 0;
    for (int row = 0; row < kShopRows; ++row) {
      const int idx = shop_page_ * kShopRows + row;
      if (idx >= count) break;
      const Box b = shopRow(row);
      fillRect(b.x, b.y, b.w, b.h, ink.button);
      drawRect(b.x, b.y, b.w, b.h, shopActive(idx) ? ink.accent : ink.button);
      if (shop_tab_ == 0) fillRect(b.x + 8, b.y + 12, 22, 22, paletteOf(idx).accent);
      if (shop_tab_ == 1) drawShipHull(b.x + 4, b.y + 15, idx);
      if (shop_tab_ == 2) fillRect(b.x + 8, b.y + 12, 22, 22, laserColorOf(idx));
      setTextDatum(lgfx::middle_left);
      setTextColor(ink.ink);
      drawString(shopName(shop_tab_, idx), b.x + (shop_tab_ == 1 ? 42 : 38), b.y + b.h / 2);
      char state[16];
      shopStatus(shop_tab_, idx, state, sizeof state);
      setTextDatum(lgfx::middle_right);
      setTextColor(ink.muted);
      drawString(state, b.x + b.w - 10, b.y + b.h / 2);
    }
    char page[32];
    snprintf(page, sizeof page, "PAGE %d/%d", shop_page_ + 1, pages);
    drawKey(page, shopPageBox(), false);
    setTextDatum(lgfx::middle_left);
    setTextColor(ink.accent);
    drawString(shop_msg_, 140, 410);
    drawKey("BACK", homeBox(), false);
  }

  void beginShop() {
    fadeOut();
    mode_ = Mode::Shop;
    shop_msg_ = "";
    paintShop();
    fadeIn();
  }

  // ---- Forge -------------------------------------------------------------

  int forgeItemCount(int tab) const { return tab == 0 ? kGunCount : 2; }

  const char* forgeItemName(int tab, int idx) const {
    if (tab == 0) return gunToken(idx);
    if (tab == 1) return idx == 0 ? "SHIELD" : "SPARE HEART";
    return idx == 0 ? "GUN POWER" : "THRUSTERS";
  }

  bool forgeItemOwned(int tab, int idx) const {
    if (tab == 0) return gunUnlocked(idx);
    return true;
  }

  const char* forgeAttrName(int tab, int item, int attr) const {
    if (attr < 0 || attr >= kAttrCount) return "";
    if (tab == 0) return kGunAttrs[item][attr].name;
    if (tab == 1) return kPowerAttrs[item][attr].name;
    return kUpgradeAttrs[item][attr].name;
  }

  int forgeAttrPrice(int attr) const { return (kAttrCount - attr) * 10; }

  int forgeTotal() const {
    return forgeAttrPrice(forge_attr_a_) + forgeAttrPrice(forge_attr_b_);
  }

  uint32_t forgePerk(int tab, int item, int attr) const {
    if (attr < 0 || attr >= kAttrCount) return 0;
    if (tab == 1) return kPowerAttrs[item][attr].perk;
    if (tab == 2) return kUpgradeAttrs[item][attr].perk;
    return 0;
  }

  const char* forgeRevealName() const {
    static char buf[32];
    if (forge_tab_ == 0) return gunToken(activeForgeGun());
    snprintf(buf, sizeof buf, "%s+%s", forgeAttrName(forge_tab_, forge_first_, forge_attr_a_),
             forgeAttrName(forge_tab_, forge_second_, forge_attr_b_));
    return buf;
  }

  void paintForge() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(ink.ink);
    drawString("FORGE", 12, 18);
    setTextDatum(lgfx::middle_right);
    setTextColor(ink.accent);
    char coins[16];
    snprintf(coins, sizeof coins, "$%u", static_cast<unsigned>(bank_));
    drawString(coins, kPanelWidth - 12, 18);

    if (forge_step_ == 0 || forge_step_ == 1) {
      static const char* const kTabs[3] = {"GUNS", "POWER", "UPGRADE"};
      for (int t = 0; t < 3; ++t) drawKey(kTabs[t], forgeTab(t), t == forge_tab_);
      setTextDatum(lgfx::middle_center);
      setTextColor(ink.muted);
      if (forge_step_ == 0) {
        drawString("PICK THE FIRST ITEM", kPanelWidth / 2, 78);
      } else {
        char line[40];
        snprintf(line, sizeof line, "FIRST: %s", forgeItemName(forge_tab_, forge_first_));
        drawString(line, kPanelWidth / 2, 78);
      }
      const int count = forgeItemCount(forge_tab_);
      const int pages = (count + kShopRows - 1) / kShopRows;
      if (forge_page_ >= pages) forge_page_ = 0;
      for (int row = 0; row < kShopRows; ++row) {
        const int idx = forge_page_ * kShopRows + row;
        if (idx >= count) break;
        const Box b = forgeRow(row);
        const bool first = forge_step_ == 1 && idx == forge_first_;
        fillRect(b.x, b.y, b.w, b.h, ink.button);
        drawRect(b.x, b.y, b.w, b.h, first ? ink.accent : ink.button);
        setTextDatum(lgfx::middle_left);
        setTextColor(ink.ink);
        drawString(forgeItemName(forge_tab_, idx), b.x + 12, b.y + b.h / 2);
        setTextDatum(lgfx::middle_right);
        setTextColor(ink.muted);
        const char* status = "PICK";
        if (forge_tab_ == 0 && !forgeItemOwned(0, idx)) status = "LOCKED";
        else if (first) status = "FIRST";
        drawString(status, b.x + b.w - 10, b.y + b.h / 2);
      }
      if (pages > 1) {
        char page[32];
        snprintf(page, sizeof page, "PAGE %d/%d", forge_page_ + 1, pages);
        drawKey(page, forgePageBox(), false);
      }
      if (forgeAnyValid() && (forge_tab_ == 0 || forge_tab_ == 2)) {
        char up[36];
        const int lv = gunLv(activeForgeGun());
        if (lv >= kGunMaxLv) snprintf(up, sizeof up, "FORGED MAX");
        else snprintf(up, sizeof up, "UPG LV%d $%d", lv + 1, 50 * (lv + 1));
        drawKey(up, forgeUpBox(), false);
      }
      if (forge_msg_[0] != '\0') {
        setTextDatum(lgfx::middle_left);
        setTextColor(ink.accent);
        drawString(forge_msg_, 12, 402);
      }
      drawKey("BACK", homeBox(), false);
      return;
    }

    if (forge_step_ == 2 || forge_step_ == 3) {
      const int item = forge_step_ == 2 ? forge_first_ : forge_second_;
      char line[48];
      setTextDatum(lgfx::middle_center);
      setTextColor(ink.muted);
      if (forge_step_ == 2) {
        snprintf(line, sizeof line, "FIRST: %s", forgeItemName(forge_tab_, item));
        drawString(line, kPanelWidth / 2, 56);
        setTextColor(ink.accent);
        drawString("PICK AN ATTRIBUTE", kPanelWidth / 2, 76);
      } else {
        snprintf(line, sizeof line, "FIRST: %s %s", forgeItemName(forge_tab_, forge_first_),
                 forgeAttrName(forge_tab_, forge_first_, forge_attr_a_));
        drawString(line, kPanelWidth / 2, 56);
        snprintf(line, sizeof line, "SECOND: %s", forgeItemName(forge_tab_, item));
        drawString(line, kPanelWidth / 2, 76);
      }
      for (int a = 0; a < kAttrCount; ++a) {
        const Box b = forgeAttrRow(a);
        fillRect(b.x, b.y, b.w, b.h, ink.button);
        drawRect(b.x, b.y, b.w, b.h, ink.button);
        setTextDatum(lgfx::middle_left);
        setTextColor(ink.ink);
        drawString(forgeAttrName(forge_tab_, item, a), b.x + 12, b.y + b.h / 2);
        setTextDatum(lgfx::middle_right);
        setTextColor(ink.accent);
        char price[16];
        snprintf(price, sizeof price, "$%d", forgeAttrPrice(a));
        drawString(price, b.x + b.w - 10, b.y + b.h / 2);
      }
      drawKey("BACK", homeBox(), false);
      return;
    }

    if (forge_step_ == 4) {
      setFont(&fonts::Font4);
      setTextDatum(lgfx::middle_center);
      setTextColor(ink.ink);
      drawString("COMBINE?", kPanelWidth / 2, 100);
      setFont(&fonts::Font2);
      char line[48];
      snprintf(line, sizeof line, "%s %s", forgeItemName(forge_tab_, forge_first_),
               forgeAttrName(forge_tab_, forge_first_, forge_attr_a_));
      drawString(line, kPanelWidth / 2, 148);
      setTextColor(ink.muted);
      drawString("+", kPanelWidth / 2, 172);
      setTextColor(ink.ink);
      snprintf(line, sizeof line, "%s %s", forgeItemName(forge_tab_, forge_second_),
               forgeAttrName(forge_tab_, forge_second_, forge_attr_b_));
      drawString(line, kPanelWidth / 2, 196);
      char total[24];
      snprintf(total, sizeof total, "TOTAL $%d", forgeTotal());
      setTextColor(ink.accent);
      drawString(total, kPanelWidth / 2, 228);
      if (forge_msg_[0] != '\0') {
        setTextColor(ink.accent);
        drawString(forge_msg_, kPanelWidth / 2, 252);
      }
      drawKey("YES", forgeYesBox(), false);
      drawKey("NO", forgeNoBox(), false);
      return;
    }

    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("FORGED!", kPanelWidth / 2, 92);
    setFont(&fonts::Font2);
    setTextColor(ink.accent);
    drawString(forgeRevealName(), kPanelWidth / 2, 138);
    char line[48];
    if (forge_tab_ == 0) {
      const int lv = gunLv(activeForgeGun());
      snprintf(line, sizeof line, "LV%d  %d/8 TRAITS (+%s)", lv + 1, forgedSlotCount(),
               forgedLatestTrait());
    } else {
      snprintf(line, sizeof line, "%s", forge_tab_ == 1 ? "NEW POWER-UP" : "NEW UPGRADE");
    }
    setTextColor(ink.muted);
    drawString(line, kPanelWidth / 2, 168);
    if (forge_msg_[0] != '\0') {
      setTextColor(ink.accent);
      drawString(forge_msg_, kPanelWidth / 2, 204);
    }
    if (forge_tab_ == 0 && forgeAnyValid() && gunLv(activeForgeGun()) < kGunMaxLv) {
      char up[36];
      const int lv = gunLv(activeForgeGun());
      snprintf(up, sizeof up, "UPGRADE $%d", 50 * (lv + 1));
      drawKey(up, forgeRevealUpBox(), false);
    }
    drawKey(forge_tab_ == 0 ? "EQUIP" : "DONE", forgeEquipBox(), false);
    drawKey("BACK", homeBox(), false);
  }

  void beginForge() {
    fadeOut();
    mode_ = Mode::Forge;
    forge_step_ = 0;
    forge_tab_ = 0;
    forge_page_ = 0;
    forge_first_ = 0;
    forge_second_ = 0;
    forge_attr_a_ = 0;
    forge_attr_b_ = 0;
    forge_msg_ = "";
    forge_focus_ = -1;
    if (equippedForged() != nullptr) forge_focus_ = gun_equip_ - kGunCount;
    else {
      for (int i = kMaxForged - 1; i >= 0; --i) {
        if (forged_[i].valid) {
          forge_focus_ = i;
          break;
        }
      }
    }
    paintForge();
    fadeIn();
  }

  void forgeBack() {
    if (forge_step_ == 0 || forge_step_ == 5) {
      beginMenu();
      return;
    }
    forge_step_ -= 1;
    forge_msg_ = "";
    if (forge_step_ == 1) forge_page_ = 0;
    paintForge();
  }

  void forgePick(int row) {
    if (forge_step_ == 0) {
      const int idx = forge_page_ * kShopRows + row;
      if (idx >= forgeItemCount(forge_tab_) || !forgeItemOwned(forge_tab_, idx)) return;
      forge_first_ = idx;
      forge_step_ = 1;
      forge_page_ = 0;
      forge_msg_ = "";
      paintForge();
      return;
    }
    if (forge_step_ == 1) {
      const int idx = forge_page_ * kShopRows + row;
      if (idx >= forgeItemCount(forge_tab_) || !forgeItemOwned(forge_tab_, idx)) return;
      forge_second_ = idx;
      forge_step_ = 2;
      forge_attr_a_ = 0;
      forge_msg_ = "";
      paintForge();
      return;
    }
    if (row < 0 || row >= kAttrCount) return;
    if (forge_step_ == 2) {
      forge_attr_a_ = row;
      forge_step_ = 3;
      forge_attr_b_ = 0;
      forge_msg_ = "";
      paintForge();
      return;
    }
    if (forge_step_ == 3) {
      forge_attr_b_ = row;
      forge_step_ = 4;
      forge_msg_ = "";
      paintForge();
    }
  }

  void forgeFlash() {
    for (int i = 0; i < 3; ++i) {
      fillScreen(TFT_WHITE);
      delay(70);
      fillScreen(palette().bg);
      delay(70);
    }
  }

  void forgeConfirm() {
    const int total = forgeTotal();
    if (bank_ < total) {
      forge_msg_ = "NOT ENOUGH COINS";
      paintForge();
      return;
    }
    int slot = -1;
    if (forge_tab_ == 0) {
      slot = firstEmptyForge();
      if (slot < 0) {
        forge_msg_ = "FORGE FULL";
        paintForge();
        return;
      }
    }
    bank_ = static_cast<uint16_t>(bank_ - total);
    if (forge_tab_ == 0) {
      ForgedGun& g = forged_[slot];
      g.valid = 1;
      g.base = static_cast<uint8_t>(forge_first_);
      g.donor = static_cast<uint8_t>(forge_second_);
      g.attr_a = static_cast<uint8_t>(forge_attr_a_);
      g.attr_b = static_cast<uint8_t>(forge_attr_b_);
      clampForged(g);
      gun_lv_[kGunCount + slot] = 0;
      gun_equip_ = static_cast<uint8_t>(kGunCount + slot);
      forge_focus_ = slot;
      saveGear();
    } else if (forge_tab_ == 1) {
      forge_power_ |= forgePerk(1, forge_first_, forge_attr_a_) |
                      forgePerk(1, forge_second_, forge_attr_b_);
    } else {
      forge_up_ |= forgePerk(2, forge_first_, forge_attr_a_) |
                   forgePerk(2, forge_second_, forge_attr_b_);
      ship_step_ = kShipStep + thrust_ + (perk(PERK_SPEED) ? 1 : 0);
    }
    saveShop();
    forgeFlash();
    forge_step_ = 5;
    forge_msg_ = "";
    paintForge();
    fadeIn();
  }

  void forgeUpgradeGun() {
    if (!forgeAnyValid()) return;
    const int gun = activeForgeGun();
    const int lv = gunLv(gun);
    if (lv >= kGunMaxLv) {
      forge_msg_ = "MAXED";
      paintForge();
      return;
    }
    const int price = 50 * (lv + 1);
    if (bank_ < price) {
      forge_msg_ = "NOT ENOUGH COINS";
      paintForge();
      return;
    }
    bank_ = static_cast<uint16_t>(bank_ - price);
    gun_lv_[gun] = static_cast<uint8_t>(lv + 1);
    gun_equip_ = static_cast<uint8_t>(gun);
    forge_focus_ = gun - kGunCount;
    saveGear();
    saveShop();
    forgeFlash();
    forge_tab_ = 0;
    forge_step_ = 5;
    forge_msg_ = "";
    paintForge();
    fadeIn();
  }

  // ---- Who killed you ----------------------------------------------------

  const char* foeKiller(const FoeBolt& foe) const {
    if (god_ != 0 && (god_entrance_ || foe.kind >= 3)) return godName();
    if (mini_) return "MINI BOSS";
    if (foe.kind == 2) return "CANNON";
    if (foe.kind == 1) return "ASSAULT";
    return "ALIEN";
  }

  const char* unitKiller(uint8_t kind) const {
    switch (kind) {
      case 2:
        if (god_ != 0) return godName();
        return raid_ ? "RAID BOSS" : "BOSS";
      case 3:
        return "DRONE";
      case 4:
        return "ASSAULT";
      case 5:
        return "CANNON";
      case 6:
        return "MINI BOSS";
      default:
        return "ALIEN";
    }
  }

  // ---- Checkpoints -------------------------------------------------------

  void paintChecks() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("CHECKPOINTS", kPanelWidth / 2, 26);
    for (int i = 0; i < 15; ++i) {
      const Box cell = checkCell(i);
      const bool open = (camp_ & (1u << i)) != 0;
      char label[12];
      snprintf(label, sizeof label, "%d", (i + 1) * 20);
      fillRect(cell.x, cell.y, cell.w, cell.h, open ? ink.button : ink.bg);
      drawRect(cell.x, cell.y, cell.w, cell.h, open ? ink.accent : ink.button);
      setTextColor(open ? ink.ink : ink.muted);
      drawString(label, cell.x + cell.w / 2, cell.y + cell.h / 2);
    }
    drawKey("BACK", homeBox(), false);
  }

  void beginChecks() {
    fadeOut();
    mode_ = Mode::Check;
    paintChecks();
    fadeIn();
  }

  // ---- Restart confirmation ---------------------------------------------

  void paintConfirm() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("RESTART GAME?", kPanelWidth / 2, 120);
    setFont(&fonts::Font2);
    setTextColor(ink.muted);
    drawString("Are you sure?", kPanelWidth / 2, 156);
    drawKey("YES", yesBox(), false);
    drawKey("NO", noBox(), false);
  }

  void beginConfirm() {
    fadeOut();
    mode_ = Mode::Confirm;
    paintConfirm();
    fadeIn();
  }

  // ---- Campaign ----------------------------------------------------------

  void beginCampaign() {
    demo_ = false;
    clearBoosts();
    for (int i = 0; i < kBoosts; ++i) {
      if (boost_armed_[i] == 0) continue;
      boost_ticks_[i] = boost_armed_[i];
      boost_armed_[i] = 0;
    }
    saveShop();
    fadeOut();
    score_ = 0;
    lives_ = 3;
    if (perk(PERK_LIFE)) lives_ += 1;
    if (spare_ > 0) {
      spare_ -= 1;
      lives_ += 1;
      saveShop();
    }
    if (perk(PERK_SHIELD_START) && shield_ < shieldCap()) {
      shield_ = static_cast<uint8_t>(shield_ + 1);
      saveShop();
    }
    revived_ = false;
    god_ = 0;
    god_wave_ = 0;
    god_entrance_ = false;
    mini_ = false;
    raid_ = false;
    invuln_ = 0;
    clearFoe();
    foe_cool_ = 0;
    hud_msg_ticks_ = 0;
    beam_burst_ = 0;
    beam_cool_ = 0;
    sky_on_ = false;
    rng_ = esp_random();
    if (rng_ == 0) rng_ = 1;
    startLevel();
    saveCampaign();
    fadeIn();
  }

  void restartCampaign() {
    level_ = 1;
    camp_ = 0;
    raid_won_ = false;
    score_ = 0;
    prefs_.putUShort("lv", 1);
    prefs_.putUShort("cp", 0);
    prefs_.putUChar("raidok", 0);
    beginCampaign();
  }

  // ---- Divine Machines ---------------------------------------------------

  int divineSlots() const {
    int slots = 1 + static_cast<int>(div_multi_);
    if (slots > kGods) slots = kGods;
    return slots;
  }

  int divineEquipped() const {
    int n = 0;
    for (int i = 0; i < kGods; ++i) {
      if ((div_equip_ & (1u << i)) != 0) n += 1;
    }
    return n;
  }

  bool divineOn(int i) const {
    return i >= 0 && i < kGods && (div_equip_ & (1u << i)) != 0;
  }

  void trimDivine() {
    div_unlock_ = static_cast<uint16_t>(div_unlock_ & 0x0FFF);
    if (div_multi_ > 11) div_multi_ = 11;
    div_equip_ = static_cast<uint16_t>(div_equip_ & div_unlock_);
    const int cap = divineSlots();
    int n = 0;
    uint16_t keep = 0;
    for (int i = 0; i < kGods; ++i) {
      if ((div_equip_ & (1u << i)) == 0) continue;
      if (n >= cap) break;
      keep = static_cast<uint16_t>(keep | (1u << i));
      n += 1;
    }
    div_equip_ = keep;
  }

  const char* machineName(int i) const {
    static const char* names[kGods] = {
        "LIGHTNING", "DOUBLE", "TSUNAMI", "HEAL RAY", "AEGIS", "BERSERKER",
        "SOLARIS", "SILVER", "CHARM", "ANVIL", "CADUCEUS", "REVEL"};
    if (i < 0 || i >= kGods) return "MACHINE";
    return names[i];
  }

  const char* machineHint(int i) const {
    static const char* hints[kGods] = {
        "stun zap 5s", "clone ship", "wave blocks 8s", "+3 then +1",
        "3s shield x2", "1-life proc", "burn beam 5s", "homing 6s",
        "stop fire 9s", "blast 6s", "dash 6s", "slow shots 8s"};
    if (i < 0 || i >= kGods) return "";
    return hints[i];
  }

  int machineCool(int i) const {
    static const int cool[kGods] = {312, 0, 500, 625, 437, 0, 312, 375, 562, 375, 375, 500};
    if (i < 0 || i >= kGods) return 0;
    return cool[i];
  }

  void grantDivine(int god) {
    if (god < 1 || god > kGods) return;
    div_unlock_ = static_cast<uint16_t>(div_unlock_ | (1u << (god - 1)));
    saveShop();
    noteUnlock(machineName(god - 1));
  }

  void resetMachines() {
    demeter_uses_ = 0;
    athena_uses_ = 0;
    aegis_ticks_ = 0;
    aegis_hits_ = 0;
    aegis_box_ = false;
    stun_ticks_ = 0;
    charm_ticks_ = 0;
    revel_ticks_ = 0;
    hermes_ticks_ = 0;
    berserk_ticks_ = 0;
    wave_y_ = -1;
    sun_ticks_ = 0;
    god_beam_ticks_ = 0;
    god_ward_ = 0;
    drone_drawn_ = false;
    drone_n_ = 0;
    clone_alive_ = !demo_ && divineOn(1);
    clone_drawn_ = false;
    clone_cool_ = 0;
    clone_x_ = 16;
    for (int i = 0; i < kGods; ++i) div_cool_[i] = 24;
  }

  void tryBerserk() {
    if (lives_ != 1 || !divineOn(5)) return;
    if ((nextRng() % 10) != 0) return;
    berserk_ticks_ = 500;
    for (int i = 0; i < kGods; ++i) {
      div_cool_[i] = div_cool_[i] > 125 ? div_cool_[i] - 125 : 0;
    }
    if (gun_cool_ > 125) gun_cool_ -= 125;
    else gun_cool_ = 0;
    if (beam_cool_ > 125) beam_cool_ -= 125;
    else beam_cool_ = 0;
    noteUnlock("BERSERK");
  }

  void paintDivine() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("DIVINE MACHINES", kPanelWidth / 2, 18);
    char head[28];
    snprintf(head, sizeof head, "%d/%d EQUIPPED", divineEquipped(), divineSlots());
    setTextColor(ink.accent);
    drawString(head, kPanelWidth / 2, 40);
    if (div_msg_[0] != '\0') {
      setTextColor(ink.muted);
      drawString(div_msg_, kPanelWidth / 2, 62);
    }
    for (int row = 0; row < kShopRows; ++row) {
      const int idx = div_page_ * kShopRows + row;
      if (idx >= kGods) break;
      const Box b = shopRow(row);
      const bool on = divineOn(idx);
      const bool owned = (div_unlock_ & (1u << idx)) != 0;
      fillRect(b.x, b.y, b.w, b.h, on ? ink.buttonOn : ink.button);
      drawRect(b.x, b.y, b.w, b.h, on ? ink.accent : ink.muted);
      setTextDatum(lgfx::middle_left);
      setTextColor(owned ? ink.ink : ink.muted);
      char line[28];
      snprintf(line, sizeof line, "%s %s", godNameOf(idx + 1), machineName(idx));
      drawString(line, b.x + 8, b.y + 14);
      setTextColor(ink.muted);
      drawString(machineHint(idx), b.x + 8, b.y + 32);
      setTextDatum(lgfx::middle_right);
      setTextColor(on ? ink.accent : ink.muted);
      drawString(on ? "ON" : (owned ? "READY" : "LOCKED"), b.x + b.w - 8, b.y + b.h / 2);
    }
    drawKey("PAGE", shopPageBox(), false);
    drawKey("BACK", homeBox(), false);
  }

  void beginDivine() {
    div_page_ = 0;
    div_msg_[0] = '\0';
    fadeOut();
    mode_ = Mode::Divine;
    paintDivine();
    fadeIn();
  }

  void divineToggle(int idx) {
    if (idx < 0 || idx >= kGods) return;
    div_msg_[0] = '\0';
    const uint16_t bit = static_cast<uint16_t>(1u << idx);
    if ((div_unlock_ & bit) == 0) {
      snprintf(div_msg_, sizeof div_msg_, "LOCKED");
      paintDivine();
      return;
    }
    if ((div_equip_ & bit) != 0) {
      div_equip_ = static_cast<uint16_t>(div_equip_ & static_cast<uint16_t>(~bit));
    } else if (divineEquipped() >= divineSlots()) {
      snprintf(div_msg_, sizeof div_msg_, "NEED MULTI");
      paintDivine();
      return;
    } else {
      div_equip_ = static_cast<uint16_t>(div_equip_ | bit);
    }
    saveShop();
    paintDivine();
  }

  // ---- Olympian gods -----------------------------------------------------

  bool isGodLevel(int lv) const { return lv % 25 == 0; }

  int godForLevel(int lv) const { return ((lv / 25 - 1) % 12) + 1; }

  const char* godNameOf(int god) const {
    switch (god) {
      case 1: return "ZEUS";
      case 2: return "HERA";
      case 3: return "POSEIDON";
      case 4: return "DEMETER";
      case 5: return "ATHENA";
      case 6: return "ARES";
      case 7: return "APOLLO";
      case 8: return "ARTEMIS";
      case 9: return "APHRODITE";
      case 10: return "HEPHAESTUS";
      case 11: return "HERMES";
      case 12: return "DIONYSUS";
      default: return "GOD";
    }
  }

  const char* godName() const { return godNameOf(god_); }

  uint16_t godInkOf(int god) const {
    switch (god) {
      case 1: return color565(255, 214, 56);
      case 2: return color565(226, 170, 255);
      case 3: return color565(64, 190, 255);
      case 4: return color565(120, 220, 96);
      case 5: return color565(206, 214, 232);
      case 6: return color565(255, 72, 60);
      case 7: return color565(255, 200, 88);
      case 8: return color565(176, 220, 255);
      case 9: return color565(255, 140, 200);
      case 10: return color565(255, 128, 40);
      case 11: return color565(140, 255, 220);
      case 12: return color565(190, 96, 255);
      default: return palette().accent;
    }
  }

  uint16_t godInk() const { return godInkOf(god_); }

  int godBarHp() const { return 12 + level_ / 12; }

  // Pillars and the ring are scenery. They never overlap the ship as a hit.
  void paintGodMark(uint16_t ink) {
    const int cx = kPanelWidth / 2;
    switch (god_) {
      case 1:
        drawLine(cx - 44, 98, cx - 34, 114, ink);
        drawLine(cx - 34, 114, cx - 46, 114, ink);
        drawLine(cx - 46, 114, cx - 32, 134, ink);
        drawLine(cx + 44, 98, cx + 34, 114, ink);
        drawLine(cx + 34, 114, cx + 46, 114, ink);
        drawLine(cx + 46, 114, cx + 32, 134, ink);
        break;
      case 2:
        for (int i = -1; i <= 1; ++i) {
          drawCircle(cx + i * 28, 102, 6, ink);
          fillCircle(cx + i * 28, 102, 2, ink);
        }
        break;
      case 3:
        for (int band = 0; band < 2; ++band) {
          const int y = 104 + band * 18;
          for (int x = 48; x < kPanelWidth - 56; x += 10) {
            const bool down = ((x / 10) & 1) == 0;
            drawLine(x, y + (down ? 0 : 5), x + 10, y + (down ? 5 : 0), ink);
          }
        }
        break;
      case 4:
        for (int i = -1; i <= 1; ++i) {
          const int x = cx + i * 36;
          fillRect(x, 92, 2, 16, ink);
          drawLine(x, 96, x - 6, 92, ink);
          drawLine(x + 1, 100, x + 7, 96, ink);
          drawLine(x, 104, x - 6, 100, ink);
        }
        break;
      case 5:
        drawLine(cx, 90, cx - 14, 104, ink);
        drawLine(cx, 90, cx + 14, 104, ink);
        drawLine(cx - 14, 104, cx - 14, 118, ink);
        drawLine(cx + 14, 104, cx + 14, 118, ink);
        drawLine(cx - 14, 118, cx, 132, ink);
        drawLine(cx + 14, 118, cx, 132, ink);
        break;
      case 6:
        drawLine(cx - 30, 94, cx - 8, 132, ink);
        drawLine(cx + 30, 94, cx + 8, 132, ink);
        break;
      case 7:
        for (int dx = -1; dx <= 1; ++dx) {
          for (int dy = -1; dy <= 1; ++dy) {
            if (dx == 0 && dy == 0) continue;
            const int inner = (dx != 0 && dy != 0) ? 6 : 8;
            const int outer = (dx != 0 && dy != 0) ? 14 : 18;
            drawLine(cx + dx * inner, 108 + dy * inner, cx + dx * outer, 108 + dy * outer, ink);
          }
        }
        break;
      case 8:
        drawCircle(cx, 108, 10, ink);
        fillCircle(cx + 6, 106, 8, palette().bg);
        break;
      case 9:
        fillCircle(cx - 5, 100, 5, ink);
        fillCircle(cx + 5, 100, 5, ink);
        drawLine(cx - 10, 102, cx, 118, ink);
        drawLine(cx + 10, 102, cx, 118, ink);
        break;
      case 10:
        fillRect(cx - 12, 96, 24, 5, ink);
        fillRect(cx - 6, 101, 12, 8, ink);
        fillRect(cx - 14, 109, 28, 4, ink);
        break;
      case 11:
        drawLine(cx - 8, 108, cx - 28, 100, ink);
        drawLine(cx - 8, 108, cx - 26, 112, ink);
        drawLine(cx - 16, 106, cx - 28, 108, ink);
        drawLine(cx + 8, 108, cx + 28, 100, ink);
        drawLine(cx + 8, 108, cx + 26, 112, ink);
        drawLine(cx + 16, 106, cx + 28, 108, ink);
        break;
      default:
        fillCircle(cx, 96, 3, ink);
        fillCircle(cx - 8, 100, 3, ink);
        fillCircle(cx + 8, 100, 3, ink);
        fillCircle(cx - 4, 108, 3, ink);
        fillCircle(cx + 4, 108, 3, ink);
        fillCircle(cx, 114, 3, ink);
        break;
    }
  }

  void paintGodArena(bool title = true) {
    const uint16_t ink = godInk();
    for (int i = 0; i < 6; ++i) {
      const int x = 18 + i * 56;
      fillRect(x, 40, 6, kShipY - 44, ink);
      fillRect(x - 6, 40, 18, 5, ink);
    }
    drawCircle(kPanelWidth / 2, 150, 78, ink);
    drawCircle(kPanelWidth / 2, 150, 60, ink);
    paintGodMark(ink);
    if (!title) return;
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink);
    char line[24];
    snprintf(line, sizeof line, "%s DOMAIN", godName());
    drawString(line, kPanelWidth / 2, 150);
  }

  void buildGodArena() {
    unit_n_ = 0;
    boss_ = false;
    mini_ = false;
    raid_ = false;
    barrier_n_ = 0;
    sky_on_ = false;
    boss_layers_ = 0;
    boss_layer_max_ = 0;
    god_ = static_cast<uint8_t>(godForLevel(level_));
    god_wave_ = 0;
    god_bar_ = 3;
    god_phase_ = 0;
    god_entrance_ = false;
    god_hp_ = 0;
    god_hp_max_ = 1;
    drop_px_ = 8;
    foe_gap_ = 30;
    step_px_ = 2;
    buildTerrain();
    spawnGodWave();
  }

  // Four waves of goons, each led by a mini boss.
  void spawnGodWave() {
    unit_n_ = 0;
    boss_ = false;
    mini_ = false;
    const int wave = god_wave_;
    const int cols = 4 + wave;
    const int rows = 2 + (wave >= 2 ? 1 : 0);
    const int pitch_x = 30;
    const int pitch_y = 22;
    for (int row = 0; row < rows; ++row) {
      for (int col = 0; col < cols; ++col) {
        int kind = (row == 0) ? 1 : 0;
        const uint32_t roll = nextRng() % 100;
        if (roll < 12) kind = 4;
        else if (roll < 20) kind = 5;
        addUnit(col * pitch_x, row * pitch_y, kind);
      }
    }
    const int mini_w = 30;
    boss_max_ = 4 + level_ / 30 + wave;
    boss_hp_ = boss_max_;
    addUnit((cols * pitch_x - mini_w) / 2, -6, 6);
    origin_x_ = (kPanelWidth - cols * pitch_x) / 2;
    origin_y_ = 40;
    form_dir_ = 1;
    step_px_ = 2 + wave;
    foe_cool_ = 0;
    clearFoe();
    clearBolts();
    if (wave > 0 && mode_ == Mode::Play) paintLevel();
  }

  void beginGodSplash() {
    mode_ = Mode::Splash;
    splash_at_ = millis() + 1000;
    const Palette ink = palette();
    fillScreen(ink.bg);
    setTextDatum(lgfx::middle_center);
    setFont(&fonts::Font6);
    if (textWidth(godName()) > kPanelWidth - 24) setFont(&fonts::Font4);
    setTextColor(godInk());
    drawString(godName(), kPanelWidth / 2, 190);
    setFont(&fonts::Font2);
    setTextColor(ink.muted);
    drawString("DESCENDS", kPanelWidth / 2, 250);
  }

  void startGodFight() {
    unit_n_ = 0;
    boss_ = true;
    mini_ = false;
    raid_ = false;
    god_entrance_ = true;
    god_bar_ = 3;
    god_phase_ = 0;
    god_hp_max_ = godBarHp();
    god_hp_ = god_hp_max_;
    boss_max_ = 1;
    boss_hp_ = 1;
    boss_layers_ = 0;
    boss_layer_max_ = 0;
    boss_style_ = 5;
    laser_live_ = false;
    clearFoe();
    clearBolts();
    const int boss_ox = (kPanelWidth - kBossW) / 2;
    addUnit(boss_ox, 4, 2);
    // Two themed escorts flank the god and keep firing while it fights.
    addUnit(boss_ox - 52, 14, 7);
    addUnit(boss_ox + kBossW + 34, 14, 7);
    origin_x_ = 0;
    origin_y_ = 34;
    form_dir_ = 1;
    step_px_ = 2;
    mode_ = Mode::Play;
    god_ward_ = 0;
    god_beam_ticks_ = 0;
    paintLevel();
    noteUnlock("GOD");
  }


  int unitW(const Unit& unit) const {
    if (unit.kind == 2) return kBossW;
    if (unit.kind == 3) return kDroneW;
    if (unit.kind == 5) return kAlienW + 4;
    if (unit.kind == 6) return kMiniW;
    if (unit.kind == 7) return kAllyW;
    return kAlienW;
  }
  int unitH(const Unit& unit) const {
    if (unit.kind == 2) return kBossH;
    if (unit.kind == 3) return kDroneH;
    if (unit.kind == 5) return kAlienH + 2;
    if (unit.kind == 6) return kMiniH;
    if (unit.kind == 7) return kAllyH;
    return kAlienH;
  }

  bool anyMinion() const {
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].alive && units_[i].kind != 2) return true;
    }
    return false;
  }

  bool bossAlive() const {
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].alive && units_[i].kind == 2) return true;
    }
    return false;
  }

  void drawAlienAt(int x, int y, uint8_t kind) {
    const Palette ink = palette();
    const uint16_t body = kind == 0 ? ink.low : ink.top;
    const uint16_t eye = ink.bg;
    if (kind == 4) {
      // Assault trooper: angular hull, twin barrels.
      const uint16_t plate = color565(255, 120, 96);
      fillRect(x + 4, y + 2, 10, 7, plate);
      fillRect(x + 1, y + 8, 16, 4, body);
      fillRect(x + 2, y + 12, 3, 2, plate);
      fillRect(x + 13, y + 12, 3, 2, plate);
      fillRect(x + 5, y + 9, 2, 4, eye);
      fillRect(x + 11, y + 9, 2, 4, eye);
      return;
    }
    if (kind == 5) {
      // Cannon: wide hull with one fat barrel and a fuse light.
      const uint16_t plate = color565(255, 150, 60);
      fillRect(x + 1, y + 4, 20, 9, body);
      fillRect(x + 6, y + 1, 10, 4, plate);
      fillRect(x + 8, y + 13, 6, 3, plate);
      fillRect(x + 3, y + 6, 3, 3, eye);
      fillRect(x + 15, y + 6, 3, 3, eye);
      fillRect(x + 9, y + 8, 4, 2, color565(255, 220, 120));
      return;
    }
    if (kind == 6) {
      // Mini boss: squat, armoured, with its own small bar.
      const uint16_t plate = god_ != 0 ? godInk() : ink.accent;
      fillRect(x + 4, y + 4, kMiniW - 8, kMiniH - 8, ink.top);
      fillRect(x, y + 8, 5, 6, plate);
      fillRect(x + kMiniW - 5, y + 8, 5, 6, plate);
      fillRect(x + 9, y + 7, 4, 4, eye);
      fillRect(x + kMiniW - 13, y + 7, 4, 4, eye);
      fillRect(x + 12, y + kMiniH - 4, 6, 3, plate);
      int pool = boss_max_ > 0 ? boss_max_ : 1;
      int left = boss_hp_ < 0 ? 0 : boss_hp_;
      if (left > pool) left = pool;
      fillRect(x + 4, y + kMiniH - 3, (kMiniW - 8) * left / pool, 2, plate);
      return;
    }
    if (kind == 7) {
      // God ally: small winged escort in the god's colour.
      const uint16_t plate = godInk();
      fillRect(x + 4, y + 4, 10, 7, plate);
      fillRect(x, y + 2, 4, 4, plate);
      fillRect(x + kAllyW - 4, y + 2, 4, 4, plate);
      fillRect(x + 7, y + 6, 4, 3, eye);
      return;
    }
    if (kind == 3) {
      fillRect(x + 2, y + 3, 12, 6, ink.top);
      fillRect(x, y + 5, 4, 3, ink.accent);
      fillRect(x + 12, y + 5, 4, 3, ink.accent);
      fillRect(x + 6, y + 5, 2, 2, eye);
      return;
    }
    if (kind == 2) {
      fillRect(x + 8, y + 4, 28, 16, body);
      fillRect(x + 4, y, 6, 8, body);
      fillRect(x + 34, y, 6, 8, body);
      if (boss_style_ == 2 && boss_open_) {
        fillRect(x + 12, y + 7, 6, 6, laserColor());
        fillRect(x + 26, y + 7, 6, 6, laserColor());
      } else if (boss_style_ == 2) {
        fillRect(x + 12, y + 9, 8, 3, ink.muted);
        fillRect(x + 26, y + 9, 8, 3, ink.muted);
      } else if (boss_style_ == 3) {
        fillRect(x + 10, y + 10, 24, 4, color565(255, 72, 64));
      } else if (boss_style_ == 4) {
        fillRect(x + 20, y + 4, 4, 16, color565(255, 48, 40));
      } else {
        fillRect(x + 14, y + 8, 4, 4, eye);
        fillRect(x + 26, y + 8, 4, 4, eye);
      }
      fillRect(x + 6, y + 20, 8, 6, body);
      fillRect(x + 30, y + 20, 8, 6, body);
      if (boss_layers_ > 0) {
        drawRect(x + 1, y + 1, kBossW - 2, kBossH - 2, ink.accent);
      }
      int pool = boss_layers_ > 0 ? boss_layer_max_ : boss_max_;
      int left = boss_layers_ > 0 ? boss_shield_ : boss_hp_;
      int inner = pool > 0 ? (kBossW - 8) * left / pool : 4;
      if (inner < 4) inner = 4;
      fillRect(x + 4, y + kBossH - 4, inner, 3, boss_layers_ > 0 ? ink.accent : ink.shot);
      return;
    }
    if (kind == 1) {
      fillRect(x + 4, y + 2, 10, 8, body);
      fillRect(x + 2, y, 2, 4, body);
      fillRect(x + 14, y, 2, 4, body);
      fillRect(x + 6, y + 4, 2, 2, eye);
      fillRect(x + 10, y + 4, 2, 2, eye);
      fillRect(x + 2, y + 10, 4, 3, body);
      fillRect(x + 12, y + 10, 4, 3, body);
      return;
    }
    fillRect(x + 3, y + 1, 12, 4, body);
    fillRect(x + 1, y + 5, 16, 6, body);
    fillRect(x + 5, y + 6, 2, 2, eye);
    fillRect(x + 11, y + 6, 2, 2, eye);
    fillRect(x + 2, y + 11, 3, 3, body);
    fillRect(x + 13, y + 11, 3, 3, body);
  }

  int shipClearX(int x) const { return x - 7; }
  int shipClearY() const { return kShipY - 8; }
  int shipClearW() const { return kShipW + 15; }
  int shipClearH() const { return kShipH + 17; }

  void drawShieldBubble(int x) {
    if (shield_ <= 0) return;
    const int cx = x + kShipW / 2;
    const int cy = kShipY + kShipH / 2;
    const uint16_t ring = perk(PERK_SHIELD_GLOW) ? color565(170, 245, 255) : color565(96, 200, 255);
    const uint16_t dim = color565(38, 92, 150);
    drawEllipse(cx, cy, 22, 15, ring);
    drawEllipse(cx, cy, 21, 14, dim);
    if (perk(PERK_SHIELD_GLOW)) drawEllipse(cx, cy, 20, 13, ring);
    drawPixel(cx - 10, cy - 12, color565(255, 255, 255));
  }

  void drawShipHull(int x, int y, int model) {
    const Palette pal = palette();
    const uint16_t ink = pal.ship;
    const uint16_t acc = pal.accent;
    switch (model % kShipCount) {
      case 1:
        fillRect(x + 15, y, 2, 15, ink);
        fillRect(x + 6, y + 7, 20, 5, ink);
        fillRect(x + 1, y + 3, 3, 12, ink);
        fillRect(x + 28, y + 3, 3, 12, ink);
        fillRect(x + 14, y + 5, 4, 3, acc);
        break;
      case 2:
        fillRect(x + 6, y + 2, 5, 14, ink);
        fillRect(x + 21, y + 2, 5, 14, ink);
        fillRect(x + 11, y + 5, 10, 7, ink);
        fillRect(x, y + 10, 32, 4, ink);
        fillRect(x + 14, y + 6, 4, 4, acc);
        break;
      case 3:
        fillTriangle(x + 16, y, x, y + 14, x + 31, y + 14, ink);
        fillRect(x + 6, y + 13, 6, 3, acc);
        fillRect(x + 20, y + 13, 6, 3, acc);
        fillRect(x + 14, y + 6, 4, 3, acc);
        break;
      case 4:
        fillRect(x + 4, y + 4, 24, 11, ink);
        fillRect(x, y + 7, 32, 6, ink);
        fillRect(x + 5, y, 3, 5, acc);
        fillRect(x + 14, y, 4, 5, acc);
        fillRect(x + 24, y, 3, 5, acc);
        fillRect(x + 10, y + 8, 12, 3, acc);
        break;
      case 5:
        fillRect(x + 14, y, 4, 16, ink);
        fillRect(x + 3, y + 4, 10, 3, ink);
        fillRect(x + 19, y + 4, 10, 3, ink);
        fillRect(x, y + 10, 12, 4, ink);
        fillRect(x + 20, y + 10, 12, 4, ink);
        fillRect(x + 13, y + 6, 6, 4, acc);
        break;
      default:
        fillRect(x + kShipW / 2 - 2, y, 4, kShipH, ink);
        fillRect(x, y + 9, kShipW, 6, ink);
        fillRect(x + kShipW / 2 - 1, y + 5, 2, 4, acc);
        break;
    }
  }

  void drawShipAt(int x) {
    drawShipHull(x, kShipY, ship_model_);
    drawShieldBubble(x);
  }

  void drawBarrier(int index) {
    const Barrier& bar = barriers_[index];
    const uint16_t ink = palette().barrier;
    for (int col = 0; col < kBarCols; ++col) {
      for (int row = 0; row < bar.col[col]; ++row) {
        fillRect(bar.x + col * 3, bar.y + row * 3, 3, 2, ink);
      }
    }
  }

  void drawHud() {
    const Palette ink = palette();
    fillRect(0, 0, kPanelWidth, 24, ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(ink.ink);
    char line[40];
    char boosts[28];
    boostHud(boosts, sizeof boosts);
    if (hud_msg_ticks_ > 0) {
      snprintf(line, sizeof line, "%s", hud_msg_);
    } else if (god_ != 0) {
      if (god_entrance_) snprintf(line, sizeof line, "%s", godName());
      else if (boosts[0] != '\0') snprintf(line, sizeof line, "%s %d/4", boosts, god_wave_ + 1);
      else snprintf(line, sizeof line, "%s %d/4", godName(), god_wave_ + 1);
    } else if (boosts[0] != '\0') {
      snprintf(line, sizeof line, "%s %d %d", boosts, level_, score_);
    } else if (mini_) {
      snprintf(line, sizeof line, "MINI %d %d", level_, score_);
    } else if (boss_) {
      const char* tag = "CORE";
      if (raid_ && anyMinion()) tag = "GOONS";
      else if (boss_style_ == 1 && anyMinion()) tag = "DRONES";
      else if (boss_layers_ > 0) tag = "SHIELD";
      else if (boss_style_ == 2) tag = boss_open_ ? "OPEN" : "SHUT";
      else if (boss_style_ == 3) tag = "SPRAY";
      else if (boss_style_ == 4) tag = "LASER";
      else if (raid_) tag = "RAID";
      snprintf(line, sizeof line, "%s %d %d", tag, level_, score_);
    } else {
      snprintf(line, sizeof line, "MACHINA %d %d", level_, score_);
    }
    const int slots = heartSlots();
    if (textWidth(line) > kPanelWidth - 70 - slots * 14) {
      if (boosts[0] != '\0' && hud_msg_ticks_ == 0) {
        snprintf(line, sizeof line, "%s", boosts);
      } else {
        snprintf(line, sizeof line, "M %d %d", level_, score_);
      }
    }
    drawString(line, 8, 12);
    drawHearts();
    char coins[12];
    snprintf(coins, sizeof coins, "$%u", static_cast<unsigned>(bank_));
    setTextDatum(lgfx::middle_right);
    drawString(coins, kPanelWidth - 8 - slots * 14 - 6, 12);
    if (boss_ || god_ != 0) drawBossBar();
  }

  void drawBossBar() {
    const Palette ink = palette();
    if (god_ != 0 && god_entrance_) {
      // Three health bars, one per fighting style.
      const int w = 96;
      for (int bar = 0; bar < 3; ++bar) {
        const int x = 8 + bar * (w + 4);
        fillRect(x, 26, w, 8, ink.button);
        const bool live = bar < god_bar_;
        if (live) {
          int pool = god_hp_max_ > 0 ? god_hp_max_ : 1;
          int left = (bar == god_bar_ - 1) ? god_hp_ : pool;
          if (left < 0) left = 0;
          if (left > pool) left = pool;
          const int fill = (w - 4) * left / pool;
          if (fill > 0) fillRect(x + 2, 28, fill, 4, godInk());
        }
        drawRect(x, 26, w, 8, live ? godInk() : ink.button);
      }
      return;
    }
    int pool = boss_layers_ > 0 ? boss_layer_max_ : boss_max_;
    int left = boss_layers_ > 0 ? boss_shield_ : boss_hp_;
    if (pool < 1) pool = 1;
    if (left < 0) left = 0;
    if (left > pool) left = pool;
    const int w = 160 * left / pool;
    fillRect(8, 26, 200, 8, ink.bg);
    fillRect(8, 26, 160, 6, ink.button);
    if (w > 0) fillRect(8, 26, w, 6, boss_layers_ > 0 ? ink.accent : ink.shot);
    for (int i = 0; i < 4; ++i) {
      fillRect(172 + i * 8, 26, 6, 6, i < boss_layers_ ? ink.accent : ink.button);
    }
  }

  void drawGameKeys(const char* held) {
    drawKey("LEFT", leftBox(), held != nullptr && std::strcmp(held, "LEFT") == 0);
    drawKey("FIRE", fireBox(), held != nullptr && std::strcmp(held, "FIRE") == 0);
    drawKey("RIGHT", rightBox(), held != nullptr && std::strcmp(held, "RIGHT") == 0);
  }

  void addUnit(int ox, int oy, int kind) {
    if (unit_n_ >= kMaxUnits) {
      return;
    }
    units_[unit_n_].ox = static_cast<int16_t>(ox);
    units_[unit_n_].oy = static_cast<int16_t>(oy);
    units_[unit_n_].kind = static_cast<uint8_t>(kind);
    units_[unit_n_].hp = static_cast<uint8_t>(kind == 7 ? 2 : 1);
    units_[unit_n_].stun = 0;
    units_[unit_n_].alive = true;
    unit_n_ += 1;
  }

  void centerFormation() {
    int min_x = 999;
    int max_x = 0;
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].ox < min_x) min_x = units_[i].ox;
      const int right = units_[i].ox + unitW(units_[i]);
      if (right > max_x) max_x = right;
    }
    if (max_x <= min_x) {
      origin_x_ = (kPanelWidth - kBossW) / 2;
      return;
    }
    origin_x_ = (kPanelWidth - (max_x - min_x)) / 2 - min_x;
  }

  void placeBoss(bool raid) {
    unit_n_ = 0;
    boss_ = true;
    raid_ = raid;
    barrier_n_ = 0;
    boss_open_ = false;
    boss_tick_ = 0;
    laser_tick_ = 0;
    laser_live_ = false;
    boss_shield_ = 0;
    boss_shield_max_ = 0;
    boss_layers_ = 0;
    boss_layer_max_ = 0;
    if (!raid) boss_style_ = static_cast<uint8_t>(((level_ / 5) - 1) % 5);
    int core = raid ? 36 + level_ / 3 : 20 + level_ / 4;
    if (core > 99) core = 99;
    int layers = 0;
    if (boss_style_ == 0) {
      layers = 2;
      if (level_ >= 80) layers += 1;
      if (level_ >= 180) layers += 1;
      if (raid) layers += 1;
    } else if (raid && boss_style_ != 4) {
      layers = 2;
    }
    if (boss_style_ == 4) layers = 0;
    if (layers > 4) layers = 4;
    int layer_hp = 5 + level_ / 20;
    if (layer_hp > 14) layer_hp = 14;
    boss_layers_ = static_cast<uint8_t>(layers);
    boss_layer_max_ = static_cast<uint8_t>(layer_hp);
    boss_shield_max_ = boss_layer_max_;
    boss_shield_ = layers > 0 ? boss_layer_max_ : 0;
    boss_max_ = core;
    boss_hp_ = core;
    int boss_ox = 0;
    if (raid) {
      const int cols = 6;
      const int pitch = 28;
      boss_ox = (cols * pitch - kBossW) / 2;
      for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < cols; ++col) {
          addUnit(col * pitch, 40 + row * 22, row == 0 ? 1 : 0);
        }
      }
    }
    addUnit(boss_ox, 0, 2);
    if (boss_style_ == 1) {
      int count = 2;
      if (level_ >= 40) count = 3;
      if (level_ >= 120) count = 4;
      if (raid && count < 4) count += 1;
      for (int slot = 0; slot < count; ++slot) {
        int ox = boss_ox - 52;
        if (slot == 1) ox = boss_ox + kBossW + 8;
        else if (slot == 2) ox = boss_ox - 76;
        else if (slot == 3) ox = boss_ox + kBossW + 32;
        addUnit(ox, 6, 3);
      }
    }
    step_px_ = 2 + level_ / 80 + static_cast<int>(nextRng() % 2);
    if (step_px_ > 5) step_px_ = 5;
    centerFormation();
    origin_y_ = raid ? 46 : 52;
    form_dir_ = 1;
  }

  void placeMiniBoss() {
    unit_n_ = 0;
    boss_ = true;
    mini_ = true;
    raid_ = false;
    barrier_n_ = 0;
    boss_open_ = false;
    boss_layers_ = 0;
    boss_layer_max_ = 0;
    boss_style_ = 5;
    boss_max_ = 6 + level_ / 20;
    if (boss_max_ > 22) boss_max_ = 22;
    boss_hp_ = boss_max_;
    const int cols = 4 + level_ / 60;
    const int pitch = 30;
    for (int col = 0; col < cols; ++col) {
      addUnit(col * pitch, 44 + (col & 1) * 20, (col & 1) != 0 ? 1 : 0);
    }
    addUnit((cols * pitch - kMiniW) / 2, 0, 6);
    origin_x_ = (kPanelWidth - cols * pitch) / 2;
    origin_y_ = 38;
    form_dir_ = 1;
    step_px_ = 2 + level_ / 70;
    if (step_px_ > 5) step_px_ = 5;
    foe_gap_ = 28;
  }

  void buildLevel() {
    unit_n_ = 0;
    boss_ = (level_ % 5) == 0;
    mini_ = false;
    raid_ = false;
    god_ = 0;
    god_wave_ = 0;
    god_entrance_ = false;
    barrier_n_ = 0;
    boss_open_ = false;
    boss_tick_ = 0;
    laser_tick_ = 0;
    laser_live_ = false;
    sky_on_ = false;
    boss_shield_ = 0;
    boss_shield_max_ = 0;
    boss_layers_ = 0;
    boss_layer_max_ = 0;
    const int tier = (level_ - 1) / 20;
    drop_px_ = 8 + tier;
    if (drop_px_ > 20) drop_px_ = 20;
    foe_gap_ = 42 - level_ / 20;
    if (boss_) foe_gap_ -= 6;
    if (foe_gap_ < 14) foe_gap_ = 14;
    buildTerrain();
    if (isGodLevel(level_)) {
      buildGodArena();
      return;
    }
    if (!boss_ && raid_won_ && (nextRng() % 100) < 40) {
      placeMiniBoss();
      return;
    }
    if (boss_) {
      placeBoss(false);
      return;
    }
    const int shape = static_cast<int>(nextRng() % 5);
    int cols = 4 + static_cast<int>(nextRng() % 3);
    if (level_ > 80) cols += 1;
    if (level_ > 180) cols += static_cast<int>(nextRng() % 2);
    if (cols > 7) cols = 7;
    int rows = 2 + static_cast<int>(nextRng() % 2);
    if (level_ > 40) rows += 1;
    if (level_ > 120 && (nextRng() & 1) != 0) rows += 1;
    if (level_ > 220) rows += 1;
    if (rows > 5) rows = 5;
    const int pitch_x = 30;
    const int pitch_y = 22;
    const int yellow_rows = 1 + static_cast<int>(nextRng() % rows);
    step_px_ = 2 + level_ / 50 + static_cast<int>(nextRng() % 3);
    if (step_px_ > 7) step_px_ = 7;
    for (int row = 0; row < rows; ++row) {
      for (int col = 0; col < cols; ++col) {
        int ox = col * pitch_x;
        const int oy = row * pitch_y;
        bool keep = true;
        if (shape == 1) {
          if (col < row || col >= cols - row) keep = false;
        } else if (shape == 2) {
          if ((row & 1) != 0) ox += pitch_x / 2;
        } else if (shape == 3) {
          if ((row == 0 || row == rows - 1) && (col == 0 || col == cols - 1)) keep = false;
        } else if (shape == 4) {
          if (col == cols / 2) keep = false;
        }
        if (!keep) continue;
        int kind = row < yellow_rows ? 1 : 0;
        if ((nextRng() % 5) == 0) kind ^= 1;
        const uint32_t roll = nextRng() % 100;
        if (level_ >= 3 && roll < 6 + level_ / 25) kind = 4;
        else if (level_ >= 8 && roll >= 90 - level_ / 30) kind = 5;
        addUnit(ox, oy, kind);
      }
    }
    if (unit_n_ == 0) {
      addUnit(0, 0, 1);
    }
    int min_x = 999;
    int max_x = 0;
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].ox < min_x) min_x = units_[i].ox;
      const int right = units_[i].ox + kAlienW;
      if (right > max_x) max_x = right;
    }
    origin_x_ = (kPanelWidth - (max_x - min_x)) / 2 - min_x;
    origin_y_ = 34;
    form_dir_ = 1;
    if (level_ < 40) barrier_n_ = 2 + static_cast<int>(nextRng() % 2);
    else if (level_ < 140) barrier_n_ = static_cast<int>(nextRng() % 4);
    else barrier_n_ = static_cast<int>(nextRng() % 3);
    if (barrier_n_ > kMaxBarriers) barrier_n_ = kMaxBarriers;
    const int slot = barrier_n_ > 0 ? (kPanelWidth - barrier_n_ * 36) / (barrier_n_ + 1) : 0;
    for (int i = 0; i < barrier_n_; ++i) {
      barriers_[i].on = true;
      barriers_[i].x = slot + i * (36 + slot);
      barriers_[i].y = 292;
      const int notch = static_cast<int>(nextRng() % kBarCols);
      for (int col = 0; col < kBarCols; ++col) {
        uint8_t h = static_cast<uint8_t>(4 + (nextRng() % 3));
        if (col == notch) h = static_cast<uint8_t>(1 + (nextRng() % 2));
        barriers_[i].col[col] = h;
      }
    }
  }

  void paintLevel() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    paintStars();
    if (god_ != 0) paintGodArena();
    else {
      if (terrain_ == 1) paintPlanet();
      paintScraps();
    }
    if (sky_on_) drawSky();
    for (int i = 0; i < barrier_n_; ++i) {
      drawBarrier(i);
    }
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      drawAlienAt(origin_x_ + units_[i].ox, origin_y_ + units_[i].oy, units_[i].kind);
    }
    drawShipAt(player_x_);
    if (clone_alive_) {
      clone_drawn_ = true;
      drawShipAt(clone_x_);
    }
    paintDrones();
    if (aegis_hits_ > 0) {
      aegis_box_x_ = player_x_ - 3;
      aegis_box_ = true;
      drawRect(aegis_box_x_, kShipY - 3, kShipW + 6, kShipH + 6, color565(220, 230, 245));
    }
    if (buddy_) {
      buddy_x_ = player_x_ - 14;
      if (buddy_x_ < 4) buddy_x_ = player_x_ + kShipW + 4;
      buddy_drawn_ = true;
      drawBuddy();
    }
    if (drop_on_) drawDrop();
    paintCoins();
    drawHud();
    if (demo_) {
      // The attract loop shows one big PLAY key where the ship controls sit.
      fillRect(0, kButtonY, kPanelWidth, kPanelHeight - kButtonY, ink.bg);
      drawKey("PLAY", playBox(), false);
      return;
    }
    drawBack(false);
    drawGameKeys(nullptr);
  }

  void startLevel() {
    clearCoins();
    mode_ = Mode::Play;
    shot_ = false;
    clearFoe();
    held_ = nullptr;
    beam_on_ = false;
    beam_burst_ = 0;
    beam_cool_ = 0;
    gun_cool_ = 0;
    drop_on_ = false;
    sky_on_ = false;
    buddy_drawn_ = false;
    invuln_ = 0;
    clearBolts();
    player_x_ = (kPanelWidth - kShipW) / 2;
    resetMachines();
    buildLevel();
    paintLevel();
    // A checkpoint opens every 20 levels and is kept in flash.
    if (!demo_ && level_ > 0 && level_ % 20 == 0) {
      const int bit = level_ / 20 - 1;
      if (bit >= 0 && bit < 15) camp_ = static_cast<uint16_t>(camp_ | (1u << bit));
      if (perk(PERK_SHIELD_REPAIR) && shield_ < shieldCap()) {
        shield_ = static_cast<uint8_t>(shield_ + 1);
        saveShop();
      }
    }
    saveCampaign();
  }

  void beginGame() {
    fadeOut();
    score_ = 0;
    level_ = 1;
    lives_ = 3;
    clearFoe();
    foe_cool_ = 0;
    hud_msg_ticks_ = 0;
    raid_ = false;
    beam_burst_ = 0;
    beam_cool_ = 0;
    sky_on_ = false;
    rng_ = esp_random();
    if (rng_ == 0) rng_ = 1;
    startLevel();
    fadeIn();
  }

  void beginAdvance() {
    if (beam_on_) {
      eraseBeam();
      beam_on_ = false;
    }
    beam_burst_ = 0;
    for (int i = 0; i < kBolts; ++i) {
      if (!bolts_[i].on) continue;
      repair(bolts_[i].x, bolts_[i].y, bolts_[i].w, bolts_[i].h);
    }
    for (int i = 0; i < kFoeBolts; ++i) {
      if (!foes_[i].on) continue;
      repair(foes_[i].x, foes_[i].y, foes_[i].w, foes_[i].h);
    }
    clearFoe();
    clearCoins();
    saveShop();
    mode_ = Mode::Advance;
    shot_ = false;
    clearBolts();
    advance_at_ = millis() + 900;
    const Palette ink = palette();
    fillRect(16, 140, kPanelWidth - 32, 100, ink.bg);
    drawRect(16, 140, kPanelWidth - 32, 100, ink.accent);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("INVADERS MACHINA", kPanelWidth / 2, 164);
    char line[28];
    const int next = level_ + 1;
    if (next % 5 == 0) {
      const int style = ((next / 5) - 1) % 5;
      const char* gimmick = "BREAK SHIELD";
      if (style == 1) gimmick = "DROP DRONES";
      if (style == 2) gimmick = "HIT WHEN OPEN";
      if (style == 3) gimmick = "DODGE SPRAY";
      if (style == 4) gimmick = "DODGE LASER";
      snprintf(line, sizeof line, "BOSS %d", next);
      drawString(line, kPanelWidth / 2, 188);
      setTextColor(ink.accent);
      drawString(gimmick, kPanelWidth / 2, 212);
    } else {
      snprintf(line, sizeof line, "LEVEL %d", next);
      drawString(line, kPanelWidth / 2, 198);
    }
  }

  void showOver(const char* title, const char* killer) {
    fadeOut();
    clearCoins();
    saveShop();
    mode_ = Mode::Over;
    shot_ = false;
    clearFoe();
    beam_on_ = false;
    beam_burst_ = 0;
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.muted);
    drawString("INVADERS MACHINA", kPanelWidth / 2, 150);
    setTextColor(ink.ink);
    drawString(title, kPanelWidth / 2, 186);
    if (killer != nullptr) {
      char who[40];
      snprintf(who, sizeof who, "%s Killed you", killer);
      setTextColor(ink.accent);
      drawString(who, kPanelWidth / 2, 214);
    }
    char line[24];
    snprintf(line, sizeof line, "LV %d   %d", level_, score_);
    setTextColor(ink.muted);
    drawString(line, kPanelWidth / 2, 244);
    drawKey("HOME", homeBox(), false);
    fadeIn();
  }

  void endRound(bool won) {
    clearBoosts();
    if (won) showOver("CLEARED", nullptr);
    else showOver("YOU DIED", hit_by_);
  }

  // The ship blinks where it is; the level keeps its formation and progress.
  void blinkShip() {
    for (int i = 0; i < 3; ++i) {
      eraseShipAt(player_x_);
      delay(85);
      drawShipAt(player_x_);
      delay(85);
    }
    eraseShipAt(player_x_);
    drawShipAt(player_x_);
  }

  // If the formation is already on the ship line, lift it a little so the
  // player gets a fair chance instead of dying again the next tick.
  void liftFormation() {
    int bottom = 0;
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int y = origin_y_ + units_[i].oy + unitH(units_[i]);
      if (y > bottom) bottom = y;
    }
    if (bottom > kShipY - 28) origin_y_ -= bottom - (kShipY - 28);
  }

  void playerHit() {
    if (demo_) {
      lives_ = 3;
      startLevel();
      return;
    }
    if (invuln_ > 0 || boostOn(3)) return;
    const int inv = perk(PERK_INVULN) ? 100 : 80;
    clearFoe();
    shot_ = false;
    beam_on_ = false;
    beam_burst_ = 0;
    clearBolts();
    if (aegis_hits_ > 0) {
      aegis_hits_ -= 1;
      invuln_ = 36;
      liftFormation();
      paintLevel();
      blinkShip();
      drawHud();
      return;
    }
    if (shield_ > 0) {
      shield_ -= 1;
      invuln_ = inv;
      liftFormation();
      paintLevel();
      blinkShip();
      drawHud();
      saveShop();
      return;
    }
    lives_ -= 1;
    if (lives_ <= 0) {
      if (perk(PERK_REVIVE) && !revived_) {
        revived_ = true;
        lives_ = 1;
        tryBerserk();
        invuln_ = inv;
        liftFormation();
        paintLevel();
        blinkShip();
        drawHud();
        return;
      }
      endRound(false);
      return;
    }
    tryBerserk();
    invuln_ = inv;
    liftFormation();
    paintLevel();
    blinkShip();
    drawHud();
    saveCampaign();
  }

  void drawBack(bool held) { drawKey("BACK", backBox(), held); }


  bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) const {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
  }

  void addScore(int points) {
    score_ += points;
    if (score_ > 99999) score_ = 99999;
    drawHud();
  }

  void drawScrap(const Scrap& scrap) {
    const Palette ink = palette();
    if (terrain_ == 0) {
      fillRect(scrap.x, scrap.y, scrap.w, scrap.h, ink.barrier);
      if (scrap.w > 5 && scrap.h > 3) {
        fillRect(scrap.x + 2, scrap.y + 2, scrap.w / 2, scrap.h / 2, ink.muted);
      }
      return;
    }
    if (terrain_ == 1) {
      fillRect(scrap.x, scrap.y, scrap.w, 2, ink.planet);
      return;
    }
    if (terrain_ == 2) {
      fillRect(0, scrap.y, kPanelWidth, 2, ink.planet);
      return;
    }
    fillRect(scrap.x, scrap.y, scrap.w, 2, ink.top);
  }

  void paintScraps() {
    for (int i = 0; i < scrap_n_; ++i) drawScrap(scraps_[i]);
  }

  void buildTerrain() {
    terrain_ = static_cast<uint8_t>(level_ % 4);
    scrap_n_ = 0;
    const int count = terrain_ == 0 ? 5 : terrain_ == 3 ? 6 : 3;
    for (int i = 0; i < count && scrap_n_ < kScraps; ++i) {
      Scrap& scrap = scraps_[scrap_n_];
      scrap.x = static_cast<int16_t>(nextRng() % (kPanelWidth - 48));
      scrap.y = static_cast<int16_t>(48 + (nextRng() % 170));
      if (terrain_ == 0) {
        scrap.w = static_cast<uint8_t>(10 + (nextRng() % 10));
        scrap.h = static_cast<uint8_t>(6 + (nextRng() % 6));
      } else if (terrain_ == 1) {
        scrap.w = static_cast<uint8_t>(36 + (nextRng() % 48));
        scrap.h = 2;
      } else if (terrain_ == 2) {
        scrap.x = 0;
        scrap.y = static_cast<int16_t>(64 + i * 58);
        scrap.w = 8;
        scrap.h = 2;
      } else {
        scrap.w = static_cast<uint8_t>(6 + (nextRng() % 12));
        scrap.h = 2;
      }
      scrap_n_ += 1;
    }
  }

  bool starBlocked(int x, int y) const {
    if (y < 28 || y >= kShipY - 8) return true;
    for (int i = 0; i < barrier_n_; ++i) {
      if (!barriers_[i].on) continue;
      if (x >= barriers_[i].x && x < barriers_[i].x + kBarCols * 3 && y >= barriers_[i].y &&
          y < barriers_[i].y + 21) {
        return true;
      }
    }
    return false;
  }

  void stepSpace() {
    space_tick_ += 1;
    stepSky();
    const Palette ink = palette();
    for (int i = 0; i < kStars; ++i) {
      Star& star = stars_[i];
      const int speed = star.layer ? 2 : 1;
      if (star.layer == 0 && (space_tick_ & 1) != 0) continue;
      const int ox = star.x;
      const int oy = star.y;
      if (!starBlocked(ox, oy)) drawPixel(ox, oy, ink.bg);
      int ny = oy + speed;
      int nx = ox;
      if (ny >= kShipY - 8) {
        ny = 28 + static_cast<int>(nextRng() % 24);
        nx = static_cast<int>(nextRng() % kPanelWidth);
      }
      star.x = static_cast<int16_t>(nx);
      star.y = static_cast<int16_t>(ny);
      if (!starBlocked(nx, ny)) drawPixel(nx, ny, star.layer ? ink.star : ink.muted);
    }
    const bool move_small = (space_tick_ & 1) == 0;
    for (int i = 0; i < scrap_n_; ++i) {
      if (terrain_ == 2) {
        if (i != ((space_tick_ / 2) % scrap_n_)) continue;
      } else if (!move_small) {
        continue;
      }
      Scrap& scrap = scraps_[i];
      int ox = scrap.x;
      int ow = scrap.w;
      int oh = scrap.h > 0 ? scrap.h : 2;
      if (terrain_ == 2) {
        ox = 0;
        ow = kPanelWidth;
        oh = 2;
      }
      repair(ox, scrap.y, ow, oh);
      int ny = scrap.y + 1;
      if (ny > 248) ny = 42 + static_cast<int>(nextRng() % 28);
      scrap.y = static_cast<int16_t>(ny);
      drawScrap(scrap);
    }
  }

  void clearFoe() {
    for (int i = 0; i < kFoeBolts; ++i) foes_[i].on = false;
  }

  void drawBuddy() {
    fillRect(buddy_x_ + 2, kShipY + 4, 6, 4, palette().accent);
    fillRect(buddy_x_, kShipY + 6, 10, 2, palette().ship);
  }

  void stepBuddy() {
    if (!buddy_) return;
    int want = player_x_ - 14;
    if (want < 4) want = player_x_ + kShipW + 4;
    if (want > kPanelWidth - 14) want = kPanelWidth - 14;
    if (buddy_drawn_ && buddy_x_ != want) repair(buddy_x_, kShipY + 2, 12, 10);
    buddy_x_ = want;
    buddy_drawn_ = true;
    drawBuddy();
    if (buddy_cool_ > 0) {
      buddy_cool_ -= 1;
      return;
    }
    buddy_cool_ = 18;
    spawnBoltAt(buddy_x_ + 5, kShipY - 2, 0, -8, 2, 6, 0);
  }

  int nextLockedGun() const {
    for (int gun = 1; gun < kGunCount; ++gun) {
      if (!gunUnlocked(gun)) return gun;
    }
    return 0;
  }

  void spawnDropAt(int which, int x, int y) {
    drop_on_ = true;
    drop_which_ = static_cast<uint8_t>(which);
    if (x < 16) x = 16;
    if (x > kPanelWidth - 32) x = kPanelWidth - 32;
    if (y < 40) y = 40;
    if (y > kShipY - 40) y = kShipY - 40;
    drop_x_ = x;
    drop_y_ = y;
    drawDrop();
  }

  void spawnDrop(int which) {
    spawnDropAt(which, static_cast<int>(16 + (nextRng() % (kPanelWidth - 48))), 40);
  }

  uint16_t boostLen(int i) const {
    static const uint16_t kLen[kBoosts] = {500, 500, 500, 250, 500};
    if (i < 0 || i >= kBoosts) return 0;
    return kLen[i];
  }

  int boostPrice(int i) const {
    static const int kPrice[kBoosts] = {20, 25, 15, 40, 20};
    if (i < 0 || i >= kBoosts) return 0;
    return kPrice[i];
  }

  const char* boostName(int i) const {
    static const char* names[kBoosts] = {"RUSH", "FURY", "WIDE", "VEIL", "SALVO"};
    if (i < 0 || i >= kBoosts) return "";
    return names[i];
  }

  bool boostOn(int i) const { return i >= 0 && i < kBoosts && boost_ticks_[i] > 0; }

  void clearBoosts() {
    for (int i = 0; i < kBoosts; ++i) boost_ticks_[i] = 0;
  }

  void boostHud(char* buf, int n) const {
    if (n < 1) return;
    buf[0] = '\0';
    int used = 0;
    for (int i = 0; i < kBoosts; ++i) {
      if (!boostOn(i)) continue;
      const char* name = boostName(i);
      const int len = static_cast<int>(std::strlen(name));
      if (used + (used > 0 ? 1 : 0) + len >= n) break;
      if (used > 0) buf[used++] = ' ';
      for (int c = 0; c < len; ++c) buf[used++] = name[c];
      buf[used] = '\0';
    }
  }

  void stepBoosts() {
    bool ended = false;
    for (int i = 0; i < kBoosts; ++i) {
      if (boost_ticks_[i] == 0) continue;
      boost_ticks_[i] = static_cast<uint16_t>(boost_ticks_[i] - 1);
      if (boost_ticks_[i] == 0) ended = true;
    }
    if (ended && hud_msg_ticks_ == 0) drawHud();
  }

  bool rollTempBoost(bool placed, int x, int y) {
    static const uint8_t kRate[kBoosts] = {8, 12, 5, 3, 15};
    const int start = static_cast<int>(nextRng() % kBoosts);
    for (int n = 0; n < kBoosts; ++n) {
      const int i = (start + n) % kBoosts;
      if ((nextRng() % 100) >= kRate[i]) continue;
      if (placed) spawnDropAt(12 + i, x, y);
      else spawnDrop(12 + i);
      return true;
    }
    return false;
  }

  void maybeMiniDrop(int x, int y) {
    if (drop_on_) return;
    if ((nextRng() % 100) < 5) {
      spawnDropAt(11, x, y);
      return;
    }
    const int which = nextLockedGun();
    if (which != 0 && (nextRng() % 10) == 0) {
      spawnDropAt(which, x, y);
      return;
    }
    if (rollTempBoost(true, x, y)) return;
    const uint32_t roll = nextRng() % 100;
    const uint32_t heart = 35 + (perk(PERK_HEAL) ? 15 : 0);
    if (roll < heart) spawnDropAt(9, x, y);
    else spawnDropAt(10, x, y);
  }

  void maybeDrop(bool boss_kill) {
    if (drop_on_) return;
    if (boss_kill) {
      const int which = nextLockedGun();
      if (which != 0 && (nextRng() % 10) == 0) {
        spawnDrop(which);
        return;
      }
    }
    if (rollTempBoost(false, 0, 0)) return;
    const uint32_t roll = nextRng() % 100;
    const uint32_t heart = 5 + (perk(PERK_HEAL) ? 10 : 0);
    if (roll < heart) spawnDrop(9);
    else if (roll < 20) spawnDrop(10);
  }

  void drawDrop() {
    uint16_t color = color565(255, 220, 80);
    if (drop_which_ == 9) color = color565(255, 64, 96);
    else if (drop_which_ == 10) color = color565(255, 210, 48);
    else if (drop_which_ == 11) color = color565(96, 200, 255);
    else if (drop_which_ >= 12 && drop_which_ < 12 + kBoosts) {
      static const uint8_t kR[kBoosts] = {255, 255, 80, 230, 255};
      static const uint8_t kG[kBoosts] = {140, 60, 220, 230, 80};
      static const uint8_t kB[kBoosts] = {32, 48, 255, 255, 220};
      const int i = drop_which_ - 12;
      color = color565(kR[i], kG[i], kB[i]);
    } else if (drop_which_ >= 1 && drop_which_ <= 8) {
      static const uint8_t kR[8] = {255, 80, 80, 255, 80, 255, 255, 180};
      static const uint8_t kG[8] = {150, 255, 120, 80, 180, 80, 140, 80};
      static const uint8_t kB[8] = {40, 120, 255, 255, 255, 255, 40, 255};
      const int i = drop_which_ - 1;
      color = color565(kR[i], kG[i], kB[i]);
    }
    fillRect(drop_x_, drop_y_, 14, 14, color);
    fillRect(drop_x_ + 4, drop_y_ + 4, 6, 6, palette().bg);
  }

  void noteUnlock(const char* name) {
    snprintf(hud_msg_, sizeof hud_msg_, "%s", name);
    hud_msg_ticks_ = 45;
    drawHud();
  }

  void collectDrop() {
    if (drop_which_ >= 1 && drop_which_ <= 8) {
      gun_unlock_ = static_cast<uint8_t>(gun_unlock_ | (1u << (drop_which_ - 1)));
      gun_equip_ = drop_which_;
      saveGear();
      noteUnlock(gunToken(drop_which_));
    } else if (drop_which_ == 9) {
      const int cap = perk(PERK_MAXHP) ? 9 : 8;
      if (lives_ < cap) lives_ += 1;
      noteUnlock("HEART");
    } else if (drop_which_ == 10) {
      const int gun = gun_equip_;
      if (gun >= 0 && gun < kGunSlots && gun_lv_[gun] < kGunMaxLv) {
        gun_lv_[gun] = static_cast<uint8_t>(gun_lv_[gun] + 1);
        saveGear();
        noteUnlock("GUN +1");
      } else {
        noteUnlock("MAX");
      }
    } else if (drop_which_ == 11) {
      if (shield_ < shieldCap()) {
        shield_ = static_cast<uint8_t>(shield_ + 1);
        saveShop();
      }
      noteUnlock("SHIELD");
    } else if (drop_which_ >= 12 && drop_which_ < 12 + kBoosts) {
      const int i = drop_which_ - 12;
      boost_ticks_[i] = boostLen(i);
      noteUnlock(boostName(i));
    }
    drop_on_ = false;
  }

  void beginRaid(bool fresh) {
    if (beam_on_) {
      eraseBeam();
      beam_on_ = false;
    }
    beam_burst_ = 0;
    clearBolts();
    clearFoe();
    drop_on_ = false;
    sky_on_ = false;
    laser_live_ = false;
    if (fresh) {
      const int jump = 1 + static_cast<int>(nextRng() % 4);
      boss_style_ = static_cast<uint8_t>((boss_style_ + jump) % 5);
    }
    placeBoss(true);
    mode_ = Mode::Play;
    paintLevel();
    if (fresh) noteUnlock("RAID");
  }

  void drawSky() {
    if (!sky_on_) return;
    const Palette ink = palette();
    if (sky_kind_ == 0) {
      fillCircle(sky_x_, sky_y_, sky_r_, ink.planet);
      fillCircle(sky_x_ - sky_r_ / 3, sky_y_ - sky_r_ / 4, sky_r_ / 4, ink.muted);
    } else {
      drawCircle(sky_x_, sky_y_, sky_r_, ink.muted);
      drawCircle(sky_x_, sky_y_, (sky_r_ * 2) / 3, ink.accent);
      fillCircle(sky_x_, sky_y_, sky_r_ / 3, ink.bg);
    }
  }

  void stepSky() {
    if (mode_ != Mode::Play) return;
    if (sky_on_) {
      const int ox = sky_x_ - sky_r_ - 1;
      const int oy = sky_y_ - sky_r_ - 1;
      const int d = sky_r_ * 2 + 4;
      sky_y_ += 2;
      if (sky_y_ > 240) {
        sky_on_ = false;
        repair(ox, oy, d, d);
        return;
      }
      repair(ox, oy, d, d);
      drawSky();
      return;
    }
    if ((space_tick_ % 180) != 0 || (nextRng() % 3) != 0) return;
    sky_on_ = true;
    sky_kind_ = static_cast<uint8_t>(nextRng() & 1);
    sky_r_ = 14 + static_cast<int>(nextRng() % 9);
    sky_x_ = 24 + static_cast<int>(nextRng() % (kPanelWidth - 48));
    sky_y_ = 56 + sky_r_;
    drawSky();
  }


  int beamBurstLen() const {
    int len = 188 + gunLv(gun_equip_) * 12;
    len += 30 * forgedCount(ATTR_BURST);
    if (perk(PERK_BURST)) len += 30;
    len += 20 * forgedCount(ATTR_TWIN);
    if (perk(PERK_TWIN)) len += 20;
    return len;
  }

  int beamCoolLen() const {
    int cool = 80 - gunLv(gun_equip_) * 8;
    cool -= 20 * forgedCount(ATTR_COOL);
    if (perk(PERK_COOL)) cool -= 20;
    cool -= 10 * forgedCount(ATTR_RAPID);
    if (perk(PERK_RAPID)) cool -= 10;
    if (boostOn(0)) cool -= 20;
    if (cool < 16) cool = 16;
    return cool;
  }

  void stepDrop() {
    if (!drop_on_) return;
    const int ox = drop_x_;
    const int oy = drop_y_;
    const int ny = drop_y_ + 3;
    if (overlap(drop_x_, ny, 14, 14, player_x_, kShipY, kShipW, kShipH)) {
      repair(ox, oy, 14, 14);
      collectDrop();
      drawShipAt(player_x_);
      return;
    }
    if (ny > kShipY + 8) {
      repair(ox, oy, 14, 14);
      drop_on_ = false;
      return;
    }
    drop_y_ = ny;
    drawDrop();
    uncover(ox, oy, drop_x_, drop_y_, 14, 14);
  }

  void clearBolts() {
    for (int i = 0; i < kBolts; ++i) bolts_[i].on = false;
  }

  bool spawnBoltAt(int cx, int y, int vx, int vy, int w, int h, int tag) {
    if (perk(PERK_WIDE)) w += 1;
    if (boostOn(2)) w += 2;
    if (equippedForged() != nullptr) {
      w += forgedCount(ATTR_WIDE);
      const int pwr = forgedCount(ATTR_POWER);
      h += (pwr > 3 ? 6 : pwr * 2);
      if (forgedHas(ATTR_SPREAD)) {
        vx += spread_side_ ? 2 : -2;
        spread_side_ = !spread_side_;
      }
      if (tag == 0 && forgedHas(ATTR_HOMING)) tag = 3;
      else if (tag == 0 && forgedHas(ATTR_WAVE)) tag = 1;
    }
    for (int i = 0; i < kBolts; ++i) {
      if (bolts_[i].on) continue;
      bolts_[i].on = true;
      bolts_[i].w = static_cast<uint8_t>(w);
      bolts_[i].h = static_cast<uint8_t>(h);
      bolts_[i].x = static_cast<int16_t>(cx - w / 2);
      bolts_[i].y = static_cast<int16_t>(y);
      bolts_[i].vx = static_cast<int8_t>(vx);
      bolts_[i].vy = static_cast<int8_t>(vy);
      bolts_[i].tag = static_cast<uint8_t>(tag);
      bolts_[i].pierce = static_cast<uint8_t>(
          forgedCount(ATTR_PIERCE) + (perk(PERK_PIERCE) ? 1 : 0));
      bolts_[i].last = -1;
      fillRect(bolts_[i].x, bolts_[i].y, w, h, laserColor());
      return true;
    }
    return false;
  }

  int boltSlots() const {
    int free = 0;
    for (int i = 0; i < kBolts; ++i) {
      if (!bolts_[i].on) free += 1;
    }
    return free;
  }

  int fireGap() const {
    const int gun = baseGun();
    const int lv = gunLv(gun_equip_);
    int gap;
    if (gun == 1) gap = 10 - lv;
    else if (gun == 2) gap = 3 - lv > 2 ? 3 - lv : 2;
    else if (gun == 4) gap = 5 - lv > 2 ? 5 - lv : 2;
    else if (gun == 5) gap = 6 - lv > 2 ? 6 - lv : 2;
    else if (gun == 6) gap = 14 - lv * 2 > 5 ? 14 - lv * 2 : 5;
    else if (gun == 7) gap = 12 - lv * 2 > 5 ? 12 - lv * 2 : 5;
    else if (gun == 8) gap = 9 - lv > 4 ? 9 - lv : 4;
    else gap = 4 - lv > 2 ? 4 - lv : 2;
    gap -= 2 * forgedCount(ATTR_RAPID);
    if (perk(PERK_RAPID)) gap -= 1;
    if (boostOn(0)) gap -= 2;
    return gap < 2 ? 2 : gap;
  }

  void spawnForgeExtras(int cx, int lv) {
    const int twins = forgedCount(ATTR_TWIN) + (perk(PERK_TWIN) ? 1 : 0);
    if (twins >= 1) spawnBoltAt(cx, kShipY - 8, 0, -8 - lv, 3, 6, 0);
    if (twins >= 2 || forgedCount(ATTR_SPREAD) >= 2) {
      spawnBoltAt(cx - 8, kShipY - 8, -2, -8 - lv, 3, 6, 0);
      spawnBoltAt(cx + 8, kShipY - 8, 2, -8 - lv, 3, 6, 0);
    }
    if (perk(PERK_SALVO) || boostOn(4)) {
      spawnBoltAt(cx - 6, kShipY - 8, -1, -8 - lv, 3, 6, 0);
      spawnBoltAt(cx + 6, kShipY - 8, 1, -8 - lv, 3, 6, 0);
    }
  }

  void tryFire() {
    const int gun = baseGun();
    if (gun == 3 || gun_cool_ > 0) return;
    const int lv = gunLv(gun_equip_);
    const int cx = player_x_ + kShipW / 2;
    if (gun == 1) {
      if (boltSlots() < 3) return;
      const int kick = lv / 2;
      spawnBoltAt(cx, kShipY - 7, -2, -7 - kick, 3, 6, 0);
      spawnBoltAt(cx, kShipY - 7, 0, -8 - kick, 3, 6, 0);
      spawnBoltAt(cx, kShipY - 7, 2, -7 - kick, 3, 6, 0);
      if (lv >= 3) spawnBoltAt(cx, kShipY - 7, 0, -9, 3, 6, 0);
      spawnForgeExtras(cx, lv);
      gun_cool_ = fireGap();
      return;
    }
    if (gun == 4) {
      spawnBoltAt(player_x_ + 4, kShipY - 10, 0, -9 - lv, 3, 8, 0);
      spawnBoltAt(player_x_ + kShipW - 4, kShipY - 10, 0, -9 - lv, 3, 8, 0);
      spawnForgeExtras(cx, lv);
      gun_cool_ = fireGap();
      return;
    }
    if (gun == 5) {
      if (spawnBoltAt(cx, kShipY - 10, 2, -8 - lv / 2, 3, 8, 1)) {
        spawnForgeExtras(cx, lv);
        gun_cool_ = fireGap();
      }
      return;
    }
    if (gun == 6) {
      if (spawnBoltAt(cx, kShipY - 16, 0, -16, 2, 16, 2)) {
        spawnForgeExtras(cx, lv);
        gun_cool_ = fireGap();
      }
      return;
    }
    if (gun == 7) {
      if (boltSlots() < 5) return;
      const int vxs[5] = {-3, -1, 0, 1, 3};
      for (int i = 0; i < 5; ++i) {
        spawnBoltAt(cx, kShipY - 8, vxs[i], -8 - lv / 2, 3, 6, 0);
      }
      spawnForgeExtras(cx, lv);
      gun_cool_ = fireGap();
      return;
    }
    if (gun == 8) {
      if (spawnBoltAt(cx, kShipY - 10, 0, -7, 3, 8, 3)) {
        spawnForgeExtras(cx, lv);
        gun_cool_ = fireGap();
      }
      return;
    }
    const int vy = gun == 2 ? -12 - lv : -9 - lv;
    const int h = gun == 2 ? 10 : 8;
    if (spawnBoltAt(cx, kShipY - h - 1, 0, vy, 3, h, 0)) {
      spawnForgeExtras(cx, lv);
      gun_cool_ = fireGap();
    }
  }

  void steerBolt(Bolt& bolt) {
    if (bolt.tag == 1) {
      if ((space_tick_ & 3) == 0) bolt.vx = static_cast<int8_t>(-bolt.vx);
      if (bolt.vx == 0) bolt.vx = 2;
      return;
    }
    if (bolt.tag == 4) {
      int best = -1;
      int best_rank = -1;
      for (int u = 0; u < unit_n_; ++u) {
        if (!units_[u].alive) continue;
        const int ay = origin_y_ + units_[u].oy;
        if (ay > bolt.y + 8) continue;
        int rank = 1 + units_[u].hp;
        if (units_[u].kind == 2) rank += 1000;
        else if (units_[u].kind == 6) rank += 400;
        else if (units_[u].kind == 7) rank += 40;
        if (rank > best_rank) {
          best_rank = rank;
          best = origin_x_ + units_[u].ox + unitW(units_[u]) / 2;
        }
      }
      if (best < 0) return;
      const int cx = bolt.x + bolt.w / 2;
      if (best < cx - 2 && bolt.vx > -4) bolt.vx = static_cast<int8_t>(bolt.vx - 1);
      else if (best > cx + 2 && bolt.vx < 4) bolt.vx = static_cast<int8_t>(bolt.vx + 1);
      return;
    }
    if (bolt.tag != 3) return;
    int best = -1;
    int best_d = 9999;
    const int cx = bolt.x + bolt.w / 2;
    for (int u = 0; u < unit_n_; ++u) {
      if (!units_[u].alive) continue;
      const int ax = origin_x_ + units_[u].ox + unitW(units_[u]) / 2;
      const int ay = origin_y_ + units_[u].oy;
      if (ay > bolt.y) continue;
      int dist = ax - cx;
      if (dist < 0) dist = -dist;
      dist += (bolt.y - ay) / 4;
      if (dist < best_d) {
        best_d = dist;
        best = ax;
      }
    }
    if (best < 0) return;
    if (best < cx - 2 && bolt.vx > -4) bolt.vx = static_cast<int8_t>(bolt.vx - 1);
    else if (best > cx + 2 && bolt.vx < 4) bolt.vx = static_cast<int8_t>(bolt.vx + 1);
  }

  void stepBolts() {
    for (int i = 0; i < kBolts; ++i) {
      Bolt& bolt = bolts_[i];
      if (!bolt.on) continue;
      steerBolt(bolt);
      const int ox = bolt.x;
      const int oy = bolt.y;
      const int nx = bolt.x + bolt.vx;
      const int ny = bolt.y + bolt.vy;
      const bool miss = ny < 26 || nx < -8 || nx > kPanelWidth;
      if (miss || hitBarrier(nx, ny, bolt.w, bolt.h)) {
        repair(ox, oy, bolt.w, bolt.h);
        bolt.on = false;
        continue;
      }
      if (bolt.tag == 2) {
        pierceUnit(bolt, nx, ny);
        bolt.x = static_cast<int16_t>(nx);
        bolt.y = static_cast<int16_t>(ny);
        fillRect(nx, ny, bolt.w, bolt.h, laserColor());
        uncover(ox, oy, nx, ny, bolt.w, bolt.h);
        continue;
      }
      if (bolt.pierce > 0 && pierceUnit(bolt, nx, ny)) {
        bolt.pierce -= 1;
        if (bolt.pierce == 0) {
          repair(ox, oy, bolt.w, bolt.h);
          bolt.on = false;
          continue;
        }
        bolt.x = static_cast<int16_t>(nx);
        bolt.y = static_cast<int16_t>(ny);
        fillRect(nx, ny, bolt.w, bolt.h, laserColor());
        uncover(ox, oy, nx, ny, bolt.w, bolt.h);
        continue;
      }
      if (hitUnitBox(nx, ny, bolt.w, bolt.h)) {
        repair(ox, oy, bolt.w, bolt.h);
        bolt.on = false;
        continue;
      }
      bolt.x = static_cast<int16_t>(nx);
      bolt.y = static_cast<int16_t>(ny);
      fillRect(nx, ny, bolt.w, bolt.h, laserColor());
      uncover(ox, oy, nx, ny, bolt.w, bolt.h);
    }
  }

  bool pierceUnit(Bolt& bolt, int sx, int sy) {
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive || bolt.last == i) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      const int aw = unitW(units_[i]);
      const int ah = unitH(units_[i]);
      if (!overlap(sx, sy, bolt.w, bolt.h, ax, ay, aw, ah)) continue;
      bolt.last = static_cast<int8_t>(i);
      applyHit(i);
      if (!units_[i].alive) repair(ax, ay, aw, ah);
      else drawAlienAt(ax, ay, units_[i].kind);
      return true;
    }
    return false;
  }

  bool applyHit(int index) {
    Unit& unit = units_[index];
    if (unit.kind == 6) {
      if (boss_hp_ > 0) boss_hp_ -= playerDamage();
      addScore(20);
      if (boss_hp_ <= 0) {
        unit.alive = false;
        const int cx = origin_x_ + unit.ox + unitW(unit) / 2;
        const int cy = origin_y_ + unit.oy + unitH(unit) / 2;
        dropCoins(cx - kCoinW / 2, cy - kCoinW / 2, 2);
        maybeMiniDrop(cx - 7, cy);
        return true;
      }
      drawAlienAt(origin_x_ + unit.ox, origin_y_ + unit.oy, unit.kind);
      drawHud();
      return false;
    }
    if (unit.kind == 7) {
      addScore(10);
      const int dmg = playerDamage();
      if (unit.hp > dmg) {
        unit.hp = static_cast<uint8_t>(unit.hp - dmg);
        drawAlienAt(origin_x_ + unit.ox, origin_y_ + unit.oy, unit.kind);
        return false;
      }
      unit.hp = 0;
      unit.alive = false;
      if ((nextRng() % 100) < 20) {
        spawnCoin(origin_x_ + unit.ox + unitW(unit) / 2 - kCoinW / 2,
                  origin_y_ + unit.oy + unitH(unit) / 2 - kCoinW / 2);
      }
      return true;
    }
    if (unit.kind == 2) {
      if (god_ != 0 && god_entrance_) {
        // Three bars, three styles. Emptying a bar advances the phase.
        addScore(10);
        if (god_hp_ > 0) god_hp_ -= playerDamage();
        if (god_hp_ <= 0) {
          if (god_bar_ > 1) {
            god_bar_ = static_cast<uint8_t>(god_bar_ - 1);
            god_phase_ = static_cast<uint8_t>(3 - god_bar_);
            god_hp_max_ = godBarHp() + (3 - god_bar_) * 2;
            god_hp_ = god_hp_max_;
            clearFoe();
            noteUnlock("PHASE");
          } else {
            const int slain = god_;
            unit.alive = false;
            for (int u = 0; u < unit_n_; ++u) {
              if (u != index && units_[u].alive) {
                units_[u].alive = false;
                repair(origin_x_ + units_[u].ox, origin_y_ + units_[u].oy,
                       unitW(units_[u]), unitH(units_[u]));
              }
            }
            boss_ = false;
            god_ = 0;
            god_entrance_ = false;
            god_ward_ = 0;
            god_beam_ticks_ = 0;
            grantDivine(slain);
            dropCoins(origin_x_ + unit.ox + kBossW / 2 - kCoinW / 2, origin_y_ + unit.oy + kBossH / 2 - kCoinW / 2, 10);
            maybeDrop(true);
            return true;
          }
        }
        drawAlienAt(origin_x_ + unit.ox, origin_y_ + unit.oy, unit.kind);
        drawHud();
        return false;
      }
      if (anyMinion()) return false;
      if (boss_style_ == 2 && !boss_open_) return false;
      if (boss_layers_ > 0) {
        if (boss_shield_ > 0) boss_shield_ = static_cast<uint8_t>(boss_shield_ - 1);
        addScore(10);
        if (boss_shield_ == 0) {
          boss_layers_ = static_cast<uint8_t>(boss_layers_ - 1);
          if (boss_layers_ > 0) {
            boss_shield_ = boss_layer_max_;
            boss_shield_max_ = boss_layer_max_;
          }
        }
        drawAlienAt(origin_x_ + unit.ox, origin_y_ + unit.oy, unit.kind);
        return false;
      }
      boss_hp_ -= playerDamage();
      addScore(10);
      if (boss_hp_ <= 0) {
        unit.alive = false;
        if (raid_) {
          raid_won_ = true;
          saveCampaign();
        }
        dropCoins(origin_x_ + unit.ox + unitW(unit) / 2 - kCoinW / 2, origin_y_ + unit.oy + unitH(unit) / 2 - kCoinW / 2, raid_ ? 4 : 3);
        maybeDrop(true);
        return true;
      }
      drawAlienAt(origin_x_ + unit.ox, origin_y_ + unit.oy, unit.kind);
      return false;
    }
    unit.alive = false;
    addScore(10);
    if ((nextRng() % 100) < 20) {
      spawnCoin(origin_x_ + unit.ox + unitW(unit) / 2 - kCoinW / 2, origin_y_ + unit.oy + unitH(unit) / 2 - kCoinW / 2);
    }
    maybeDrop(false);
    return true;
  }

  void eraseBeam() {
    repair(beam_x_, 36, 3, kShipY - 36);
    for (int i = 0; i < scrap_n_; ++i) {
      const Scrap& scrap = scraps_[i];
      const int sx = terrain_ == 2 ? 0 : scrap.x;
      const int sw = terrain_ == 2 ? kPanelWidth : scrap.w;
      const int sh = terrain_ == 2 ? 2 : scrap.h;
      if (!overlap(beam_x_, 36, 3, kShipY - 36, sx, scrap.y, sw, sh)) continue;
      if (terrain_ == 2) fillRect(beam_x_, scrap.y, 3, 2, palette().planet);
      else drawScrap(scrap);
    }
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      if (overlap(beam_x_, 36, 3, kShipY - 36, ax, ay, unitW(units_[i]), unitH(units_[i]))) {
        drawAlienAt(ax, ay, units_[i].kind);
      }
    }
    for (int i = 0; i < barrier_n_; ++i) {
      if (!barriers_[i].on) continue;
      if (overlap(beam_x_, 36, 3, kShipY - 36, barriers_[i].x, barriers_[i].y, kBarCols * 3, 21)) {
        drawBarrier(i);
      }
    }
    if (drop_on_ && overlap(beam_x_, 36, 3, kShipY - 36, drop_x_, drop_y_, 14, 14)) drawDrop();
    if (sky_on_ && overlap(beam_x_, 36, 3, kShipY - 36, sky_x_ - sky_r_, sky_y_ - sky_r_, sky_r_ * 2, sky_r_ * 2)) {
      drawSky();
    }
    if (buddy_drawn_) drawBuddy();
    drawShipAt(player_x_);
  }

  void hurtAlongBeam() {
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      const int aw = unitW(units_[i]);
      const int ah = unitH(units_[i]);
      if (!overlap(beam_x_, 36, 3, kShipY - 36, ax, ay, aw, ah)) continue;
      applyHit(i);
      if (!units_[i].alive) repair(ax, ay, aw, ah);
      else drawAlienAt(ax, ay, units_[i].kind);
    }
  }

  void paintForgedPage() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("FORGED", kPanelWidth / 2, 22);
    int row = 0;
    for (int i = 0; i < kMaxForged; ++i) {
      if (!forged_[i].valid) continue;
      char line[32];
      gunLine(kGunCount + i, line, sizeof line);
      drawCard(line, nullptr, forgedListBox(row));
      row += 1;
    }
    if (row == 0) {
      setTextColor(ink.muted);
      drawString("NONE YET", kPanelWidth / 2, 160);
      drawString("COMBINE TWO GUNS IN FORGE", kPanelWidth / 2, 188);
    }
    drawKey("BACK", homeBox(), false);
  }

  void paintCustom() {
    if (custom_page_ != 0) {
      paintForgedPage();
      return;
    }
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("CUSTOM", kPanelWidth / 2, 22);
    drawKey("FORGED", customForgeBox(), false);
    char color_line[24];
    snprintf(color_line, sizeof color_line, "LASER %s", laserName());
    drawCard(color_line, "tap to change", colorBox());
    char ship_line[24];
    snprintf(ship_line, sizeof ship_line, "SHIP %s", shipName());
    drawCard(ship_line, "tap to change", shipBox());
    for (int gun = 0; gun < kGunCount; ++gun) {
      char line[32];
      gunLine(gun, line, sizeof line);
      drawCard(line, nullptr, gunBox(gun));
    }
    drawKey("BACK", homeBox(), false);
  }

  void beginCustom(bool from_pause) {
    custom_from_pause_ = from_pause;
    custom_page_ = 0;
    fadeOut();
    mode_ = Mode::Custom;
    paintCustom();
    fadeIn();
  }

  void beginPause() {
    const bool keep_advance = mode_ == Mode::Custom && pause_advance_;
    if (beam_on_) {
      eraseBeam();
      beam_on_ = false;
    }
    if (beam_burst_ > 0) {
      beam_burst_ = 0;
      beam_cool_ = beamCoolLen();
    }
    for (int i = 0; i < kBolts; ++i) {
      if (!bolts_[i].on) continue;
      repair(bolts_[i].x, bolts_[i].y, bolts_[i].w, bolts_[i].h);
      bolts_[i].on = false;
    }
    if (mode_ == Mode::Advance) pause_advance_ = true;
    else if (!keep_advance) pause_advance_ = false;
    mode_ = Mode::Pause;
    const Palette ink = palette();
    fillRect(24, 128, 272, 230, ink.bg);
    drawRect(24, 128, 272, 230, ink.accent);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("INVADERS MACHINA", kPanelWidth / 2, 152);
    drawCard("RESUME", nullptr, pauseResumeBox());
    drawCard("CUSTOM", "ship and guns", pauseCustomBox());
    drawCard("HOME", nullptr, pauseHomeBox());
  }

  void leaveCustom() {
    if (custom_from_pause_) beginPause();
    else beginMenu();
  }

  void resumePlay() {
    if (pause_advance_) {
      beginAdvance();
      return;
    }
    mode_ = Mode::Play;
    beam_on_ = false;
    beam_burst_ = 0;
    clearBolts();
    clearFoe();
    paintLevel();
  }

  bool hitBarrier(int sx, int sy, int sw, int sh) {
    for (int i = 0; i < barrier_n_; ++i) {
      if (!barriers_[i].on) continue;
      const int bw = kBarCols * 3;
      const int bh = 7 * 3;
      if (!overlap(sx, sy, sw, sh, barriers_[i].x, barriers_[i].y, bw, bh)) {
        continue;
      }
      int col = (sx - barriers_[i].x) / 3;
      if (col < 0) col = 0;
      if (col >= kBarCols) col = kBarCols - 1;
      if (barriers_[i].col[col] == 0) continue;
      if (sy > barriers_[i].y + barriers_[i].col[col] * 3) continue;
      barriers_[i].col[col] = static_cast<uint8_t>(barriers_[i].col[col] - 1);
      repair(barriers_[i].x, barriers_[i].y, bw, bh);
      drawBarrier(i);
      return true;
    }
    return false;
  }

  bool hitUnitBox(int sx, int sy, int sw, int sh) {
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      const int aw = unitW(units_[i]);
      const int ah = unitH(units_[i]);
      if (!overlap(sx, sy, sw, sh, ax, ay, aw, ah)) continue;
      applyHit(i);
      if (!units_[i].alive) repair(ax, ay, aw, ah);
      else drawAlienAt(ax, ay, units_[i].kind);
      return true;
    }
    return false;
  }

  bool anyAlive() const {
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].alive) return true;
    }
    return false;
  }

  void moveFormation() {
    int left = kPanelWidth;
    int right = 0;
    int bottom = 0;
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int x = origin_x_ + units_[i].ox;
      const int y = origin_y_ + units_[i].oy;
      if (x < left) left = x;
      if (x + unitW(units_[i]) > right) right = x + unitW(units_[i]);
      if (y + unitH(units_[i]) > bottom) bottom = y + unitH(units_[i]);
    }
    if (right <= 0) return;
    const int old_x = origin_x_;
    const int old_y = origin_y_;
    // Game ticks are 16 ms. step_px_ is the distance that used to land every
    // 32 ms, so each tick moves about half of it and the picture updates twice
    // as often without doubling the march.
    const int step = step_px_ > 1 ? (step_px_ + 1) / 2 : step_px_;
    if (form_dir_ > 0 && right + step >= kPanelWidth - 4) {
      form_dir_ = -1;
      origin_y_ += drop_px_;
    } else if (form_dir_ < 0 && left - step <= 4) {
      form_dir_ = 1;
      origin_y_ += drop_px_;
    } else {
      origin_x_ += form_dir_ * step;
    }
    const int dx = origin_x_ - old_x;
    const int dy = origin_y_ - old_y;
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int prev_ox = units_[i].ox;
      const int prev_oy = units_[i].oy;
      if (units_[i].stun > 0) {
        units_[i].ox = static_cast<int16_t>(prev_ox - dx);
        units_[i].oy = static_cast<int16_t>(prev_oy - dy);
      }
      const int w = unitW(units_[i]);
      const int h = unitH(units_[i]);
      const int nx = origin_x_ + units_[i].ox;
      const int ny = origin_y_ + units_[i].oy;
      drawAlienAt(nx, ny, units_[i].kind);
      uncover(old_x + prev_ox, old_y + prev_oy, nx, ny, w, h);
    }
    (void)bottom;
  }

  void stepBeam(bool firing, int next_beam) {
    if (beam_cool_ > 0) {
      beam_cool_ -= 1;
      if (beam_on_) {
        eraseBeam();
        beam_on_ = false;
      }
      beam_burst_ = 0;
      return;
    }
    if (beam_burst_ > 0 && !firing) {
      if (beam_on_) eraseBeam();
      beam_on_ = false;
      beam_burst_ = 0;
      beam_cool_ = beamCoolLen();
      return;
    }
    if (beam_burst_ > 0) {
      if (beam_on_ && next_beam != beam_x_) eraseBeam();
      beam_x_ = next_beam;
      beam_on_ = true;
      fillRect(beam_x_, 36, 3, kShipY - 36, laserColor());
      beam_burst_ -= 1;
      beam_tick_ += 1;
      if ((beam_tick_ & 3) == 0) {
        hurtAlongBeam();
        if (beam_on_) fillRect(beam_x_, 36, 3, kShipY - 36, laserColor());
      }
      if (beam_burst_ == 0) {
        if (beam_on_) eraseBeam();
        beam_on_ = false;
        beam_cool_ = beamCoolLen();
      }
      return;
    }
    if (!firing) return;
    beam_burst_ = beamBurstLen();
    beam_tick_ = 0;
    beam_x_ = next_beam;
    beam_on_ = true;
    fillRect(beam_x_, 36, 3, kShipY - 36, laserColor());
  }

  uint16_t foeInk(uint8_t kind) const {
    if (kind == 3) return godInk();
    if (kind == 2) return color565(255, 168, 56);
    if (kind == 1) return color565(255, 140, 96);
    return color565(255, 72, 64);
  }

  void drawFoe(const FoeBolt& foe) {
    if (foe.kind == 3) {
      const uint16_t ink = godInk();
      fillRect(foe.x, foe.y, 10, 10, color565(16, 12, 20));
      fillRect(foe.x + 2, foe.y, 6, 10, ink);
      fillRect(foe.x, foe.y + 2, 10, 6, ink);
      fillRect(foe.x + 3, foe.y + 3, 4, 4, color565(255, 255, 255));
      return;
    }
    if (foe.kind == 4) {
      const uint16_t ink = god_ != 0 ? godInk() : color565(120, 220, 96);
      fillRect(foe.x, foe.y, foe.w, foe.h, ink);
      if (foe.w > 6 && foe.h > 6) fillRect(foe.x + 3, foe.y + 3, foe.w - 6, foe.h - 6, color565(255, 255, 255));
      return;
    }
    if (foe.kind == 5) {
      fillRect(foe.x, foe.y, foe.w, foe.h, color565(255, 140, 200));
      return;
    }
    fillRect(foe.x, foe.y, foe.w, foe.h, foeInk(foe.kind));
  }

  bool spawnFoe(int x, int y, int vx, uint8_t kind = 0, int vy_override = -1) {
    const int w = kind == 3 ? 10 : (kind == 4 ? 14 : (kind == 5 ? 4 : (kind == 2 ? 5 : (kind == 1 ? 2 : kShotW))));
    const int h = kind == 3 ? 10 : (kind == 4 ? 10 : (kind == 5 ? 8 : (kind == 2 ? 6 : (kind == 1 ? 6 : kShotH))));
    const int vy = vy_override >= 0 ? vy_override
                             : kind == 3 ? (4 + (god_entrance_ ? god_phase_ : 0))
                             : kind == 4 ? 2
                             : kind == 5 ? 3
                             : (kind == 2 ? 4 : (kind == 1 ? 8 : 6));
    if (x < 0) x = 0;
    if (x > kPanelWidth - w) x = kPanelWidth - w;
    for (int i = 0; i < kFoeBolts; ++i) {
      if (foes_[i].on) continue;
      foes_[i].on = true;
      foes_[i].x = static_cast<int16_t>(x);
      foes_[i].y = static_cast<int16_t>(y);
      foes_[i].vx = static_cast<int8_t>(vx);
      foes_[i].vy = static_cast<int8_t>(vy);
      foes_[i].kind = kind;
      foes_[i].w = static_cast<uint8_t>(w);
      foes_[i].h = static_cast<uint8_t>(h);
      drawFoe(foes_[i]);
      return true;
    }
    return false;
  }

  int countFoeKind(int kind) const {
    int n = 0;
    for (int i = 0; i < kFoeBolts; ++i) {
      if (foes_[i].on && foes_[i].kind == kind) n += 1;
    }
    return n;
  }

  // A god hazard is the same cross the player can see. It only hurts where it is drawn.
  void spawnGodOrb(int x, int y, int vx, int vy) {
    const int w = 10;
    const int h = 10;
    if (x < 4) x = 4;
    if (x > kPanelWidth - w - 4) x = kPanelWidth - w - 4;
    if (y < 32) y = 32;
    if (y > kShipY - 90) y = kShipY - 90;
    if (overlap(x, y, w, h, player_x_, kShipY, kShipW, kShipH)) return;
    spawnFoe(x, y, vx, 3, vy);
  }

  // Each Olympian throws a slow, visible orb. Only Ares homes onto the ship.
  void fireGodGimmick(int x, int y) {
    const int have = countFoeKind(3);
    if (have >= 2) return;
    const int px = player_x_ + kShipW / 2;
    switch (god_) {
      case 1:
        spawnGodOrb(x - 5, y, 0, 2);
        break;
      case 2:
        if (have > 0) break;
        spawnGodOrb(12, 44, 1, 2);
        spawnGodOrb(kPanelWidth - 22, 44, -1, 2);
        break;
      case 3: {
        if (have > 0) break;
        const int gap = 48 + static_cast<int>(nextRng() % (kPanelWidth - 140));
        for (int sx = 20; sx < kPanelWidth - 30; sx += 84) {
          if (sx > gap - 28 && sx < gap + 28) continue;
          spawnGodOrb(sx, 40, 0, 2);
        }
        break;
      }
      case 4:
        spawnGodOrb(20 + static_cast<int>(nextRng() % (kPanelWidth - 50)), 40, 0, 1);
        break;
      case 5:
        if (have > 0) break;
        spawnGodOrb(x - 40, y, 0, 2);
        spawnGodOrb(x + 30, y, 0, 2);
        break;
      case 6:
        spawnGodHazard(x - 5, y);
        break;
      case 7:
        spawnGodOrb(x - 5, y, (space_tick_ & 32) ? 1 : -1, 2);
        break;
      case 8: {
        const int dx = px - x;
        const int vx = dx < -24 ? -1 : (dx > 24 ? 1 : 0);
        spawnGodOrb(x - 5, y, vx, 2);
        break;
      }
      case 9: {
        const int dx = px - x;
        const int vx = dx < -8 ? -1 : (dx > 8 ? 1 : 0);
        spawnGodOrb(x - 5, y, vx, 2);
        break;
      }
      case 10:
        spawnGodOrb(16 + static_cast<int>(nextRng() % (kPanelWidth - 42)), 36, 0, 2);
        break;
      case 11:
        spawnGodOrb(x - 5, y, (nextRng() & 1) ? 1 : -1, 2);
        break;
      default: {
        const int vx = static_cast<int>(nextRng() % 3) - 1;
        spawnGodOrb(24 + static_cast<int>(nextRng() % (kPanelWidth - 58)), 40, vx, 1);
        break;
      }
    }
  }

  void restoreRect(int x, int y, int w, int h) {
    repair(x, y, w, h);
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      if (overlap(x, y, w, h, ax, ay, unitW(units_[i]), unitH(units_[i]))) {
        drawAlienAt(ax, ay, units_[i].kind);
      }
    }
    for (int i = 0; i < scrap_n_; ++i) {
      const Scrap& scrap = scraps_[i];
      const int sx = terrain_ == 2 ? 0 : scrap.x;
      const int sw = terrain_ == 2 ? kPanelWidth : scrap.w;
      const int sh = terrain_ == 2 ? 2 : scrap.h;
      if (!overlap(x, y, w, h, sx, scrap.y, sw, sh)) continue;
      if (terrain_ == 2) fillRect(x, scrap.y, w, 2, palette().planet);
      else drawScrap(scrap);
    }
    for (int i = 0; i < barrier_n_; ++i) {
      if (!barriers_[i].on) continue;
      if (overlap(x, y, w, h, barriers_[i].x, barriers_[i].y, kBarCols * 3, 21)) drawBarrier(i);
    }
    for (int i = 0; i < kFoeBolts; ++i) {
      if (!foes_[i].on) continue;
      if (overlap(x, y, w, h, foes_[i].x, foes_[i].y, foes_[i].w, foes_[i].h)) drawFoe(foes_[i]);
    }
    if (sky_on_) drawSky();
    if (drop_on_ && overlap(x, y, w, h, drop_x_, drop_y_, 14, 14)) drawDrop();
    drawShipAt(player_x_);
    if (clone_alive_ && clone_drawn_) drawShipAt(clone_x_);
    if (buddy_drawn_) drawBuddy();
  }

  void stepWave() {
    if (wave_y_ < 0) return;
    const int old = wave_y_;
    restoreRect(0, old, kPanelWidth, 14);
    wave_y_ -= 6;
    if (wave_y_ < 28) {
      wave_y_ = -1;
      return;
    }
    fillRect(0, wave_y_, kPanelWidth, 12, color565(48, 160, 255));
    fillRect(0, wave_y_ + 4, kPanelWidth, 4, color565(190, 230, 255));
  }

  void stepSun() {
    if (sun_ticks_ <= 0) return;
    const uint16_t ink = (sun_ticks_ & 2) ? color565(255, 250, 220) : color565(255, 196, 64);
    fillRect(sun_x_, 36, 6, kShipY - 36, ink);
    if ((sun_ticks_ % 8) == 1) {
      for (int i = unit_n_ - 1; i >= 0; --i) {
        if (!units_[i].alive) continue;
        const int ax = origin_x_ + units_[i].ox;
        const int ay = origin_y_ + units_[i].oy;
        if (!overlap(sun_x_, 36, 6, kShipY - 36, ax, ay, unitW(units_[i]), unitH(units_[i]))) continue;
        applyHit(i);
      }
    }
    sun_ticks_ -= 1;
    if (sun_ticks_ == 0) restoreRect(sun_x_, 36, 6, kShipY - 36);
  }

  bool stepGodBeam() {
    god_beam_ticks_ = 0;
    return false;
  }

  void eraseDrones() {
    if (!drone_drawn_) return;
    for (int i = 0; i < drone_n_; ++i) repair(drone_x_[i], kShipY - 14, 8, 8);
    drone_drawn_ = false;
    drone_n_ = 0;
  }

  void paintDrones() {
    eraseDrones();
    if (demo_) return;
    int n = 0;
    for (int i = 0; i < kGods; ++i) {
      if (!divineOn(i)) continue;
      int x = player_x_ - 6 + n * 10;
      if (x < 2) x = 2;
      if (x > kPanelWidth - 10) x = kPanelWidth - 10;
      drone_x_[n] = x;
      fillRect(x, kShipY - 14, 8, 6, godInkOf(i + 1));
      fillRect(x + 2, kShipY - 12, 4, 2, color565(255, 255, 255));
      n += 1;
    }
    drone_n_ = n;
    drone_drawn_ = n > 0;
  }

  void stepClone() {
    if (demo_ || !divineOn(1)) {
      if (clone_drawn_) eraseShipAt(clone_x_);
      clone_drawn_ = false;
      clone_alive_ = false;
      return;
    }
    if (!clone_alive_) return;
    int want = (kPanelWidth - kShipW) - player_x_;
    if (want < 4) want = 4;
    if (want > kPanelWidth - kShipW - 4) want = kPanelWidth - kShipW - 4;
    if (want > player_x_ - kShipW && want < player_x_ + kShipW) {
      want = player_x_ > (kPanelWidth / 2) ? 8 : kPanelWidth - kShipW - 8;
    }
    if (clone_drawn_ && clone_x_ != want) eraseShipAt(clone_x_);
    clone_x_ = want;
    clone_drawn_ = true;
    drawShipAt(clone_x_);
    if (clone_cool_ > 0) clone_cool_ -= 1;
    else if (spawnBoltAt(clone_x_ + kShipW / 2, kShipY - 10, 0, -8, 3, 8, 0)) {
      clone_cool_ = fireGap();
    }
  }

  void stepAegisBox() {
    if (aegis_box_) {
      repair(aegis_box_x_, kShipY - 3, kShipW + 6, kShipH + 6);
      aegis_box_ = false;
    }
    if (aegis_hits_ <= 0) return;
    aegis_box_x_ = player_x_ - 3;
    drawRect(aegis_box_x_, kShipY - 3, kShipW + 6, kShipH + 6, color565(220, 230, 245));
    aegis_box_ = true;
  }

  bool castLightning() {
    int best = -1;
    int best_y = -999;
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ay = origin_y_ + units_[i].oy;
      if (ay > best_y) {
        best_y = ay;
        best = i;
      }
    }
    if (best < 0) return false;
    const int zx = origin_x_ + units_[best].ox + unitW(units_[best]) / 2 - 2;
    const int zw = 4;
    const int zh = kShipY - 30;
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      if (!overlap(zx, 30, zw, zh, ax, ay, unitW(units_[i]), unitH(units_[i]))) continue;
      units_[i].stun = 94;
    }
    applyHit(best);
    fillRect(zx, 30, zw, zh, color565(255, 240, 120));
    delay(20);
    restoreRect(zx, 30, zw, zh);
    return true;
  }

  bool castTsunami() {
    if (wave_y_ >= 0) return false;
    wave_y_ = kShipY - 20;
    return true;
  }

  bool castHeal() {
    if (demeter_uses_ >= 2 || lives_ >= 9) return false;
    demeter_uses_ += 1;
    lives_ += demeter_uses_ == 1 ? 3 : 1;
    if (lives_ > 9) lives_ = 9;
    noteUnlock(demeter_uses_ == 1 ? "+3 LIFE" : "+1 LIFE");
    return true;
  }

  bool castAegis() {
    if (athena_uses_ >= 2 || aegis_hits_ > 0) return false;
    athena_uses_ += 1;
    aegis_ticks_ = 188;
    aegis_hits_ = 3;
    noteUnlock("AEGIS");
    return true;
  }

  bool castSolaris() {
    sun_x_ = player_x_ + kShipW / 2 - 2;
    if (sun_x_ < 4) sun_x_ = 4;
    if (sun_x_ > kPanelWidth - 10) sun_x_ = kPanelWidth - 10;
    sun_ticks_ = 36;
    return true;
  }

  bool castArrow() {
    bool any = false;
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].alive) any = true;
    }
    if (!any) return false;
    return spawnBoltAt(player_x_ + kShipW / 2, kShipY - 12, 0, -8, 3, 10, 4);
  }

  bool castCharm() {
    charm_ticks_ = 188;
    return true;
  }

  bool castAnvil() {
    int best_n = 0;
    int bx = 0;
    int by = 0;
    bool found = false;
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox + unitW(units_[i]) / 2;
      const int ay = origin_y_ + units_[i].oy + unitH(units_[i]) / 2;
      int n = 0;
      for (int j = 0; j < unit_n_; ++j) {
        if (!units_[j].alive) continue;
        const int jx = origin_x_ + units_[j].ox + unitW(units_[j]) / 2;
        const int jy = origin_y_ + units_[j].oy + unitH(units_[j]) / 2;
        const int dx = ax - jx;
        const int dy = ay - jy;
        if (dx * dx + dy * dy <= 48 * 48) n += 1;
      }
      if (!found || n > best_n) {
        found = true;
        best_n = n;
        bx = ax;
        by = ay;
      }
    }
    if (!found) return false;
    for (int r = 6; r <= kBlastR; r += 7) drawCircle(bx, by, r, color565(255, 140, 40));
    for (int i = unit_n_ - 1; i >= 0; --i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      if (!overlap(bx - kBlastR, by - kBlastR, kBlastR * 2, kBlastR * 2, ax, ay, unitW(units_[i]),
                   unitH(units_[i]))) {
        continue;
      }
      applyHit(i);
    }
    delay(30);
    restoreRect(bx - kBlastR - 1, by - kBlastR - 1, kBlastR * 2 + 2, kBlastR * 2 + 2);
    return true;
  }

  bool castHermes() {
    hermes_ticks_ = 94;
    if (invuln_ < 94) invuln_ = 94;
    return true;
  }

  bool castRevel() {
    revel_ticks_ = 188;
    return true;
  }

  bool triggerMachine(int i) {
    switch (i) {
      case 0: return castLightning();
      case 2: return castTsunami();
      case 3: return castHeal();
      case 4: return castAegis();
      case 6: return castSolaris();
      case 7: return castArrow();
      case 8: return castCharm();
      case 9: return castAnvil();
      case 10: return castHermes();
      case 11: return castRevel();
      default: return false;
    }
  }

  void stepMachines() {
    if (demo_ || mode_ != Mode::Play) return;
    if (stun_ticks_ > 0) stun_ticks_ -= 1;
    if (charm_ticks_ > 0) charm_ticks_ -= 1;
    if (revel_ticks_ > 0) revel_ticks_ -= 1;
    if (hermes_ticks_ > 0) hermes_ticks_ -= 1;
    if (berserk_ticks_ > 0) berserk_ticks_ -= 1;
    if (aegis_ticks_ > 0) {
      aegis_ticks_ -= 1;
      if (aegis_ticks_ == 0) aegis_hits_ = 0;
    }
    stepAegisBox();
    stepWave();
    stepSun();
    stepClone();
    paintDrones();
    for (int i = 0; i < kGods; ++i) {
      if (!divineOn(i)) continue;
      if (div_cool_[i] > 0) {
        div_cool_[i] -= 1;
        continue;
      }
      if (triggerMachine(i)) div_cool_[i] = machineCool(i);
    }
  }

  void spawnGodHazard(int x, int y) {
    const int dx = (player_x_ + kShipW / 2) - (x + 5);
    const int vx = dx < -6 ? -1 : (dx > 6 ? 1 : 0);
    spawnGodOrb(x, y, vx, 2);
  }

  // A cannon shell bursts on contact. On this panel 3 mm is about 20 px,
  // which is the kBlastR constant.
  bool blastAt(int cx, int cy) {
    const uint16_t ink = color565(255, 176, 60);
    for (int r = 5; r <= kBlastR; r += 5) {
      drawCircle(cx, cy, r, ink);
    }
    for (int i = 0; i < barrier_n_; ++i) {
      if (!barriers_[i].on) continue;
      for (int col = 0; col < kBarCols; ++col) {
        const int bx = barriers_[i].x + col * 3;
        if (bx < cx - kBlastR || bx > cx + kBlastR) continue;
        barriers_[i].col[col] = 0;
      }
      repair(barriers_[i].x, barriers_[i].y, kBarCols * 3, 21);
      drawBarrier(i);
    }
    const bool hit = cx > player_x_ - kBlastR && cx < player_x_ + kShipW + kBlastR &&
                     cy > kShipY - kBlastR && cy < kShipY + kShipH + kBlastR;
    delay(45);
    repair(cx - kBlastR - 1, cy - kBlastR - 1, kBlastR * 2 + 2, kBlastR * 2 + 2);
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      if (overlap(cx - kBlastR, cy - kBlastR, kBlastR * 2, kBlastR * 2, ax, ay, unitW(units_[i]),
                  unitH(units_[i]))) {
        drawAlienAt(ax, ay, units_[i].kind);
      }
    }
    for (int i = 0; i < barrier_n_; ++i) {
      if (!barriers_[i].on) continue;
      if (overlap(cx - kBlastR, cy - kBlastR, kBlastR * 2, kBlastR * 2, barriers_[i].x,
                  barriers_[i].y, kBarCols * 3, 21)) {
        drawBarrier(i);
      }
    }
    drawShipAt(player_x_);
    if (clone_alive_ && clone_drawn_) drawShipAt(clone_x_);
    if (clone_alive_ && cx > clone_x_ - kBlastR && cx < clone_x_ + kShipW + kBlastR &&
        cy > kShipY - kBlastR && cy < kShipY + kShipH + kBlastR) {
      clone_alive_ = false;
      if (clone_drawn_) eraseShipAt(clone_x_);
      clone_drawn_ = false;
    }
    drawHud();
    return hit;
  }

  void fireGodVolley() {
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive || units_[i].stun > 0) continue;
      if (units_[i].kind == 7) {
        if (countFoeKind(3) < 2) {
          spawnGodOrb(origin_x_ + units_[i].ox + kAllyW / 2 - 5,
                      origin_y_ + units_[i].oy + kAllyH, 0, 2);
        }
        continue;
      }
      if (units_[i].kind != 2) continue;
      const int x = origin_x_ + units_[i].ox + kBossW / 2;
      const int y = origin_y_ + units_[i].oy + kBossH;
      fireGodGimmick(x, y);
    }
  }

  bool stepFoe() {
    bool lost = false;
    for (int i = 0; i < kFoeBolts; ++i) {
      FoeBolt& foe = foes_[i];
      if (!foe.on) continue;
      const int ox = foe.x;
      const int oy = foe.y;
      const int w0 = foe.w;
      const int h0 = foe.h;
      if (wave_y_ >= 0 && overlap(ox, oy, w0, h0, 0, wave_y_, kPanelWidth, 14)) {
        repair(ox, oy, w0, h0);
        foe.on = false;
        continue;
      }
      if (revel_ticks_ > 0 && (space_tick_ & 15) == 0 && foe.kind != 3) {
        int nv = foe.vx + static_cast<int>(nextRng() % 3) - 1;
        if (nv > 3) nv = 3;
        if (nv < -3) nv = -3;
        foe.vx = static_cast<int8_t>(nv);
      }
      // Homing orbs belong to Ares alone. Other gods never steer a shot onto the ship.
      if (foe.kind == 3 && god_ == 6 && (space_tick_ & 7) == 0 && oy < kShipY - 80) {
        const int target = player_x_ + kShipW / 2;
        const int center = ox + foe.w / 2;
        if (target < center - 6 && foe.vx > -2) foe.vx = static_cast<int8_t>(foe.vx - 1);
        else if (target > center + 6 && foe.vx < 2) foe.vx = static_cast<int8_t>(foe.vx + 1);
      }
      if (foe.kind == 3 && god_ == 9 && (space_tick_ & 7) == 0) {
        if (foe.vx > 0) foe.vx = static_cast<int8_t>(foe.vx - 1);
        else if (foe.vx < 0) foe.vx = static_cast<int8_t>(foe.vx + 1);
      }
      if (foe.kind == 3 && god_ == 12 && (space_tick_ & 15) == 0) {
        foe.vx = static_cast<int8_t>(static_cast<int>(nextRng() % 3) - 1);
      }
      if (foe.kind == 5 && (space_tick_ & 3) == 0) {
        if (foe.vx > 0) foe.vx = static_cast<int8_t>(foe.vx - 1);
        else if (foe.vx < 0) foe.vx = static_cast<int8_t>(foe.vx + 1);
      }
      if (foe.kind == 4 && god_ == 12 && (space_tick_ & 15) == 0) {
        foe.vx = static_cast<int8_t>(static_cast<int>(nextRng() % 3) - 1);
      }
      if (revel_ticks_ > 0 && (space_tick_ & 1) != 0) continue;
      const int nx = foe.x + foe.vx;
      const int ny = foe.y + foe.vy;
      const int w = foe.w;
      const int h = foe.h;
      const bool hit_ship = overlap(nx, ny, w, h, player_x_, kShipY, kShipW, kShipH);
      const bool hit_clone = clone_alive_ && overlap(nx, ny, w, h, clone_x_, kShipY, kShipW, kShipH);
      const bool blocked = nx < 0 || nx > kPanelWidth - w || ny >= kButtonY || hitBarrier(nx, ny, w, h);
      if (hit_clone && !hit_ship) {
        repair(ox, oy, w, h);
        foe.on = false;
        if (foe.kind == 2 && blastAt(nx, ny)) {
          lost = true;
          hit_by_ = foeKiller(foe);
        }
        clone_alive_ = false;
        if (clone_drawn_) eraseShipAt(clone_x_);
        clone_drawn_ = false;
        continue;
      }
      if (hit_ship || blocked) {
        repair(ox, oy, w, h);
        foe.on = false;
        if (foe.kind == 2) {
          if (blastAt(nx, ny)) {
            lost = true;
            hit_by_ = foeKiller(foe);
          }
        } else if (hit_ship) {
          lost = true;
          hit_by_ = foeKiller(foe);
        }
      } else {
        foe.x = static_cast<int16_t>(nx);
        foe.y = static_cast<int16_t>(ny);
        if (foe.kind >= 3) {
          repair(ox, oy, w, h);
          drawFoe(foe);
        } else {
          fillRect(nx, ny, w, h, foeInk(foe.kind));
          uncover(ox, oy, nx, ny, w, h);
        }
      }
    }
    if (mode_ != Mode::Play) return lost;
    foe_cool_ += 1;
    if (foe_cool_ < foe_gap_) return lost;
    foe_cool_ = 0;
    if (charm_ticks_ > 0) return lost;
    if (god_ != 0 && god_entrance_ && bossAlive()) {
      fireGodVolley();
      return lost;
    }
    if (god_ != 0 && (space_tick_ & 1) == 0) {
      fireGodGimmick(kPanelWidth / 2, 40);
    }
    if (boss_ && boss_style_ == 3 && bossAlive()) {
      for (int i = 0; i < unit_n_; ++i) {
        if (!units_[i].alive || units_[i].kind != 2 || units_[i].stun > 0) continue;
        const int x = origin_x_ + units_[i].ox + kBossW / 2;
        const int y = origin_y_ + units_[i].oy + kBossH;
        spawnFoe(x - 8, y, -2);
        spawnFoe(x, y, 0);
        spawnFoe(x + 8, y, 2);
      }
    }
    int alive = 0;
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].alive && units_[i].kind != 2 && units_[i].stun == 0) alive += 1;
    }
    if (alive == 0) {
      if (bossAlive() && boss_style_ != 3) {
        for (int i = 0; i < unit_n_; ++i) {
          if (!units_[i].alive || units_[i].kind != 2 || units_[i].stun > 0) continue;
          spawnFoe(origin_x_ + units_[i].ox + unitW(units_[i]) / 2,
                   origin_y_ + units_[i].oy + unitH(units_[i]), 0);
          break;
        }
      }
      return lost;
    }
    int pick = static_cast<int>(nextRng() % static_cast<uint32_t>(alive));
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive || units_[i].kind == 2 || units_[i].stun > 0) continue;
      if (pick > 0) {
        pick -= 1;
        continue;
      }
      const int mx = origin_x_ + units_[i].ox + unitW(units_[i]) / 2;
      const int my = origin_y_ + units_[i].oy + unitH(units_[i]);
      if (units_[i].kind == 4) {
        // Assault: a short burst of fast rounds.
        spawnFoe(mx - 5, my, -1, 1);
        spawnFoe(mx + 5, my, 1, 1);
      } else if (units_[i].kind == 5) {
        // Cannon: one shell that bursts on impact.
        spawnFoe(mx - 2, my, 0, 2);
      } else if (units_[i].kind == 6) {
        spawnFoe(mx, my, 0, 1);
        spawnFoe(mx - 12, my, -1, 0);
        spawnFoe(mx + 12, my, 1, 0);
      } else if (units_[i].kind == 7) {
        spawnFoe(mx, my, 0, 1);
      } else {
        spawnFoe(mx, my, 0);
      }
      break;
    }
    return lost;
  }

  void repairLaserColumn() {
    const int x = boss_aim_ - 2;
    const int y = 36;
    const int w = 8;
    const int h = kShipY - 36;
    repair(x, y, w, h);
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      if (overlap(x, y, w, h, ax, ay, unitW(units_[i]), unitH(units_[i]))) {
        drawAlienAt(ax, ay, units_[i].kind);
      }
    }
    for (int i = 0; i < scrap_n_; ++i) {
      const Scrap& scrap = scraps_[i];
      const int sx = terrain_ == 2 ? 0 : scrap.x;
      const int sw = terrain_ == 2 ? kPanelWidth : scrap.w;
      const int sh = terrain_ == 2 ? 2 : scrap.h;
      if (!overlap(x, y, w, h, sx, scrap.y, sw, sh)) continue;
      if (terrain_ == 2) fillRect(x, scrap.y, w, 2, palette().planet);
      else drawScrap(scrap);
    }
    for (int i = 0; i < barrier_n_; ++i) {
      if (!barriers_[i].on) continue;
      if (overlap(x, y, w, h, barriers_[i].x, barriers_[i].y, kBarCols * 3, 21)) drawBarrier(i);
    }
    if (sky_on_) drawSky();
    if (drop_on_) drawDrop();
    drawShipAt(player_x_);
    if (buddy_drawn_) drawBuddy();
  }

  bool stepBossLaser() {
    if (!boss_ || boss_style_ != 4 || !bossAlive() || mode_ != Mode::Play) {
      if (laser_live_) {
        repairLaserColumn();
        laser_live_ = false;
      }
      return false;
    }
    laser_tick_ += 1;
    const int phase = laser_tick_ % 100;
    if (phase == 1) {
      boss_aim_ = player_x_ + kShipW / 2 - 1;
      if (boss_aim_ < 6) boss_aim_ = 6;
      if (boss_aim_ > kPanelWidth - 8) boss_aim_ = kPanelWidth - 8;
    }
    if (phase >= 2 && phase <= 35) {
      fillRect(boss_aim_, 36, 2, kShipY - 36, color565(90, 36, 36));
      laser_live_ = true;
      return false;
    }
    if (phase >= 36 && phase < 48) {
      fillRect(boss_aim_ - 1, 36, 4, kShipY - 36, color565(255, 48, 40));
      laser_live_ = true;
      if (phase == 36) {
        return overlap(boss_aim_ - 1, kShipY, 4, kShipH, player_x_, kShipY, kShipW, kShipH);
      }
      return false;
    }
    if (phase == 48 && laser_live_) {
      repairLaserColumn();
      laser_live_ = false;
    }
    return false;
  }

  void stepGame(bool down, bool rose, int tx, int ty) {
    const char* held = nullptr;
    if (down && contains(leftBox(), tx, ty)) held = "LEFT";
    else if (down && contains(fireBox(), tx, ty)) held = "FIRE";
    else if (down && contains(rightBox(), tx, ty)) held = "RIGHT";
    if (held != held_) {
      if (!demo_) drawGameKeys(held);
      held_ = held;
    }
    const int old_player = player_x_;
    const int pace = ship_step_ + (hermes_ticks_ > 0 ? 4 : 0);
    if (held != nullptr && std::strcmp(held, "LEFT") == 0) {
      player_x_ -= pace;
    } else if (held != nullptr && std::strcmp(held, "RIGHT") == 0) {
      player_x_ += pace;
    }
    if (player_x_ < 4) player_x_ = 4;
    if (player_x_ > kPanelWidth - kShipW - 4) player_x_ = kPanelWidth - kShipW - 4;
    (void)rose;
    if (invuln_ > 0) invuln_ -= 1;
    stepBoosts();
    const bool firing = held != nullptr && std::strcmp(held, "FIRE") == 0;
    if (gun_cool_ > 0) gun_cool_ -= 1;
    if (hud_msg_ticks_ > 0) {
      hud_msg_ticks_ -= 1;
      if (hud_msg_ticks_ == 0) drawHud();
    }
    const int next_beam = player_x_ + kShipW / 2 - 1;
    startWrite();
    stepSpace();
    if (player_x_ != old_player) {
      eraseShipAt(old_player);
      drawShipAt(player_x_);
    }
    stepBuddy();
    stepMachines();
    if (baseGun() == 3) stepBeam(firing, next_beam);
    else {
      if (beam_on_) {
        eraseBeam();
        beam_on_ = false;
      }
      beam_burst_ = 0;
      if (firing) tryFire();
    }
    stepBolts();
    stepDrop();
    stepCoins();
    const bool was_open = boss_open_;
    if (boss_ && boss_style_ == 2 && bossAlive()) {
      boss_tick_ += 1;
      boss_open_ = (boss_tick_ % 80) >= 40;
    }
    if (mode_ == Mode::Play) moveFormation();
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].stun > 0) units_[i].stun = static_cast<uint8_t>(units_[i].stun - 1);
    }
    if (was_open != boss_open_) drawHud();
    bool lost = stepFoe();
    if (!lost && stepGodBeam()) {
      lost = true;
      hit_by_ = godName();
    }
    if (!lost && stepBossLaser()) {
      lost = true;
      hit_by_ = "LASER";
    }
    endWrite();
    if (mode_ != Mode::Play) return;
    if (lost) {
      playerHit();
      return;
    }
    if (!anyAlive()) {
      if (drop_on_) return;
      if (demo_) {
        level_ += 1;
        if (level_ > kLevels) level_ = 1;
        startLevel();
        return;
      }
      if (god_ != 0) {
        // The four goon waves each end with a mini boss. Then the god drops in.
        god_wave_ = static_cast<uint8_t>(god_wave_ + 1);
        if (god_wave_ < 4) {
          spawnGodWave();
          noteUnlock("WAVE");
        } else {
          beginGodSplash();
        }
        return;
      }
      if (mini_) {
        mini_ = false;
        beginAdvance();
        return;
      }
      if (boss_ && !raid_ && !isGodLevel(level_) && (nextRng() % 5) == 0) {
        beginRaid(true);
        return;
      }
      bool granted = false;
      if (raid_) {
        if (!buddy_ && (nextRng() % 100) < 35) {
          buddy_ = true;
          saveGear();
          granted = true;
        }
        raid_ = false;
      }
      if (level_ >= kLevels) {
        endRound(true);
      } else {
        if (granted) noteUnlock("DRONE");
        beginAdvance();
      }
      return;
    }
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      if (origin_y_ + units_[i].oy + unitH(units_[i]) >= kShipY) {
        hit_by_ = unitKiller(units_[i].kind);
        playerHit();
        return;
      }
    }
  }


  lgfx::Bus_SPI bus_;
  lgfx::Panel_ST7796 panel_;
  lgfx::Light_PWM light_;
  Preferences prefs_;
  bool ready_ = false;
  bool touch_down_ = false;
  bool shot_ = false;
  bool regions_logged_ = false;
  bool boss_ = false;
  bool demo_ = false;
  bool mini_ = false;
  bool raid_ = false;
  bool raid_won_ = false;
  bool god_entrance_ = false;
  Mode mode_ = Mode::Demo;
  uint8_t theme_ = 0;
  uint16_t bank_ = 0;
  uint8_t themes_owned_ = 0x0F;
  uint8_t ships_owned_ = 0x01;
  uint8_t ship_model_ = 0;
  uint16_t lasers_owned_ = 0x0001;
  uint8_t shield_ = 0;
  uint8_t spare_ = 0;
  uint8_t thrust_ = 0;
  int ship_step_ = kShipStep;
  int shop_tab_ = 0;
  int shop_page_ = 0;
  Coin coins_[kCoins] = {};
  const char* hit_by_ = "ALIEN";
  const char* shop_msg_ = "";
  int origin_x_ = 0;
  int origin_y_ = 0;
  int form_dir_ = 1;
  int player_x_ = 0;
  int score_ = 0;
  int level_ = 1;
  int camp_level_ = 1;
  int unit_n_ = 0;
  int step_px_ = 2;
  int drop_px_ = 10;
  int barrier_n_ = 0;
  int boss_hp_ = 1;
  int boss_max_ = 1;
  int lives_ = 3;
  int invuln_ = 0;
  int foe_cool_ = 0;
  int foe_gap_ = 40;
  int god_hp_ = 0;
  int god_hp_max_ = 1;
  Bolt bolts_[kBolts];
  Scrap scraps_[kScraps];
  int scrap_n_ = 0;
  int space_tick_ = 0;
  int gun_cool_ = 0;
  int beam_x_ = 0;
  int beam_tick_ = 0;
  int drop_x_ = 0;
  int drop_y_ = 0;
  int boss_tick_ = 0;
  int hud_msg_ticks_ = 0;
  uint16_t boost_ticks_[kBoosts] = {};
  uint16_t boost_armed_[kBoosts] = {};
  bool beam_on_ = false;
  bool drop_on_ = false;
  bool boss_open_ = false;
  bool pause_advance_ = false;
  bool custom_from_pause_ = false;
  uint8_t terrain_ = 0;
  uint8_t drop_which_ = 0;
  uint8_t gun_equip_ = 0;
  uint8_t gun_unlock_ = 0;
  uint8_t gun_lv_[kGunSlots] = {};
  ForgedGun forged_[kMaxForged] = {};
  int custom_page_ = 0;
  int forge_focus_ = -1;
  uint8_t forge_attr_a_ = 0;
  uint8_t forge_attr_b_ = 0;
  uint32_t forge_power_ = 0;
  uint32_t forge_up_ = 0;
  int forge_tab_ = 0;
  int forge_step_ = 0;
  int forge_page_ = 0;
  int forge_first_ = 0;
  int forge_second_ = 0;
  const char* forge_msg_ = "";
  bool revived_ = false;
  bool spread_side_ = false;
  uint8_t laser_color_ = 0;
  uint8_t boss_style_ = 0;
  uint8_t boss_shield_ = 0;
  uint8_t boss_shield_max_ = 0;
  uint8_t boss_layers_ = 0;
  uint8_t boss_layer_max_ = 0;
  uint8_t sky_kind_ = 0;
  uint8_t god_ = 0;
  uint8_t god_wave_ = 0;
  uint8_t god_bar_ = 3;
  uint8_t god_phase_ = 0;
  uint16_t div_unlock_ = 0;
  uint16_t div_equip_ = 0;
  uint8_t div_multi_ = 0;
  int div_page_ = 0;
  int div_cool_[kGods] = {};
  int demeter_uses_ = 0;
  int athena_uses_ = 0;
  int stun_ticks_ = 0;
  int charm_ticks_ = 0;
  int revel_ticks_ = 0;
  int hermes_ticks_ = 0;
  int berserk_ticks_ = 0;
  int aegis_ticks_ = 0;
  int aegis_hits_ = 0;
  int aegis_box_x_ = 0;
  bool aegis_box_ = false;
  int wave_y_ = -1;
  int sun_ticks_ = 0;
  int sun_x_ = 0;
  int god_beam_ticks_ = 0;
  int god_beam_x_ = 0;
  int god_ward_ = 0;
  bool clone_alive_ = false;
  bool clone_drawn_ = false;
  int clone_x_ = 0;
  int clone_cool_ = 0;
  bool drone_drawn_ = false;
  int drone_n_ = 0;
  int drone_x_[kGods] = {};
  char div_msg_[16] = {};
  uint16_t camp_ = 0;
  bool buddy_ = false;
  bool laser_live_ = false;
  bool buddy_drawn_ = false;
  bool sky_on_ = false;
  int laser_tick_ = 0;
  int boss_aim_ = 160;
  int beam_burst_ = 0;
  int beam_cool_ = 0;
  int buddy_x_ = 0;
  int buddy_cool_ = 0;
  int sky_x_ = 0;
  int sky_y_ = 0;
  int sky_r_ = 16;
  FoeBolt foes_[kFoeBolts] = {};
  char hud_msg_[20] = {};
  const char* held_ = nullptr;
  uint32_t rng_ = 1;
  uint32_t step_at_ = 0;
  uint32_t advance_at_ = 0;
  uint32_t splash_at_ = 0;
  Unit units_[kMaxUnits] = {};
  Barrier barriers_[kMaxBarriers] = {};
  Star stars_[kStars] = {};
};

PanelDisplay* panel_display = nullptr;

}  // namespace

bool board_init() {
  if (panel_display == nullptr) {
    panel_display = new PanelDisplay();
  }
  return panel_display->begin();
}

Display* make_display() { return panel_display; }
