#include "board.h"

#include "cat_faces.h"
#include "friend_faces.h"
#include "hardware_config.h"
#include "resistive_touch.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

namespace {

// Faces stay in sprites. Everything else is drawn from the theme, and a finger
// only counts when it lands inside the rectangle that was just drawn.
class PanelDisplay : public lgfx::LGFX_Device, public Display {
 public:
  PanelDisplay()
      : idle_sprite_(this),
        blink_sprite_(this),
        mouth_sprite_(this),
        friend_a_(this),
        friend_b_(this),
        friend_c_(this) {
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
    theme_ = prefs_.getUChar("theme", 0) & 3;
    loadGear();
    makeStars();
    sprite_ok_ = bake(idle_sprite_, kCatIdle) && bake(blink_sprite_, kCatBlink) &&
                 bake(mouth_sprite_, kCatMouth);
    friend_key_ = color565(255, 0, 255);
    friend_ok_ = bakeFriend(friend_a_, 0) && bakeFriend(friend_b_, 1) && bakeFriend(friend_c_, 2);
    hunger_ = 8;
    mood_level_ = 8;
    pal_next_ = millis() + 7000;
    const CatFace& face = cat_frame(kCatIdle);
    cat_w_ = face.width;
    cat_h_ = face.height;
    rng_ = esp_random();
    if (rng_ == 0) rng_ = 1;
    fillScreen(palette().bg);
    visible_ = false;
    Serial.printf("FRAME walk_ms=%lu game_ms=%lu friends=%d\n", static_cast<unsigned long>(kWalkMs),
                  static_cast<unsigned long>(kStepMs), friend_ok_ ? friend_face_count() : 0);
    Serial.flush();
    return true;
  }

  void show(const char* expression) override {
    (void)expression;
    if (!ready_) {
      visible_ = false;
      return;
    }
    beginWander();
    visible_ = true;
  }

  void hide() override {
    placed_ = false;
    shot_ = false;
    banner_ = false;
    setBrightness(255);
    visible_ = false;
    mode_ = Mode::Away;
    away_drawn_ = false;
    if (ready_) {
      drawAway(false);
      away_drawn_ = true;
    }
  }

  void setPresence(const char* presence) override {
    Presence next = Presence::Solo;
    if (presence != nullptr && std::strcmp(presence, "linked") == 0) next = Presence::Linked;
    else if (presence != nullptr && std::strcmp(presence, "leaving") == 0) next = Presence::Leaving;
    else if (presence != nullptr && std::strcmp(presence, "away") == 0) next = Presence::Away;
    if (next == presence_) return;
    presence_ = next;
    if (next == Presence::Away || !ready_ || !visible_ || mode_ != Mode::Wander) return;
    if (presence_ == Presence::Leaving) showLeaveBanner();
    else if (banner_) hideLeaveBanner();
  }

  bool isVisible() const override { return visible_; }

  void tick() override {
    if (!ready_) return;
    if (!visible_) {
      if (mode_ == Mode::Away) tickAway(millis());
      return;
    }
    const uint32_t now = millis();
    const uint32_t pace = mode_ == Mode::Wander ? kWalkMs : kStepMs;
    if (step_at_ != 0 && static_cast<int32_t>(now - step_at_) < static_cast<int32_t>(pace)) {
      return;
    }
    step_at_ = now;
    decayNeeds(now);
    int tx = 0;
    int ty = 0;
    TouchRaw raw;
    const bool down = touch_read(raw, tx, ty);
    const char* where = controlAt(tx, ty);
    const bool on = down && std::strcmp(where, "none") != 0;
    touch_log(raw, tx, ty, where, on);
    const bool rose = down && !touch_down_;
    touch_down_ = down;

    if ((mode_ == Mode::Tetris || mode_ == Mode::Snake) && rose && on &&
        std::strcmp(where, "BACK") == 0) {
      beginHome();
      return;
    }
    if ((mode_ == Mode::Play || mode_ == Mode::Advance) && rose && on &&
        std::strcmp(where, "BACK") == 0) {
      beginPause();
      return;
    }
    if (mode_ == Mode::Advance) {
      if (static_cast<int32_t>(now - advance_at_) >= 0) {
        level_ += 1;
        startLevel();
      }
      return;
    }
    if (mode_ == Mode::Wander) {
      // The cat is leaving for the laptop. Hold the walk so the banner stays put.
      if (presence_ == Presence::Leaving) {
        if (!banner_) showLeaveBanner();
        return;
      }
      if (banner_) hideLeaveBanner();
      // Any sample inside the drawn PLAY key opens home. Empty glass is not
      // that key, so it only draws the cat over.
      if (on && std::strcmp(where, "PLAY") == 0) {
        beginHome();
        return;
      }
      if (rose && !on) {
        notice(tx, ty, now);
      }
      stepWalk(now);
      return;
    }
    if (mode_ == Mode::Home) {
      if (!rose || !on) {
        return;
      }
      if (std::strcmp(where, "INVADERS") == 0) {
        beginGame();
      } else if (std::strcmp(where, "TETRIS") == 0) {
        beginTetris();
      } else if (std::strcmp(where, "SNAKE") == 0) {
        beginSnake();
      } else if (std::strcmp(where, "CALENDAR") == 0) {
        beginCalendar();
      } else if (std::strcmp(where, "CARE") == 0) {
        beginCare();
      } else if (std::strcmp(where, "THEME") == 0) {
        theme_ = static_cast<uint8_t>((theme_ + 1) & 3);
        prefs_.putUChar("theme", theme_);
        beginHome();
      } else if (std::strcmp(where, "CUSTOM") == 0) {
        beginCustom(false);
      } else if (std::strcmp(where, "WALK") == 0) {
        beginWander();
      }
      return;
    }
    if (mode_ == Mode::Pause) {
      if (!rose || !on) return;
      if (std::strcmp(where, "RESUME") == 0) resumePlay();
      else if (std::strcmp(where, "CUSTOM") == 0) beginCustom(true);
      else if (std::strcmp(where, "HOME") == 0) beginHome();
      return;
    }
    if (mode_ == Mode::Custom) {
      if (!rose || !on) return;
      if (std::strcmp(where, "COLOR") == 0) {
        laser_color_ = static_cast<uint8_t>((laser_color_ + 1) % 12);
        saveGear();
        paintCustom();
      } else if (std::strcmp(where, "BACK") == 0) {
        leaveCustom();
      } else {
        for (int gun = 0; gun < 9; ++gun) {
          if (std::strcmp(where, gunToken(gun)) == 0) equipGun(gun);
        }
      }
      return;
    }
    if (mode_ == Mode::Calendar || mode_ == Mode::Over) {
      if (rose && on && std::strcmp(where, "HOME") == 0) {
        beginHome();
      }
      return;
    }
    if (mode_ == Mode::Care) {
      decayNeeds(now);
      if (needs_dirty_) paintCareMeters();
      if (rose && on && std::strcmp(where, "FEED") == 0) {
        hunger_ = 8;
        if (mood_level_ < 6) mood_level_ = 6;
        paintCareMeters();
      } else if (rose && on && std::strcmp(where, "CARE") == 0) {
        mood_level_ = 8;
        paintCareMeters();
      } else if (rose && on && std::strcmp(where, "HOME") == 0) {
        beginHome();
      }
      return;
    }
    if (mode_ == Mode::Play) {
      stepGame(down, rose, tx, ty);
    } else if (mode_ == Mode::Tetris) {
      stepTetris(down, rose, tx, ty);
    } else if (mode_ == Mode::Snake) {
      stepSnake(down, rose, tx, ty);
    }
  }

 private:
  enum class Mode : uint8_t {
    Wander, Home, Calendar, Play, Advance, Over, Away, Tetris, Snake, Care, Pause, Custom
  };
  enum class Presence : uint8_t { Solo, Linked, Leaving, Away };
  // Cece (a neko-like cat) wanders, notices a pointer, eases toward it, then
  // reacts briefly. These are the same beats, driven by taps on this glass.
  enum class Mood : uint8_t { Roam, Notice, Approach, React, Pause };

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
    bool alive;
  };
  struct Barrier {
    int x;
    int y;
    uint8_t col[10];
    bool on;
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
    int8_t last = -1;
    bool on = false;
  };
  struct FoeBolt {
    int16_t x = 0;
    int16_t y = 0;
    int8_t vx = 0;
    bool on = false;
  };
  struct Scrap {
    int16_t x = 0;
    int16_t y = 0;
    uint8_t w = 8;
    uint8_t h = 6;
  };
  struct Pal {
    int16_t x = 0;
    int16_t y = 0;
    int16_t ox = 0;
    int16_t oy = 0;
    int8_t vx = 1;
    int8_t vy = 0;
    uint8_t which = 0;
    uint8_t phase = 0;
    uint8_t speed = 1;
    bool placed = false;
    bool slow = false;
    bool moved = false;
    int tick = 0;
    uint32_t turn_at = 0;
    uint32_t leave_at = 0;
  };

  static constexpr uint32_t kBlinkEveryMs = 3400;
  static constexpr uint32_t kBlinkAtMs = 1600;
  static constexpr uint32_t kBlinkHoldMs = 140;
  static constexpr uint32_t kMouthEveryMs = 7600;
  static constexpr uint32_t kMouthAtMs = 2200;
  static constexpr uint32_t kMouthHoldMs = 380;
  static constexpr uint32_t kWalkMs = 8;
  static constexpr uint32_t kStepMs = 16;
  static constexpr int kLevels = 300;
  static constexpr int kMaxUnits = 40;
  static constexpr int kBolts = 6;
  static constexpr int kFoeBolts = 5;
  static constexpr int kGunCount = 9;
  static constexpr int kGunMaxLv = 5;
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
  static constexpr int kShotW = 3;
  static constexpr int kShotH = 8;
  static constexpr int kShipStep = 5;
  static constexpr int kCols = 10;
  static constexpr int kRows = 16;
  static constexpr int kCell = 14;
  static constexpr int kBoardX = 90;
  static constexpr int kBoardY = 40;
  static constexpr int kSnakeCols = 12;
  static constexpr int kSnakeRows = 16;
  static constexpr int kSnakeCell = 16;
  static constexpr int kSnakeX = 64;
  static constexpr int kSnakeY = 40;
  static constexpr int kSnakeMax = 48;
  static constexpr int kFriend = 64;

  bool bake(lgfx::LGFX_Sprite& sprite, CatFrame which) {
    const CatFace& face = cat_frame(which);
    sprite.setColorDepth(16);
    if (sprite.createSprite(face.width, face.height) == nullptr) {
      return false;
    }
    sprite.fillScreen(TFT_BLACK);
    sprite.drawPng(face.png, face.length, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f);
    return true;
  }

  bool bakeFriend(lgfx::LGFX_Sprite& sprite, int index) {
    const FriendFace& face = friend_face(index);
    sprite.setColorDepth(16);
    if (sprite.createSprite(face.width, face.height) == nullptr) {
      return false;
    }
    sprite.fillScreen(friend_key_);
    sprite.drawPng(face.png, face.length, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f);
    return true;
  }

  void loadGear() {
    gun_unlock_ = prefs_.getUChar("guns", 0);
    gun_equip_ = prefs_.getUChar("gun", 0);
    laser_color_ = static_cast<uint8_t>(prefs_.getUChar("laser", 0) % 12);
    buddy_ = prefs_.getUChar("buddy", 0) != 0;
    if (prefs_.getBytes("glv", gun_lv_, sizeof gun_lv_) != sizeof gun_lv_) {
      for (int i = 0; i < kGunCount; ++i) gun_lv_[i] = 0;
    }
    for (int i = 0; i < kGunCount; ++i) {
      if (gun_lv_[i] > kGunMaxLv) gun_lv_[i] = kGunMaxLv;
    }
    if (gun_equip_ >= kGunCount) gun_equip_ = 0;
    if (gun_equip_ > 0 && (gun_unlock_ & (1u << (gun_equip_ - 1))) == 0) gun_equip_ = 0;
  }

  void saveGear() {
    prefs_.putUChar("guns", gun_unlock_);
    prefs_.putUChar("gun", gun_equip_);
    prefs_.putUChar("laser", laser_color_);
    prefs_.putUChar("buddy", buddy_ ? 1 : 0);
    prefs_.putBytes("glv", gun_lv_, sizeof gun_lv_);
  }

  const char* laserName() const {
    switch (laser_color_ % 12) {
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

  uint16_t laserColor() const {
    switch (laser_color_ % 12) {
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
        return "CANNON";
    }
  }

  int gunLv(int gun) const {
    if (gun < 0 || gun >= kGunCount) return 0;
    return gun_lv_[gun] > kGunMaxLv ? kGunMaxLv : gun_lv_[gun];
  }

  bool gunUnlocked(int gun) const {
    if (gun <= 0) return true;
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

  const char* themeName() const {
    switch (theme_ & 3) {
      case 1:
        return "LIGHT";
      case 2:
        return "BLUE";
      case 3:
        return "AMBER";
      default:
        return "DARK NEON";
    }
  }

  Palette palette() const {
    switch (theme_ & 3) {
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
  Box invadersBox() const { return {16, 40, 288, 46}; }
  Box tetrisBox() const { return {16, 92, 288, 46}; }
  Box snakeBox() const { return {16, 144, 288, 46}; }
  Box calendarBox() const { return {16, 196, 288, 46}; }
  Box themeBox() const { return {16, 248, 288, 44}; }
  Box careBox() const { return {16, 304, 288, 44}; }
  Box customBox() const { return {16, 358, 288, 44}; }
  Box colorBox() const { return {16, 46, 288, 36}; }
  Box gunBox(int slot) const { return {16, 88 + slot * 34, 288, 32}; }
  Box pauseResumeBox() const { return {32, 176, 256, 46}; }
  Box pauseCustomBox() const { return {32, 232, 256, 46}; }
  Box pauseHomeBox() const { return {32, 288, 256, 46}; }
  Box feedBox() const { return {16, 340, 136, 52}; }
  Box cleanBox() const { return {168, 340, 136, 52}; }
  Box walkBox() const { return {24, kButtonY, kPanelWidth - 48, 44}; }
  Box homeBox() const { return {24, kButtonY, kPanelWidth - 48, 44}; }
  Box leftBox() const { return {8, kButtonY, 96, 44}; }
  Box fireBox() const { return {112, kButtonY, 96, 44}; }
  Box rightBox() const { return {216, kButtonY, 96, 44}; }
  Box backBox() const { return {228, 2, 88, 28}; }
  Box quadBox(int slot) const { return {8 + slot * 78, kButtonY, 72, 44}; }

  bool contains(const Box& box, int x, int y) const {
    return x >= box.x && y >= box.y && x < box.x + box.w && y < box.y + box.h;
  }

  const char* controlAt(int x, int y) const {
    if (mode_ == Mode::Wander) {
      return contains(playBox(), x, y) ? "PLAY" : "none";
    }
    if (mode_ == Mode::Home) {
      if (contains(invadersBox(), x, y)) return "INVADERS";
      if (contains(tetrisBox(), x, y)) return "TETRIS";
      if (contains(snakeBox(), x, y)) return "SNAKE";
      if (contains(calendarBox(), x, y)) return "CALENDAR";
      if (contains(themeBox(), x, y)) return "THEME";
      if (contains(careBox(), x, y)) return "CARE";
      if (contains(customBox(), x, y)) return "CUSTOM";
      if (contains(walkBox(), x, y)) return "WALK";
      return "none";
    }
    if (mode_ == Mode::Play || mode_ == Mode::Tetris || mode_ == Mode::Snake ||
        mode_ == Mode::Advance) {
      if (contains(backBox(), x, y)) return "BACK";
    }
    if (mode_ == Mode::Play) {
      if (contains(leftBox(), x, y)) return "LEFT";
      if (contains(fireBox(), x, y)) return "FIRE";
      if (contains(rightBox(), x, y)) return "RIGHT";
      return "none";
    }
    if (mode_ == Mode::Tetris) {
      if (contains(quadBox(0), x, y)) return "LEFT";
      if (contains(quadBox(1), x, y)) return "ROT";
      if (contains(quadBox(2), x, y)) return "DROP";
      if (contains(quadBox(3), x, y)) return "RIGHT";
      return "none";
    }
    if (mode_ == Mode::Snake) {
      if (contains(quadBox(0), x, y)) return "LEFT";
      if (contains(quadBox(1), x, y)) return "UP";
      if (contains(quadBox(2), x, y)) return "DOWN";
      if (contains(quadBox(3), x, y)) return "RIGHT";
      return "none";
    }
    if (mode_ == Mode::Calendar || mode_ == Mode::Over) {
      return contains(homeBox(), x, y) ? "HOME" : "none";
    }
    if (mode_ == Mode::Care) {
      if (contains(feedBox(), x, y)) return "FEED";
      if (contains(cleanBox(), x, y)) return "CARE";
      if (contains(homeBox(), x, y)) return "HOME";
      return "none";
    }
    if (mode_ == Mode::Pause) {
      if (contains(pauseResumeBox(), x, y)) return "RESUME";
      if (contains(pauseCustomBox(), x, y)) return "CUSTOM";
      if (contains(pauseHomeBox(), x, y)) return "HOME";
      return "none";
    }
    if (mode_ == Mode::Custom) {
      if (contains(colorBox(), x, y)) return "COLOR";
      for (int gun = 0; gun < kGunCount; ++gun) {
        if (contains(gunBox(gun), x, y)) return gunToken(gun);
      }
      if (contains(homeBox(), x, y)) return "BACK";
      return "none";
    }
    return "none";
  }

  void logRegions() {
    if (regions_logged_) {
      return;
    }
    regions_logged_ = true;
    const Box boxes[] = {playBox(),   invadersBox(), tetrisBox(), snakeBox(), calendarBox(),
                         themeBox(), careBox(),     customBox(), feedBox(),  cleanBox(),
                         walkBox(),  homeBox(),     leftBox(),   fireBox(),  rightBox(),
                         backBox()};
    const char* names[] = {"PLAY", "INVADERS", "TETRIS", "SNAKE",   "CALENDAR", "THEME",
                           "CARE", "CUSTOM",   "FEED",   "PET",     "WALK",     "HOME",
                           "LEFT", "FIRE",     "RIGHT",  "BACK"};
    for (int i = 0; i < 16; ++i) {
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

  void repair(int x, int y, int w, int h) {
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
      if (stars_[i].x >= x && stars_[i].x < x + w && stars_[i].y >= y && stars_[i].y < y + h) {
        drawPixel(stars_[i].x, stars_[i].y, ink);
      }
    }
    if (terrain_ == 1 && y + h > 404 && y < kButtonY && mode_ == Mode::Play) {
      paintPlanet();
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

  CatFrame faceAt(uint32_t now) const {
    const uint32_t elapsed = now - shown_at_;
    const uint32_t blink_pos = elapsed % kBlinkEveryMs;
    const uint32_t mouth_pos = elapsed % kMouthEveryMs;
    if (blink_pos >= kBlinkAtMs && blink_pos < kBlinkAtMs + kBlinkHoldMs) {
      return kCatBlink;
    }
    if (mood_ == Mood::React) {
      return kCatMouth;
    }
    if (glance_face_ != 0 && static_cast<int32_t>(now - glance_face_until_) < 0) {
      return glance_face_ == 2 ? kCatMouth : kCatBlink;
    }
    if (mouth_pos >= kMouthAtMs && mouth_pos < kMouthAtMs + kMouthHoldMs) {
      return kCatMouth;
    }
    return kCatIdle;
  }

  lgfx::LGFX_Sprite& spriteFor(CatFrame frame) {
    if (frame == kCatBlink) {
      return blink_sprite_;
    }
    if (frame == kCatMouth) {
      return mouth_sprite_;
    }
    return idle_sprite_;
  }

  void paintCat(int x, int y, CatFrame frame) {
    if (sprite_ok_) {
      spriteFor(frame).pushSprite(x, y);
      return;
    }
    const CatFace& face = cat_frame(frame);
    drawPng(face.png, face.length, x, y);
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

  void showLeaveBanner() {
    banner_ = true;
    const Palette ink = palette();
    fillRect(0, 0, kPanelWidth, 28, ink.button);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("To the laptop", kPanelWidth / 2, 14);
  }

  void hideLeaveBanner() {
    if (!banner_) return;
    banner_ = false;
    repair(0, 0, kPanelWidth, 28);
    if (placed_ && cat_y_ < 28 + cat_h_) paintCat(cat_x_, cat_y_, frame_);
    for (int i = 0; i < 2; ++i) {
      const Pal& pal = pals_[i];
      if (pal.phase != 0 && pal.placed && pal.y < 28 + kFriend) paintFriendAt(pal);
    }
  }

  void drawAway(bool pulseOnly) {
    const Palette ink = palette();
    if (!pulseOnly) {
      fillScreen(ink.bg);
      setFont(&fonts::Font4);
      setTextDatum(lgfx::middle_center);
      setTextColor(ink.ink);
      drawString("JITTERS", kPanelWidth / 2, 150);
      setFont(&fonts::Font2);
      setTextColor(ink.muted);
      drawString("is on the laptop", kPanelWidth / 2, 196);
      drawString("Send the cat back from", kPanelWidth / 2, 230);
      drawString("the laptop window.", kPanelWidth / 2, 254);
    }
    fillRoundRect(128, 320, 64, 28, 8, away_phase_ ? ink.accent : ink.button);
  }

  void tickAway(uint32_t now) {
    if (!away_drawn_) {
      drawAway(false);
      away_drawn_ = true;
      away_at_ = now;
      return;
    }
    if (away_at_ != 0 && static_cast<int32_t>(now - away_at_) < 450) return;
    away_at_ = now;
    away_phase_ = !away_phase_;
    drawAway(true);
  }

  void beginWander() {
    fadeOut();
    mode_ = Mode::Wander;
    mood_ = Mood::Roam;
    banner_ = false;
    shot_ = false;
    placed_ = false;
    frame_ = kCatIdle;
    cat_x_ = 40;
    cat_y_ = 70;
    vx_ = 1;
    vy_ = 0;
    speed_ = 1;
    slow_ = false;
    glance_step_ = false;
    glance_face_ = 0;
    walk_tick_ = 0;
    for (int i = 0; i < 2; ++i) {
      pals_[i].phase = 0;
      pals_[i].placed = false;
      pals_[i].moved = false;
    }
    shown_at_ = millis();
    step_at_ = 0;
    pickHeading(shown_at_, 1, 1);
    const Palette ink = palette();
    fillScreen(ink.bg);
    paintStars();
    drawKey("PLAY", playBox(), false);
    logRegions();
    fadeIn();
  }

  void pickHeading(uint32_t now, int bias_x, int bias_y) {
    static const int kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1},
                                   {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    int hx = 1;
    int hy = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
      const int i = static_cast<int>(nextRng() % 8);
      hx = kDir[i][0];
      hy = kDir[i][1];
      if (bias_x > 0 && hx < 0) continue;
      if (bias_x < 0 && hx > 0) continue;
      if (bias_y > 0 && hy < 0) continue;
      if (bias_y < 0 && hy > 0) continue;
      if (hx == vx_ && hy == vy_ && attempt < 5) continue;
      break;
    }
    if (bias_x > 0 && hx < 0) hx = 1;
    if (bias_x < 0 && hx > 0) hx = -1;
    if (bias_y > 0 && hy < 0) hy = 1;
    if (bias_y < 0 && hy > 0) hy = -1;
    if (hx == 0 && hy == 0) hx = bias_x != 0 ? bias_x : 1;
    vx_ = hx;
    vy_ = hy;
    const uint32_t roll = nextRng() % 5;
    speed_ = roll == 0 ? 2 : 1;
    slow_ = roll >= 3;
    heading_until_ = now + 700 + (nextRng() % 1700);
  }

  void startGlance(uint32_t now) {
    mood_ = Mood::Pause;
    mood_until_ = now + 460 + (nextRng() % 480);
    glance_dx_ = (nextRng() & 1) != 0 ? 6 : -6;
    glance_dy_ = static_cast<int>(nextRng() % 3) - 1;
    glance_step_ = true;
    glance_face_ = (nextRng() % 4) == 0 ? 2 : 1;
    glance_face_until_ = now + (glance_face_ == 2 ? 220 : 150);
    heading_until_ = mood_until_ + 400 + (nextRng() % 900);
  }

  void notice(int x, int y, uint32_t now) {
    const int max_x = kPanelWidth - cat_w_;
    const int max_y = playBox().y - cat_h_ - 8;
    lure_x_ = x - cat_w_ / 2;
    lure_y_ = y - cat_h_ / 2;
    if (lure_x_ < 0) lure_x_ = 0;
    if (lure_y_ < 8) lure_y_ = 8;
    if (lure_x_ > max_x) lure_x_ = max_x;
    if (lure_y_ > max_y) lure_y_ = max_y;
    mood_ = Mood::Notice;
    mood_until_ = now + 160;
  }

  void stepWalk(uint32_t now) {
    walk_tick_ += 1;
    if (heading_until_ == 0) {
      pickHeading(now, 0, 0);
    }
    if (mood_ == Mood::Notice && static_cast<int32_t>(now - mood_until_) >= 0) {
      mood_ = Mood::Approach;
    } else if (mood_ == Mood::React && static_cast<int32_t>(now - mood_until_) >= 0) {
      mood_ = Mood::Roam;
      glance_face_ = 0;
      pickHeading(now, lure_x_ >= cat_x_ ? 1 : -1, 0);
    } else if (mood_ == Mood::Pause && static_cast<int32_t>(now - mood_until_) >= 0) {
      mood_ = Mood::Roam;
      glance_face_ = 0;
      pickHeading(now, 0, 0);
    } else if (mood_ == Mood::Roam && static_cast<int32_t>(now - heading_until_) >= 0) {
      if ((nextRng() % 3) == 0) {
        startGlance(now);
      } else {
        pickHeading(now, 0, 0);
      }
    }

    const int max_x = kPanelWidth - cat_w_;
    const int max_y = playBox().y - cat_h_ - 8;
    int dx = 0;
    int dy = 0;
    if (mood_ == Mood::Approach) {
      const int ax = lure_x_ - cat_x_;
      const int ay = lure_y_ - cat_y_;
      const int reach = (ax > 36 || ax < -36 || ay > 36 || ay < -36) ? 2 : 1;
      if (ax > 0) dx = ax > reach ? reach : ax;
      else if (ax < 0) dx = ax < -reach ? -reach : ax;
      if (ay > 0) dy = ay > reach ? reach : ay;
      else if (ay < 0) dy = ay < -reach ? -reach : ay;
      if (dx == 0 && dy == 0) {
        mood_ = Mood::React;
        mood_until_ = now + 240;
        glance_face_ = 0;
      }
    } else if (mood_ == Mood::Pause) {
      if (glance_step_) {
        dx = glance_dx_;
        dy = glance_dy_;
        glance_step_ = false;
      }
    } else if (mood_ == Mood::Roam && !(slow_ && (walk_tick_ & 1) != 0)) {
      dx = vx_ * speed_;
      dy = vy_ * speed_;
    }

    int nx = cat_x_ + dx;
    int ny = cat_y_ + dy;
    if (mood_ == Mood::Roam && (nx < 0 || ny < 8 || nx > max_x || ny > max_y)) {
      const int bias_x = cat_x_ < 16 ? 1 : (cat_x_ > max_x - 16 ? -1 : 0);
      const int bias_y = cat_y_ < 24 ? 1 : (cat_y_ > max_y - 16 ? -1 : 0);
      pickHeading(now, bias_x, bias_y);
      dx = vx_ * speed_;
      dy = vy_ * speed_;
      nx = cat_x_ + dx;
      ny = cat_y_ + dy;
    }
    if (nx < 0) nx = 0;
    if (ny < 8) ny = 8;
    if (nx > max_x) nx = max_x;
    if (ny > max_y) ny = max_y;
    const CatFrame next = faceAt(now);
    const bool cat_changed = !placed_ || nx != cat_x_ || ny != cat_y_ || next != frame_;
    const bool pals_changed = stepFriends(now);
    if (!cat_changed && !pals_changed) {
      return;
    }
    startWrite();
    if (cat_changed) {
      paintCat(nx, ny, next);
      if (placed_) uncover(cat_x_, cat_y_, nx, ny, cat_w_, cat_h_);
    }
    for (int i = 0; i < 2; ++i) {
      Pal& pal = pals_[i];
      if (pal.phase == 0) {
        if (pal.placed) {
          repair(pal.ox, pal.oy, kFriend, kFriend);
          pal.placed = false;
        }
        continue;
      }
      if (!pal.moved && pal.placed) continue;
      paintFriendAt(pal);
      if (pal.placed) uncover(pal.ox, pal.oy, pal.x, pal.y, kFriend, kFriend);
      pal.placed = true;
      pal.moved = false;
    }
    for (int i = 0; i < 2; ++i) {
      Pal& pal = pals_[i];
      if (pal.phase == 0 || !pal.placed) continue;
      bool clipped = false;
      if (cat_changed && placed_ &&
          (overlap(pal.x, pal.y, kFriend, kFriend, cat_x_, cat_y_, cat_w_, cat_h_) ||
           overlap(pal.x, pal.y, kFriend, kFriend, nx, ny, cat_w_, cat_h_))) {
        clipped = true;
      }
      const Pal& other = pals_[1 - i];
      if (other.phase != 0 && other.placed &&
          overlap(pal.x, pal.y, kFriend, kFriend, other.ox, other.oy, kFriend, kFriend)) {
        clipped = true;
      }
      if (clipped) paintFriendAt(pal);
    }
    bool cover_cat = false;
    for (int i = 0; i < 2; ++i) {
      const Pal& pal = pals_[i];
      const bool touch_now = pal.phase != 0 && pal.placed &&
                             overlap(pal.x, pal.y, kFriend, kFriend, nx, ny, cat_w_, cat_h_);
      const bool touch_old =
          overlap(pal.ox, pal.oy, kFriend, kFriend, nx, ny, cat_w_, cat_h_);
      if (touch_now || touch_old) cover_cat = true;
    }
    if (cover_cat) paintCat(nx, ny, next);
    endWrite();
    cat_x_ = nx;
    cat_y_ = ny;
    frame_ = next;
    placed_ = true;
  }

  void paintFriendAt(const Pal& pal) {
    if (!friend_ok_) return;
    lgfx::LGFX_Sprite* sprite = &friend_a_;
    if (pal.which == 1) sprite = &friend_b_;
    else if (pal.which == 2) sprite = &friend_c_;
    sprite->pushSprite(pal.x, pal.y, friend_key_);
  }

  void aimPal(Pal& pal, int bias_x, int bias_y) {
    static const int kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1},
                                   {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    int hx = 1;
    int hy = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
      const int i = static_cast<int>(nextRng() % 8);
      hx = kDir[i][0];
      hy = kDir[i][1];
      if (bias_x > 0 && hx < 0) continue;
      if (bias_x < 0 && hx > 0) continue;
      if (bias_y > 0 && hy < 0) continue;
      if (bias_y < 0 && hy > 0) continue;
      if (hx == pal.vx && hy == pal.vy && attempt < 5) continue;
      break;
    }
    if (bias_x > 0 && hx < 0) hx = 1;
    if (bias_x < 0 && hx > 0) hx = -1;
    if (bias_y > 0 && hy < 0) hy = 1;
    if (bias_y < 0 && hy > 0) hy = -1;
    if (hx == 0 && hy == 0) hx = bias_x != 0 ? bias_x : 1;
    pal.vx = static_cast<int8_t>(hx);
    pal.vy = static_cast<int8_t>(hy);
    const uint32_t roll = nextRng() % 5;
    pal.speed = roll == 0 ? 2 : 1;
    pal.slow = roll >= 3;
  }

  void armPal(Pal& pal, uint32_t now, int which, bool from_left) {
    const int max_y = playBox().y - kFriend - 8;
    int span = max_y - 24;
    if (span < 1) span = 1;
    pal.which = static_cast<uint8_t>(which);
    pal.phase = 1;
    pal.x = from_left ? static_cast<int16_t>(-kFriend) : static_cast<int16_t>(kPanelWidth);
    pal.y = static_cast<int16_t>(24 + (nextRng() % static_cast<uint32_t>(span)));
    pal.ox = pal.x;
    pal.oy = pal.y;
    pal.placed = false;
    pal.moved = true;
    pal.vx = from_left ? 1 : -1;
    pal.vy = 0;
    pal.speed = 1;
    pal.slow = false;
    pal.tick = 0;
    pal.turn_at = now + 500;
    pal.leave_at = now + 5200 + (nextRng() % 4800);
  }

  void stepPal(Pal& pal, uint32_t now) {
    if (pal.phase == 0) {
      pal.moved = false;
      return;
    }
    const int max_x = kPanelWidth - kFriend;
    const int max_y = playBox().y - kFriend - 8;
    pal.tick += 1;
    if (pal.phase == 1 && static_cast<int32_t>(now - pal.leave_at) >= 0) {
      pal.phase = 2;
      pal.vx = (pal.x + kFriend / 2) < (kPanelWidth / 2) ? -1 : 1;
      pal.vy = 0;
      pal.speed = 2;
      pal.slow = false;
    } else if (pal.phase == 1 && static_cast<int32_t>(now - pal.turn_at) >= 0) {
      if ((nextRng() % 4) == 0) {
        pal.vx = 0;
        pal.vy = 0;
        pal.turn_at = now + 320 + (nextRng() % 420);
      } else {
        const int bias_x = pal.x < 12 ? 1 : (pal.x > max_x - 12 ? -1 : 0);
        const int bias_y = pal.y < 20 ? 1 : (pal.y > max_y - 12 ? -1 : 0);
        aimPal(pal, bias_x, bias_y);
        pal.turn_at = now + 480 + (nextRng() % 1200);
      }
    }
    int dx = 0;
    int dy = 0;
    if (!(pal.slow && (pal.tick & 1) != 0)) {
      dx = pal.vx * static_cast<int>(pal.speed);
      dy = pal.vy * static_cast<int>(pal.speed);
    }
    int nx = pal.x + dx;
    int ny = pal.y + dy;
    if (pal.phase == 1) {
      if (nx < 0 || nx > max_x || ny < 8 || ny > max_y) {
        const int bias_x = pal.x < 16 ? 1 : (pal.x > max_x - 16 ? -1 : 0);
        const int bias_y = pal.y < 24 ? 1 : (pal.y > max_y - 16 ? -1 : 0);
        aimPal(pal, bias_x, bias_y);
        nx = pal.x + pal.vx * static_cast<int>(pal.speed);
        ny = pal.y + pal.vy * static_cast<int>(pal.speed);
      }
      if (nx < 0) nx = 0;
      if (ny < 8) ny = 8;
      if (nx > max_x) nx = max_x;
      if (ny > max_y) ny = max_y;
    }
    pal.ox = pal.x;
    pal.oy = pal.y;
    pal.x = static_cast<int16_t>(nx);
    pal.y = static_cast<int16_t>(ny);
    pal.moved = !pal.placed || pal.x != pal.ox || pal.y != pal.oy;
    if (pal.phase == 2 && (pal.x <= -kFriend || pal.x >= kPanelWidth)) {
      pal.phase = 0;
      pal.moved = pal.placed;
    }
  }

  bool stepFriends(uint32_t now) {
    if (!friend_ok_) return false;
    const bool was_out = pals_[0].phase != 0 || pals_[1].phase != 0;
    if (!was_out && (pal_next_ == 0 || static_cast<int32_t>(now - pal_next_) >= 0)) {
      const bool from_left = (nextRng() & 1) != 0;
      const int which = static_cast<int>(nextRng() % 3);
      armPal(pals_[0], now, which, from_left);
      if ((nextRng() % 3) == 0) {
        const int other = (which + 1 + static_cast<int>(nextRng() % 2)) % 3;
        armPal(pals_[1], now, other, !from_left);
      }
      pal_next_ = now + 18000 + (nextRng() % 8000);
    }
    bool any = false;
    for (int i = 0; i < 2; ++i) {
      stepPal(pals_[i], now);
      if (pals_[i].moved || (pals_[i].phase == 0 && pals_[i].placed)) any = true;
    }
    const bool still = pals_[0].phase != 0 || pals_[1].phase != 0;
    if (was_out && !still) pal_next_ = now + 14000 + (nextRng() % 16000);
    return any;
  }

  void decayNeeds(uint32_t now) {
    if (need_at_ == 0) {
      need_at_ = now + 12000;
      return;
    }
    if (static_cast<int32_t>(now - need_at_) < 0) return;
    need_at_ = now + 10000 + (nextRng() % 2000);
    if (hunger_ > 0) hunger_ = static_cast<uint8_t>(hunger_ - 1);
    if (hunger_ == 0 || (nextRng() & 1) == 0) {
      if (mood_level_ > 0) mood_level_ = static_cast<uint8_t>(mood_level_ - 1);
    }
    needs_dirty_ = true;
  }

  void paintCareMeters() {
    needs_dirty_ = false;
    const Palette ink = palette();
    fillRect(0, 56, kPanelWidth, 270, ink.bg);
    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    const char* look = ":)";
    if (hunger_ == 0 || mood_level_ <= 2) look = ":(";
    else if (mood_level_ <= 5) look = ":|";
    drawString(look, kPanelWidth / 2, 110);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(ink.ink);
    drawString("HUNGER", 20, 176);
    drawString("MOOD", 20, 236);
    for (int i = 0; i < 8; ++i) {
      fillRect(20 + i * 36, 192, 30, 18, i < hunger_ ? ink.accent : ink.button);
      fillRect(20 + i * 36, 252, 30, 18, i < mood_level_ ? ink.top : ink.button);
    }
  }

  void beginCare() {
    fadeOut();
    mode_ = Mode::Care;
    needs_dirty_ = false;
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("CARE", kPanelWidth / 2, 28);
    paintCareMeters();
    drawKey("FEED", feedBox(), false);
    drawKey("CARE", cleanBox(), false);
    drawKey("HOME", homeBox(), false);
    fadeIn();
  }

  void beginHome() {
    fadeOut();
    mode_ = Mode::Home;
    shot_ = false;
    clearFoe();
    placed_ = false;
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("JITTERS", kPanelWidth / 2, 20);
    drawCard("INVADERS MACHINA", "300 levels", invadersBox());
    drawCard("TETRIS", "stack", tetrisBox());
    drawCard("SNAKE", "classic", snakeBox());
    drawCard("CALENDAR", "today", calendarBox());
    drawCard(themeName(), "tap to switch", themeBox());
    drawCard("CARE", "feed and mood", careBox());
    drawCard("CUSTOM", "ship and guns", customBox());
    drawKey("WALK", walkBox(), false);
    fadeIn();
  }

  void beginCalendar() {
    fadeOut();
    mode_ = Mode::Calendar;
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("CALENDAR", kPanelWidth / 2, 86);
    drawString(__DATE__, kPanelWidth / 2, 140);
    setTextColor(ink.muted);
    drawString("calendar not connected", kPanelWidth / 2, 186);
    drawKey("HOME", homeBox(), false);
    fadeIn();
  }

  int unitW(const Unit& unit) const {
    if (unit.kind == 2) return kBossW;
    if (unit.kind == 3) return kDroneW;
    return kAlienW;
  }
  int unitH(const Unit& unit) const {
    if (unit.kind == 2) return kBossH;
    if (unit.kind == 3) return kDroneH;
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

  void drawShipAt(int x) {
    const uint16_t ink = palette().ship;
    const int y = kShipY;
    fillRect(x + kShipW / 2 - 2, y, 4, kShipH, ink);
    fillRect(x, y + 9, kShipW, 6, ink);
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
    fillRect(0, 0, backBox().x - 4, 24, ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(ink.ink);
    char line[40];
    if (hud_msg_ticks_ > 0) {
      snprintf(line, sizeof line, "%s", hud_msg_);
    } else if (boss_) {
      const char* tag = "CORE";
      if (raid_ && anyMinion()) tag = "GOONS";
      else if (boss_style_ == 1 && anyMinion()) tag = "DRONES";
      else if (boss_layers_ > 0) tag = "SHIELD";
      else if (boss_style_ == 2) tag = boss_open_ ? "OPEN" : "SHUT";
      else if (boss_style_ == 3) tag = "SPRAY";
      else if (boss_style_ == 4) tag = "LASER";
      else if (raid_) tag = "RAID";
      snprintf(line, sizeof line, "%s %d %d x%d", tag, level_, score_, lives_);
    } else {
      snprintf(line, sizeof line, "MACHINA %d %d x%d", level_, score_, lives_);
    }
    if (textWidth(line) > backBox().x - 16) {
      snprintf(line, sizeof line, "M %d %d x%d", level_, score_, lives_);
    }
    drawString(line, 8, 12);
    if (boss_) drawBossBar();
  }

  void drawBossBar() {
    const Palette ink = palette();
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

  void buildLevel() {
    unit_n_ = 0;
    boss_ = (level_ % 5) == 0;
    raid_ = false;
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
    if (terrain_ == 1) paintPlanet();
    paintScraps();
    if (sky_on_) drawSky();
    for (int i = 0; i < barrier_n_; ++i) {
      drawBarrier(i);
    }
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      drawAlienAt(origin_x_ + units_[i].ox, origin_y_ + units_[i].oy, units_[i].kind);
    }
    drawShipAt(player_x_);
    if (buddy_) {
      buddy_x_ = player_x_ - 14;
      if (buddy_x_ < 4) buddy_x_ = player_x_ + kShipW + 4;
      buddy_drawn_ = true;
      drawBuddy();
    }
    if (drop_on_) drawDrop();
    drawHud();
    drawBack(false);
    drawGameKeys(nullptr);
  }

  void startLevel() {
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
    clearBolts();
    player_x_ = (kPanelWidth - kShipW) / 2;
    buildLevel();
    paintLevel();
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
      repair(foes_[i].x, foes_[i].y, kShotW, kShotH);
    }
    clearFoe();
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

  void showOver(const char* title) {
    fadeOut();
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
    char line[24];
    snprintf(line, sizeof line, "LV %d   %d", level_, score_);
    setTextColor(ink.muted);
    drawString(line, kPanelWidth / 2, 220);
    drawKey("HOME", homeBox(), false);
    fadeIn();
  }

  void endRound(bool won) { showOver(won ? "CLEARED" : "CAUGHT"); }

  void playerHit() {
    lives_ -= 1;
    clearFoe();
    shot_ = false;
    beam_on_ = false;
    beam_burst_ = 0;
    clearBolts();
    if (lives_ <= 0) {
      endRound(false);
      return;
    }
    if (raid_) {
      beginRaid(false);
      return;
    }
    startLevel();
  }

  void drawBack(bool held) { drawKey("BACK", backBox(), held); }

  void drawQuad(const char* a, const char* b, const char* c, const char* d, const char* held) {
    const char* names[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) {
      drawKey(names[i], quadBox(i), held != nullptr && std::strcmp(held, names[i]) == 0);
    }
  }

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
    if (y < 28 || y >= kShipY) return true;
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
      if (ny >= kShipY - 2) {
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
    foe_shot_ = false;
  }

  int nextLockedGun() const {
    for (int gun = 1; gun < kGunCount; ++gun) {
      if (!gunUnlocked(gun)) return gun;
    }
    return 0;
  }

  void spawnDrop(int which) {
    drop_on_ = true;
    drop_which_ = static_cast<uint8_t>(which);
    drop_x_ = static_cast<int>(16 + (nextRng() % (kPanelWidth - 48)));
    drop_y_ = 40;
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
    const uint32_t roll = nextRng() % 100;
    if (roll < 5) spawnDrop(9);
    else if (roll < 20) spawnDrop(10);
  }

  void drawDrop() {
    uint16_t color = color565(255, 220, 80);
    if (drop_which_ == 9) color = color565(255, 64, 96);
    else if (drop_which_ == 10) color = color565(255, 210, 48);
    else if (drop_which_ >= 1 && drop_which_ <= 8) {
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
      if (lives_ < 8) lives_ += 1;
      noteUnlock("HEART");
    } else if (drop_which_ == 10) {
      const int gun = gun_equip_;
      if (gun >= 0 && gun < kGunCount && gun_lv_[gun] < kGunMaxLv) {
        gun_lv_[gun] = static_cast<uint8_t>(gun_lv_[gun] + 1);
        saveGear();
        noteUnlock("GUN +1");
      } else {
        noteUnlock("MAX");
      }
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

  int beamBurstLen() const { return 188 + gunLv(3) * 12; }

  int beamCoolLen() const {
    int cool = 125 - gunLv(3) * 10;
    if (cool < 50) cool = 50;
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
    const int lv = gunLv(gun_equip_);
    if (gun_equip_ == 1) return 16 - lv;
    if (gun_equip_ == 2) return 3 - lv > 2 ? 3 - lv : 2;
    if (gun_equip_ == 4) return 8 - lv > 2 ? 8 - lv : 2;
    if (gun_equip_ == 5) return 10 - lv > 3 ? 10 - lv : 3;
    if (gun_equip_ == 6) return 22 - lv * 2 > 8 ? 22 - lv * 2 : 8;
    if (gun_equip_ == 7) return 20 - lv * 2 > 8 ? 20 - lv * 2 : 8;
    if (gun_equip_ == 8) return 14 - lv > 6 ? 14 - lv : 6;
    return 7 - lv > 2 ? 7 - lv : 2;
  }

  void tryFire() {
    if (gun_equip_ == 3 || gun_cool_ > 0) return;
    const int lv = gunLv(gun_equip_);
    const int cx = player_x_ + kShipW / 2;
    if (gun_equip_ == 1) {
      if (boltSlots() < 3) return;
      const int kick = lv / 2;
      spawnBoltAt(cx, kShipY - 7, -2, -7 - kick, 3, 6, 0);
      spawnBoltAt(cx, kShipY - 7, 0, -8 - kick, 3, 6, 0);
      spawnBoltAt(cx, kShipY - 7, 2, -7 - kick, 3, 6, 0);
      if (lv >= 3) spawnBoltAt(cx, kShipY - 7, 0, -9, 3, 6, 0);
      gun_cool_ = fireGap();
      return;
    }
    if (gun_equip_ == 4) {
      spawnBoltAt(player_x_ + 4, kShipY - 10, 0, -9 - lv, 3, 8, 0);
      spawnBoltAt(player_x_ + kShipW - 4, kShipY - 10, 0, -9 - lv, 3, 8, 0);
      gun_cool_ = fireGap();
      return;
    }
    if (gun_equip_ == 5) {
      if (spawnBoltAt(cx, kShipY - 10, 2, -8 - lv / 2, 3, 8, 1)) gun_cool_ = fireGap();
      return;
    }
    if (gun_equip_ == 6) {
      if (spawnBoltAt(cx, kShipY - 16, 0, -16, 2, 16, 2)) gun_cool_ = fireGap();
      return;
    }
    if (gun_equip_ == 7) {
      if (boltSlots() < 5) return;
      const int vxs[5] = {-3, -1, 0, 1, 3};
      for (int i = 0; i < 5; ++i) {
        spawnBoltAt(cx, kShipY - 8, vxs[i], -8 - lv / 2, 3, 6, 0);
      }
      gun_cool_ = fireGap();
      return;
    }
    if (gun_equip_ == 8) {
      if (spawnBoltAt(cx, kShipY - 10, 0, -7, 3, 8, 3)) gun_cool_ = fireGap();
      return;
    }
    const int vy = gun_equip_ == 2 ? -12 - lv : -9 - lv;
    const int h = gun_equip_ == 2 ? 10 : 8;
    if (spawnBoltAt(cx, kShipY - h - 1, 0, vy, 3, h, 0)) gun_cool_ = fireGap();
  }

  void steerBolt(Bolt& bolt) {
    if (bolt.tag == 1) {
      if ((space_tick_ & 3) == 0) bolt.vx = static_cast<int8_t>(-bolt.vx);
      if (bolt.vx == 0) bolt.vx = 2;
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

  void pierceUnit(Bolt& bolt, int sx, int sy) {
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
      return;
    }
  }

  bool applyHit(int index) {
    Unit& unit = units_[index];
    if (unit.kind == 2) {
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
      boss_hp_ -= 1;
      addScore(10);
      if (boss_hp_ <= 0) {
        unit.alive = false;
        maybeDrop(true);
        return true;
      }
      drawAlienAt(origin_x_ + unit.ox, origin_y_ + unit.oy, unit.kind);
      return false;
    }
    unit.alive = false;
    addScore(10);
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

  void paintCustom() {
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("CUSTOM", kPanelWidth / 2, 22);
    char color_line[24];
    snprintf(color_line, sizeof color_line, "LASER %s", laserName());
    drawCard(color_line, "tap to change", colorBox());
    for (int gun = 0; gun < kGunCount; ++gun) {
      char line[28];
      gunLine(gun, line, sizeof line);
      drawCard(line, nullptr, gunBox(gun));
    }
    drawKey("BACK", homeBox(), false);
  }

  void beginCustom(bool from_pause) {
    custom_from_pause_ = from_pause;
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
    else beginHome();
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
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int w = unitW(units_[i]);
      const int h = unitH(units_[i]);
      const int nx = origin_x_ + units_[i].ox;
      const int ny = origin_y_ + units_[i].oy;
      drawAlienAt(nx, ny, units_[i].kind);
      uncover(old_x + units_[i].ox, old_y + units_[i].oy, nx, ny, w, h);
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

  bool spawnFoe(int x, int y, int vx) {
    if (x < 0) x = 0;
    if (x > kPanelWidth - kShotW) x = kPanelWidth - kShotW;
    for (int i = 0; i < kFoeBolts; ++i) {
      if (foes_[i].on) continue;
      foes_[i].on = true;
      foes_[i].x = static_cast<int16_t>(x);
      foes_[i].y = static_cast<int16_t>(y);
      foes_[i].vx = static_cast<int8_t>(vx);
      fillRect(x, y, kShotW, kShotH, color565(255, 72, 64));
      return true;
    }
    return false;
  }

  bool stepFoe() {
    bool lost = false;
    for (int i = 0; i < kFoeBolts; ++i) {
      FoeBolt& foe = foes_[i];
      if (!foe.on) continue;
      const int ox = foe.x;
      const int oy = foe.y;
      const int nx = foe.x + foe.vx;
      const int ny = foe.y + 6;
      const bool hit_ship = overlap(nx, ny, kShotW, kShotH, player_x_, kShipY, kShipW, kShipH);
      const bool blocked =
          nx < 0 || nx > kPanelWidth - kShotW || ny >= kButtonY || hitBarrier(nx, ny, kShotW, kShotH);
      if (hit_ship || blocked) {
        repair(ox, oy, kShotW, kShotH);
        foe.on = false;
        if (hit_ship) lost = true;
      } else {
        foe.x = static_cast<int16_t>(nx);
        foe.y = static_cast<int16_t>(ny);
        fillRect(nx, ny, kShotW, kShotH, color565(255, 72, 64));
        uncover(ox, oy, nx, ny, kShotW, kShotH);
      }
    }
    if (mode_ != Mode::Play) return lost;
    foe_cool_ += 1;
    if (foe_cool_ < foe_gap_) return lost;
    foe_cool_ = 0;
    if (boss_ && boss_style_ == 3 && bossAlive()) {
      for (int i = 0; i < unit_n_; ++i) {
        if (!units_[i].alive || units_[i].kind != 2) continue;
        const int x = origin_x_ + units_[i].ox + kBossW / 2;
        const int y = origin_y_ + units_[i].oy + kBossH;
        spawnFoe(x - 8, y, -2);
        spawnFoe(x, y, 0);
        spawnFoe(x + 8, y, 2);
      }
    }
    int alive = 0;
    for (int i = 0; i < unit_n_; ++i) {
      if (units_[i].alive && units_[i].kind != 2) alive += 1;
    }
    if (alive == 0) {
      if (bossAlive() && boss_style_ != 3) {
        for (int i = 0; i < unit_n_; ++i) {
          if (!units_[i].alive || units_[i].kind != 2) continue;
          spawnFoe(origin_x_ + units_[i].ox + unitW(units_[i]) / 2,
                   origin_y_ + units_[i].oy + unitH(units_[i]), 0);
          break;
        }
      }
      return lost;
    }
    int pick = static_cast<int>(nextRng() % static_cast<uint32_t>(alive));
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive || units_[i].kind == 2) continue;
      if (pick > 0) {
        pick -= 1;
        continue;
      }
      spawnFoe(origin_x_ + units_[i].ox + unitW(units_[i]) / 2,
               origin_y_ + units_[i].oy + unitH(units_[i]), 0);
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
      drawGameKeys(held);
      held_ = held;
    }
    const int old_player = player_x_;
    if (held != nullptr && std::strcmp(held, "LEFT") == 0) {
      player_x_ -= kShipStep;
    } else if (held != nullptr && std::strcmp(held, "RIGHT") == 0) {
      player_x_ += kShipStep;
    }
    if (player_x_ < 4) player_x_ = 4;
    if (player_x_ > kPanelWidth - kShipW - 4) player_x_ = kPanelWidth - kShipW - 4;
    (void)rose;
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
      drawShipAt(player_x_);
      uncover(old_player, kShipY, player_x_, kShipY, kShipW, kShipH);
    }
    stepBuddy();
    if (gun_equip_ == 3) stepBeam(firing, next_beam);
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
    const bool was_open = boss_open_;
    if (boss_ && boss_style_ == 2 && bossAlive()) {
      boss_tick_ += 1;
      boss_open_ = (boss_tick_ % 80) >= 40;
    }
    if (mode_ == Mode::Play) moveFormation();
    if (was_open != boss_open_) drawHud();
    const bool lost = stepFoe() || stepBossLaser();
    endWrite();
    if (mode_ != Mode::Play) return;
    if (lost) {
      playerHit();
      return;
    }
    if (!anyAlive()) {
      if (drop_on_) return;
      if (boss_ && !raid_ && (nextRng() % 5) == 0) {
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
        playerHit();
        return;
      }
    }
  }

  void pieceCells(int kind, int rot, int ox, int oy, int* xs, int* ys) const {
    // Local cells stay inside 0..3 so a spawn or turn cannot hide a block above the well.
    static const int8_t kRot[7][4][4][2] = {
        {{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
         {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
         {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
         {{1, 0}, {1, 1}, {1, 2}, {1, 3}}},
        {{{0, 0}, {0, 1}, {1, 1}, {2, 1}},
         {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
         {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
         {{1, 0}, {1, 1}, {0, 2}, {1, 2}}},
        {{{2, 0}, {0, 1}, {1, 1}, {2, 1}},
         {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
         {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
         {{0, 0}, {1, 0}, {1, 1}, {1, 2}}},
        {{{1, 0}, {2, 0}, {1, 1}, {2, 1}},
         {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
         {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
         {{1, 0}, {2, 0}, {1, 1}, {2, 1}}},
        {{{1, 0}, {2, 0}, {0, 1}, {1, 1}},
         {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
         {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
         {{0, 0}, {0, 1}, {1, 1}, {1, 2}}},
        {{{1, 0}, {0, 1}, {1, 1}, {2, 1}},
         {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
         {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
         {{1, 0}, {0, 1}, {1, 1}, {1, 2}}},
        {{{0, 0}, {1, 0}, {1, 1}, {2, 1}},
         {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
         {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
         {{1, 0}, {0, 1}, {1, 1}, {0, 2}}},
    };
    const int use = kind >= 0 && kind < 7 ? kind : 0;
    const int turn = rot & 3;
    for (int i = 0; i < 4; ++i) {
      xs[i] = ox + kRot[use][turn][i][0];
      ys[i] = oy + kRot[use][turn][i][1];
    }
  }

  bool pieceFits(int kind, int rot, int ox, int oy) const {
    int xs[4];
    int ys[4];
    pieceCells(kind, rot, ox, oy, xs, ys);
    for (int i = 0; i < 4; ++i) {
      if (xs[i] < 0 || xs[i] >= kCols || ys[i] < 0 || ys[i] >= kRows) return false;
      if (well_[ys[i] * kCols + xs[i]] != 0) return false;
    }
    return true;
  }

  uint16_t miniInk(int kind) const {
    const Palette ink = palette();
    switch (kind % 7) {
      case 0:
        return ink.accent;
      case 1:
        return ink.top;
      case 2:
        return ink.low;
      case 3:
        return ink.ship;
      case 4:
        return ink.shot;
      case 5:
        return ink.planet;
      default:
        return ink.barrier;
    }
  }

  void paintGridCell(int gx, int gy, int kind, int origin_x, int origin_y, int cell) {
    if (gx < 0 || gy < 0) return;
    fillRect(origin_x + gx * cell, origin_y + gy * cell, cell - 1, cell - 1,
             kind == 0 ? palette().bg : miniInk(kind));
  }

  void paintPiece(int kind) {
    int xs[4];
    int ys[4];
    pieceCells(piece_, rot_, piece_x_, piece_y_, xs, ys);
    for (int i = 0; i < 4; ++i) {
      const int stored = ys[i] >= 0 ? well_[ys[i] * kCols + xs[i]] : 0;
      paintGridCell(xs[i], ys[i], kind == 0 ? stored : kind, kBoardX, kBoardY, kCell);
    }
  }

  void paintWell() {
    for (int y = 0; y < kRows; ++y) {
      for (int x = 0; x < kCols; ++x) {
        paintGridCell(x, y, well_[y * kCols + x], kBoardX, kBoardY, kCell);
      }
    }
  }

  void spawnPiece() {
    piece_ = static_cast<int>(nextRng() % 7);
    rot_ = 0;
    piece_x_ = 3;
    piece_y_ = 0;
    fall_ = 0;
    if (!pieceFits(piece_, rot_, piece_x_, piece_y_)) {
      showOver("FILLED");
      return;
    }
    paintPiece(piece_ + 1);
  }

  void lockPiece() {
    int xs[4];
    int ys[4];
    pieceCells(piece_, rot_, piece_x_, piece_y_, xs, ys);
    for (int i = 0; i < 4; ++i) {
      if (ys[i] < 0 || ys[i] >= kRows || xs[i] < 0 || xs[i] >= kCols) continue;
      well_[ys[i] * kCols + xs[i]] = static_cast<uint8_t>(piece_ + 1);
    }
    int cleared = 0;
    uint8_t packed[kCols * kRows];
    memset(packed, 0, sizeof packed);
    int write = kRows - 1;
    for (int y = kRows - 1; y >= 0; --y) {
      bool full = true;
      for (int x = 0; x < kCols; ++x) {
        if (well_[y * kCols + x] == 0) {
          full = false;
          break;
        }
      }
      if (full) {
        cleared += 1;
        continue;
      }
      memcpy(&packed[write * kCols], &well_[y * kCols], kCols);
      if (write == 0) break;
      write -= 1;
    }
    memcpy(well_, packed, sizeof packed);
    if (cleared > 0) {
      score_ += cleared * 10;
      if (score_ > 99999) score_ = 99999;
    }
    if (mode_ == Mode::Tetris) {
      paintWell();
      drawScore();
      spawnPiece();
    }
  }

  void drawScore() {
    const Palette ink = palette();
    fillRect(4, 2, 200, 28, ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(ink.ink);
    char line[20];
    snprintf(line, sizeof line, "SCORE %d", score_);
    drawString(line, 8, 16);
  }

  void beginTetris() {
    fadeOut();
    mode_ = Mode::Tetris;
    score_ = 0;
    level_ = 1;
    held_ = nullptr;
    memset(well_, 0, sizeof well_);
    const Palette ink = palette();
    fillScreen(ink.bg);
    drawRect(kBoardX - 2, kBoardY - 2, kCols * kCell + 3, kRows * kCell + 3, ink.accent);
    paintWell();
    drawBack(false);
    drawQuad("LEFT", "ROT", "DROP", "RIGHT", nullptr);
    tetris_hold_ = 0;
    drawScore();
    spawnPiece();
    fadeIn();
  }

  bool turnPiece() {
    const int next = (rot_ + 1) & 3;
    static const int kKick[5] = {0, -1, 1, -2, 2};
    for (int i = 0; i < 5; ++i) {
      const int nx = piece_x_ + kKick[i];
      if (!pieceFits(piece_, next, nx, piece_y_)) continue;
      paintPiece(0);
      piece_x_ = nx;
      rot_ = next;
      paintPiece(piece_ + 1);
      return true;
    }
    return false;
  }

  bool nudgePiece(int dir) {
    if (!pieceFits(piece_, rot_, piece_x_ + dir, piece_y_)) return false;
    paintPiece(0);
    piece_x_ += dir;
    paintPiece(piece_ + 1);
    return true;
  }

  void stepTetris(bool down, bool rose, int tx, int ty) {
    if (mode_ != Mode::Tetris) return;
    const char* held = nullptr;
    if (down && contains(quadBox(0), tx, ty)) held = "LEFT";
    else if (down && contains(quadBox(1), tx, ty)) held = "ROT";
    else if (down && contains(quadBox(2), tx, ty)) held = "DROP";
    else if (down && contains(quadBox(3), tx, ty)) held = "RIGHT";
    if (held != held_) {
      drawQuad("LEFT", "ROT", "DROP", "RIGHT", held);
      held_ = held;
      tetris_hold_ = 0;
    }
    const bool sideways = held != nullptr &&
                          (std::strcmp(held, "LEFT") == 0 || std::strcmp(held, "RIGHT") == 0);
    if (sideways) {
      const int dir = std::strcmp(held, "LEFT") == 0 ? -1 : 1;
      if (rose || tetris_hold_ >= 4) {
        nudgePiece(dir);
        tetris_hold_ = 0;
      } else {
        tetris_hold_ += 1;
      }
    } else if (rose && held != nullptr && std::strcmp(held, "ROT") == 0) {
      turnPiece();
    } else if (rose && held != nullptr && std::strcmp(held, "DROP") == 0) {
      paintPiece(0);
      while (pieceFits(piece_, rot_, piece_x_, piece_y_ + 1)) piece_y_ += 1;
      lockPiece();
      return;
    } else {
      tetris_hold_ = 0;
    }
    fall_ += 1;
    if (fall_ < 28 || mode_ != Mode::Tetris) return;
    fall_ = 0;
    if (pieceFits(piece_, rot_, piece_x_, piece_y_ + 1)) {
      paintPiece(0);
      piece_y_ += 1;
      paintPiece(piece_ + 1);
      return;
    }
    lockPiece();
  }

  bool snakeOccupies(int x, int y, bool ignore_tail) const {
    const int last = ignore_tail ? snake_len_ - 1 : snake_len_;
    for (int i = 0; i < last; ++i) {
      if (snake_x_[i] == x && snake_y_[i] == y) return true;
    }
    return false;
  }

  void placeFood() {
    for (int attempt = 0; attempt < 40; ++attempt) {
      const int x = static_cast<int>(nextRng() % kSnakeCols);
      const int y = static_cast<int>(nextRng() % kSnakeRows);
      if (snakeOccupies(x, y, false)) continue;
      food_x_ = x;
      food_y_ = y;
      paintGridCell(food_x_, food_y_, 5, kSnakeX, kSnakeY, kSnakeCell);
      return;
    }
  }

  void beginSnake() {
    fadeOut();
    mode_ = Mode::Snake;
    score_ = 0;
    level_ = 1;
    held_ = nullptr;
    snake_len_ = 3;
    snake_dir_ = 2;
    snake_next_ = 2;
    snake_tick_ = 0;
    snake_x_[0] = 4;
    snake_y_[0] = 8;
    snake_x_[1] = 3;
    snake_y_[1] = 8;
    snake_x_[2] = 2;
    snake_y_[2] = 8;
    const Palette ink = palette();
    fillScreen(ink.bg);
    drawRect(kSnakeX - 2, kSnakeY - 2, kSnakeCols * kSnakeCell + 3, kSnakeRows * kSnakeCell + 3,
             ink.accent);
    for (int y = 0; y < kSnakeRows; ++y) {
      for (int x = 0; x < kSnakeCols; ++x) {
        paintGridCell(x, y, 0, kSnakeX, kSnakeY, kSnakeCell);
      }
    }
    for (int i = 0; i < snake_len_; ++i) {
      paintGridCell(snake_x_[i], snake_y_[i], i == 0 ? 4 : 3, kSnakeX, kSnakeY, kSnakeCell);
    }
    placeFood();
    drawBack(false);
    drawQuad("LEFT", "UP", "DOWN", "RIGHT", nullptr);
    fadeIn();
  }

  void stepSnake(bool down, bool rose, int tx, int ty) {
    if (mode_ != Mode::Snake) return;
    const char* held = nullptr;
    if (down && contains(quadBox(0), tx, ty)) held = "LEFT";
    else if (down && contains(quadBox(1), tx, ty)) held = "UP";
    else if (down && contains(quadBox(2), tx, ty)) held = "DOWN";
    else if (down && contains(quadBox(3), tx, ty)) held = "RIGHT";
    if (held != held_) {
      drawQuad("LEFT", "UP", "DOWN", "RIGHT", held);
      held_ = held;
    }
    if (rose && held != nullptr) {
      int next = snake_dir_;
      if (std::strcmp(held, "LEFT") == 0) next = 0;
      else if (std::strcmp(held, "UP") == 0) next = 1;
      else if (std::strcmp(held, "RIGHT") == 0) next = 2;
      else if (std::strcmp(held, "DOWN") == 0) next = 3;
      if ((next + 2) % 4 != snake_dir_) snake_next_ = next;
    }
    snake_tick_ += 1;
    if (snake_tick_ < 10) return;
    snake_tick_ = 0;
    snake_dir_ = snake_next_;
    int hx = snake_x_[0];
    int hy = snake_y_[0];
    if (snake_dir_ == 0) hx -= 1;
    else if (snake_dir_ == 1) hy -= 1;
    else if (snake_dir_ == 2) hx += 1;
    else hy += 1;
    if (hx < 0 || hy < 0 || hx >= kSnakeCols || hy >= kSnakeRows) {
      showOver("BONK");
      return;
    }
    const bool eat = hx == food_x_ && hy == food_y_;
    if (snakeOccupies(hx, hy, !eat)) {
      showOver("BONK");
      return;
    }
    const int tail_x = snake_x_[snake_len_ - 1];
    const int tail_y = snake_y_[snake_len_ - 1];
    if (eat && snake_len_ < kSnakeMax) {
      snake_len_ += 1;
      score_ += 1;
    }
    for (int i = snake_len_ - 1; i > 0; --i) {
      snake_x_[i] = snake_x_[i - 1];
      snake_y_[i] = snake_y_[i - 1];
    }
    snake_x_[0] = static_cast<uint8_t>(hx);
    snake_y_[0] = static_cast<uint8_t>(hy);
    if (!eat) paintGridCell(tail_x, tail_y, 0, kSnakeX, kSnakeY, kSnakeCell);
    if (snake_len_ > 1) {
      paintGridCell(snake_x_[1], snake_y_[1], 3, kSnakeX, kSnakeY, kSnakeCell);
    }
    paintGridCell(hx, hy, 4, kSnakeX, kSnakeY, kSnakeCell);
    if (eat) placeFood();
  }

  lgfx::Bus_SPI bus_;
  lgfx::Panel_ST7796 panel_;
  lgfx::Light_PWM light_;
  lgfx::LGFX_Sprite idle_sprite_;
  lgfx::LGFX_Sprite blink_sprite_;
  lgfx::LGFX_Sprite mouth_sprite_;
  lgfx::LGFX_Sprite friend_a_;
  lgfx::LGFX_Sprite friend_b_;
  lgfx::LGFX_Sprite friend_c_;
  Preferences prefs_;
  bool ready_ = false;
  bool visible_ = false;
  bool sprite_ok_ = false;
  bool friend_ok_ = false;
  bool needs_dirty_ = false;
  bool placed_ = false;
  bool touch_down_ = false;
  bool shot_ = false;
  bool regions_logged_ = false;
  bool boss_ = false;
  bool banner_ = false;
  bool away_drawn_ = false;
  bool away_phase_ = false;
  Mode mode_ = Mode::Wander;
  Presence presence_ = Presence::Solo;
  Mood mood_ = Mood::Roam;
  CatFrame frame_ = kCatIdle;
  uint8_t theme_ = 0;
  int cat_w_ = 122;
  int cat_h_ = 92;
  int cat_x_ = 0;
  int cat_y_ = 0;
  int vx_ = 1;
  int vy_ = 1;
  int speed_ = 1;
  int glance_dx_ = 0;
  int glance_dy_ = 0;
  int glance_face_ = 0;
  int lure_x_ = 0;
  int lure_y_ = 0;
  int walk_tick_ = 0;
  bool slow_ = false;
  bool glance_step_ = false;
  int origin_x_ = 0;
  int origin_y_ = 0;
  int form_dir_ = 1;
  int player_x_ = 0;
  int shot_x_ = 0;
  int shot_y_ = 0;
  int score_ = 0;
  int level_ = 1;
  int unit_n_ = 0;
  int step_px_ = 2;
  int drop_px_ = 10;
  int barrier_n_ = 0;
  int boss_hp_ = 1;
  int boss_max_ = 1;
  int lives_ = 3;
  int foe_x_ = 0;
  int foe_y_ = 0;
  int foe_cool_ = 0;
  int foe_gap_ = 40;
  bool foe_shot_ = false;
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
  bool beam_on_ = false;
  bool drop_on_ = false;
  bool boss_open_ = false;
  bool pause_advance_ = false;
  bool custom_from_pause_ = false;
  uint8_t terrain_ = 0;
  uint8_t drop_which_ = 0;
  uint8_t gun_equip_ = 0;
  uint8_t gun_unlock_ = 0;
  uint8_t gun_lv_[kGunCount] = {};
  uint8_t laser_color_ = 0;
  uint8_t boss_style_ = 0;
  uint8_t boss_shield_ = 0;
  uint8_t boss_shield_max_ = 0;
  uint8_t boss_layers_ = 0;
  uint8_t boss_layer_max_ = 0;
  uint8_t sky_kind_ = 0;
  bool buddy_ = false;
  bool raid_ = false;
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
  int piece_ = 0;
  int rot_ = 0;
  int piece_x_ = 3;
  int piece_y_ = 0;
  int fall_ = 0;
  int tetris_hold_ = 0;
  uint16_t friend_key_ = 0;
  uint8_t hunger_ = 8;
  uint8_t mood_level_ = 8;
  int snake_len_ = 0;
  int snake_dir_ = 2;
  int snake_next_ = 2;
  int snake_tick_ = 0;
  int food_x_ = 0;
  int food_y_ = 0;
  uint8_t well_[kCols * kRows] = {};
  uint8_t snake_x_[kSnakeMax] = {};
  uint8_t snake_y_[kSnakeMax] = {};
  const char* held_ = nullptr;
  uint32_t rng_ = 1;
  uint32_t shown_at_ = 0;
  uint32_t step_at_ = 0;
  uint32_t advance_at_ = 0;
  uint32_t mood_until_ = 0;
  uint32_t pause_at_ = 0;
  uint32_t heading_until_ = 0;
  uint32_t glance_face_until_ = 0;
  uint32_t away_at_ = 0;
  uint32_t pal_next_ = 0;
  uint32_t need_at_ = 0;
  Pal pals_[2];
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
