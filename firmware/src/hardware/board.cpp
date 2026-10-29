#include "board.h"

#include "cat_faces.h"
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
      : idle_sprite_(this), blink_sprite_(this), mouth_sprite_(this) {
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
    makeStars();
    sprite_ok_ = bake(idle_sprite_, kCatIdle) && bake(blink_sprite_, kCatBlink) &&
                 bake(mouth_sprite_, kCatMouth);
    const CatFace& face = cat_frame(kCatIdle);
    cat_w_ = face.width;
    cat_h_ = face.height;
    rng_ = esp_random();
    if (rng_ == 0) rng_ = 1;
    fillScreen(palette().bg);
    visible_ = false;
    Serial.printf("FRAME walk_ms=%lu game_ms=%lu\n", static_cast<unsigned long>(kWalkMs),
                  static_cast<unsigned long>(kStepMs));
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
    int tx = 0;
    int ty = 0;
    TouchRaw raw;
    const bool down = touch_read(raw, tx, ty);
    const char* where = controlAt(tx, ty);
    const bool on = down && std::strcmp(where, "none") != 0;
    touch_log(raw, tx, ty, where, on);
    const bool rose = down && !touch_down_;
    touch_down_ = down;

    if ((mode_ == Mode::Play || mode_ == Mode::Tetris || mode_ == Mode::Snake ||
         mode_ == Mode::Advance) &&
        rose && on && std::strcmp(where, "BACK") == 0) {
      beginHome();
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
      } else if (std::strcmp(where, "THEME") == 0) {
        theme_ = static_cast<uint8_t>((theme_ + 1) & 3);
        prefs_.putUChar("theme", theme_);
        beginHome();
      } else if (std::strcmp(where, "WALK") == 0) {
        beginWander();
      }
      return;
    }
    if (mode_ == Mode::Calendar || mode_ == Mode::Over) {
      if (rose && on && std::strcmp(where, "HOME") == 0) {
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
  enum class Mode : uint8_t { Wander, Home, Calendar, Play, Advance, Over, Away, Tetris, Snake };
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
  };

  static constexpr uint32_t kBlinkEveryMs = 3400;
  static constexpr uint32_t kBlinkAtMs = 1600;
  static constexpr uint32_t kBlinkHoldMs = 140;
  static constexpr uint32_t kMouthEveryMs = 7600;
  static constexpr uint32_t kMouthAtMs = 2200;
  static constexpr uint32_t kMouthHoldMs = 380;
  static constexpr uint32_t kWalkMs = 8;
  static constexpr uint32_t kStepMs = 16;
  static constexpr int kLevels = 100;
  static constexpr int kMaxUnits = 24;
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
    return "none";
  }

  void logRegions() {
    if (regions_logged_) {
      return;
    }
    regions_logged_ = true;
    const Box boxes[] = {playBox(),   invadersBox(), tetrisBox(), snakeBox(), calendarBox(),
                         themeBox(), walkBox(),     homeBox(),   leftBox(),  fireBox(),
                         rightBox(), backBox()};
    const char* names[] = {"PLAY", "INVADERS", "TETRIS", "SNAKE", "CALENDAR", "THEME",
                           "WALK", "HOME",     "LEFT",   "FIRE",  "RIGHT",     "BACK"};
    for (int i = 0; i < 12; ++i) {
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
    const uint16_t ink = palette().star;
    for (int i = 0; i < kStars; ++i) {
      drawPixel(stars_[i].x, stars_[i].y, ink);
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
    if (y + h > 404 && y < kButtonY && mode_ == Mode::Play) {
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
    if (placed_ && nx == cat_x_ && ny == cat_y_ && next == frame_) {
      return;
    }
    startWrite();
    paintCat(nx, ny, next);
    if (placed_) {
      uncover(cat_x_, cat_y_, nx, ny, cat_w_, cat_h_);
    }
    endWrite();
    cat_x_ = nx;
    cat_y_ = ny;
    frame_ = next;
    placed_ = true;
  }

  void beginHome() {
    fadeOut();
    mode_ = Mode::Home;
    shot_ = false;
    foe_shot_ = false;
    placed_ = false;
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString("JITTERS", kPanelWidth / 2, 20);
    drawCard("INVADERS", "100 levels", invadersBox());
    drawCard("TETRIS", "stack", tetrisBox());
    drawCard("SNAKE", "classic", snakeBox());
    drawCard("CALENDAR", "today", calendarBox());
    drawCard(themeName(), "tap to switch", themeBox());
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

  int unitW(const Unit& unit) const { return unit.kind == 2 ? kBossW : kAlienW; }
  int unitH(const Unit& unit) const { return unit.kind == 2 ? kBossH : kAlienH; }

  void drawAlienAt(int x, int y, uint8_t kind) {
    const Palette ink = palette();
    const uint16_t body = kind == 0 ? ink.low : ink.top;
    const uint16_t eye = ink.bg;
    if (kind == 2) {
      fillRect(x + 8, y + 4, 28, 16, body);
      fillRect(x + 4, y, 6, 8, body);
      fillRect(x + 34, y, 6, 8, body);
      fillRect(x + 14, y + 8, 4, 4, eye);
      fillRect(x + 26, y + 8, 4, 4, eye);
      fillRect(x + 6, y + 20, 8, 6, body);
      fillRect(x + 30, y + 20, 8, 6, body);
      int inner = boss_max_ > 0 ? (kBossW - 8) * boss_hp_ / boss_max_ : 4;
      if (inner < 4) inner = 4;
      fillRect(x + 4, y + kBossH - 4, inner, 3, ink.shot);
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
    if (boss_) {
      snprintf(line, sizeof line, "BOSS %d  %d  x%d", level_, score_, lives_);
    } else {
      snprintf(line, sizeof line, "LV %d  %d  x%d", level_, score_, lives_);
    }
    drawString(line, 8, 12);
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

  void buildLevel() {
    unit_n_ = 0;
    boss_ = (level_ % 10) == 0;
    barrier_n_ = 0;
    const int tier = (level_ - 1) / 10;
    drop_px_ = 10 + tier;
    if (drop_px_ > 16) drop_px_ = 16;
    if (boss_) {
      boss_max_ = 4 + level_ / 10;
      boss_hp_ = boss_max_;
      addUnit(0, 0, 2);
      step_px_ = 2 + static_cast<int>(nextRng() % 2) + tier / 2;
      if (step_px_ > 5) step_px_ = 5;
      origin_x_ = (kPanelWidth - kBossW) / 2;
      origin_y_ = 48;
      form_dir_ = 1;
      return;
    }
    const int shape = static_cast<int>(nextRng() % 5);
    int cols = 4 + static_cast<int>(nextRng() % 4);
    int rows = 2;
    if (level_ > 25) rows += static_cast<int>(nextRng() % 2);
    if (level_ > 55) rows += 1;
    if (rows > 4) rows = 4;
    const int pitch_x = 30;
    const int pitch_y = 22;
    const int yellow_rows = 1 + static_cast<int>(nextRng() % rows);
    step_px_ = 2 + level_ / 14 + static_cast<int>(nextRng() % 3);
    if (step_px_ > 6) step_px_ = 6;
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
    barrier_n_ = static_cast<int>(nextRng() % 4);
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
    paintPlanet();
    for (int i = 0; i < barrier_n_; ++i) {
      drawBarrier(i);
    }
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      drawAlienAt(origin_x_ + units_[i].ox, origin_y_ + units_[i].oy, units_[i].kind);
    }
    drawShipAt(player_x_);
    drawHud();
    drawBack(false);
    drawGameKeys(nullptr);
  }

  void startLevel() {
    mode_ = Mode::Play;
    shot_ = false;
    foe_shot_ = false;
    held_ = nullptr;
    player_x_ = (kPanelWidth - kShipW) / 2;
    buildLevel();
    paintLevel();
  }

  void beginGame() {
    fadeOut();
    score_ = 0;
    level_ = 1;
    lives_ = 3;
    foe_shot_ = false;
    foe_cool_ = 0;
    rng_ = esp_random();
    if (rng_ == 0) rng_ = 1;
    startLevel();
    fadeIn();
  }

  void beginAdvance() {
    mode_ = Mode::Advance;
    shot_ = false;
    advance_at_ = millis() + 700;
    const Palette ink = palette();
    fillRect(36, 168, kPanelWidth - 72, 48, ink.bg);
    drawRect(36, 168, kPanelWidth - 72, 48, ink.accent);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    char line[24];
    const int next = level_ + 1;
    if (next % 10 == 0) {
      snprintf(line, sizeof line, "BOSS %d", next);
    } else {
      snprintf(line, sizeof line, "LEVEL %d", next);
    }
    drawString(line, kPanelWidth / 2, 192);
  }

  void showOver(const char* title) {
    fadeOut();
    mode_ = Mode::Over;
    shot_ = false;
    foe_shot_ = false;
    const Palette ink = palette();
    fillScreen(ink.bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink.ink);
    drawString(title, kPanelWidth / 2, 180);
    char line[24];
    snprintf(line, sizeof line, "LV %d   %d", level_, score_);
    setTextColor(ink.muted);
    drawString(line, kPanelWidth / 2, 214);
    drawKey("HOME", homeBox(), false);
    fadeIn();
  }

  void endRound(bool won) { showOver(won ? "CLEARED" : "CAUGHT"); }

  void playerHit() {
    lives_ -= 1;
    foe_shot_ = false;
    shot_ = false;
    if (lives_ <= 0) {
      endRound(false);
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

  bool hitBarrier(int sx, int sy) {
    for (int i = 0; i < barrier_n_; ++i) {
      if (!barriers_[i].on) continue;
      const int bw = kBarCols * 3;
      const int bh = 7 * 3;
      if (!overlap(sx, sy, kShotW, kShotH, barriers_[i].x, barriers_[i].y, bw, bh)) {
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

  bool hitUnit(int sx, int sy) {
    for (int i = 0; i < unit_n_; ++i) {
      if (!units_[i].alive) continue;
      const int ax = origin_x_ + units_[i].ox;
      const int ay = origin_y_ + units_[i].oy;
      const int aw = unitW(units_[i]);
      const int ah = unitH(units_[i]);
      if (!overlap(sx, sy, kShotW, kShotH, ax, ay, aw, ah)) continue;
      if (units_[i].kind == 2) {
        boss_hp_ -= 1;
        addScore(10);
        if (boss_hp_ <= 0) {
          units_[i].alive = false;
          repair(ax, ay, aw, ah);
        } else {
          drawAlienAt(ax, ay, 2);
        }
        return true;
      }
      units_[i].alive = false;
      repair(ax, ay, aw, ah);
      addScore(10);
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
    startWrite();
    if (player_x_ != old_player) {
      drawShipAt(player_x_);
      uncover(old_player, kShipY, player_x_, kShipY, kShipW, kShipH);
    }
    if (rose && held != nullptr && std::strcmp(held, "FIRE") == 0 && !shot_) {
      shot_ = true;
      shot_x_ = player_x_ + kShipW / 2 - kShotW / 2;
      shot_y_ = kShipY - kShotH - 1;
      fillRect(shot_x_, shot_y_, kShotW, kShotH, palette().shot);
    } else if (shot_) {
      const int ox = shot_x_;
      const int oy = shot_y_;
      const int ny = shot_y_ - 7;
      const bool struck = ny < 26 || hitBarrier(shot_x_, ny) || hitUnit(shot_x_, ny);
      if (struck) {
        repair(ox, oy, kShotW, kShotH);
        shot_ = false;
      } else {
        shot_y_ = ny;
        fillRect(shot_x_, shot_y_, kShotW, kShotH, palette().shot);
        uncover(ox, oy, shot_x_, shot_y_, kShotW, kShotH);
      }
    }
    if (mode_ == Mode::Play) {
      moveFormation();
    }
    bool lost = false;
    if (foe_shot_) {
      const int ox = foe_x_;
      const int oy = foe_y_;
      const int ny = foe_y_ + 6;
      const bool hit_ship =
          overlap(foe_x_, ny, kShotW, kShotH, player_x_, kShipY, kShipW, kShipH);
      const bool blocked = hitBarrier(foe_x_, ny) || ny >= kButtonY;
      if (hit_ship || blocked) {
        repair(ox, oy, kShotW, kShotH);
        foe_shot_ = false;
        lost = hit_ship;
      } else {
        foe_y_ = ny;
        fillRect(foe_x_, foe_y_, kShotW, kShotH, color565(255, 72, 64));
        uncover(ox, oy, foe_x_, foe_y_, kShotW, kShotH);
      }
    } else if (mode_ == Mode::Play) {
      foe_cool_ += 1;
      if (foe_cool_ >= 40) {
        foe_cool_ = 0;
        int alive = 0;
        for (int i = 0; i < unit_n_; ++i) {
          if (units_[i].alive) alive += 1;
        }
        if (alive > 0) {
          int pick = static_cast<int>(nextRng() % static_cast<uint32_t>(alive));
          for (int i = 0; i < unit_n_; ++i) {
            if (!units_[i].alive) continue;
            if (pick > 0) {
              pick -= 1;
              continue;
            }
            foe_shot_ = true;
            foe_x_ = origin_x_ + units_[i].ox + unitW(units_[i]) / 2;
            foe_y_ = origin_y_ + units_[i].oy + unitH(units_[i]);
            fillRect(foe_x_, foe_y_, kShotW, kShotH, color565(255, 72, 64));
            break;
          }
        }
      }
    }
    endWrite();
    if (mode_ != Mode::Play) return;
    if (lost) {
      playerHit();
      return;
    }
    if (!anyAlive()) {
      if (level_ >= kLevels) {
        endRound(true);
      } else {
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
    static const int8_t kShape[7][4][2] = {
        {{0, 0}, {1, 0}, {2, 0}, {3, 0}}, {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{2, 0}, {0, 1}, {1, 1}, {2, 1}}, {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
        {{1, 0}, {2, 0}, {0, 1}, {1, 1}}, {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
    };
    for (int i = 0; i < 4; ++i) {
      int x = kShape[kind][i][0] - 1;
      int y = kShape[kind][i][1];
      for (int turn = 0; turn < rot; ++turn) {
        const int nx = -y;
        y = x;
        x = nx;
      }
      xs[i] = ox + x;
      ys[i] = oy + y;
    }
  }

  bool pieceFits(int kind, int rot, int ox, int oy) const {
    int xs[4];
    int ys[4];
    pieceCells(kind, rot, ox, oy, xs, ys);
    for (int i = 0; i < 4; ++i) {
      if (xs[i] < 0 || xs[i] >= kCols || ys[i] >= kRows) return false;
      if (ys[i] >= 0 && well_[ys[i] * kCols + xs[i]] != 0) return false;
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
    piece_x_ = 4;
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
    for (int y = kRows - 1; y >= 0; --y) {
      bool full = true;
      for (int x = 0; x < kCols; ++x) {
        if (well_[y * kCols + x] == 0) full = false;
      }
      if (!full) continue;
      cleared += 1;
      if (y > 0) {
        memmove(&well_[kCols], well_, static_cast<size_t>(y) * kCols);
      }
      memset(well_, 0, kCols);
      y += 1;
    }
    if (cleared > 0) {
      score_ += cleared * 10;
      if (score_ > 99999) score_ = 99999;
      paintWell();
    }
    if (mode_ == Mode::Tetris) spawnPiece();
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
    spawnPiece();
    fadeIn();
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
    }
    if (rose && held != nullptr && std::strcmp(held, "LEFT") == 0 &&
        pieceFits(piece_, rot_, piece_x_ - 1, piece_y_)) {
      paintPiece(0);
      piece_x_ -= 1;
      paintPiece(piece_ + 1);
    } else if (rose && held != nullptr && std::strcmp(held, "RIGHT") == 0 &&
               pieceFits(piece_, rot_, piece_x_ + 1, piece_y_)) {
      paintPiece(0);
      piece_x_ += 1;
      paintPiece(piece_ + 1);
    } else if (rose && held != nullptr && std::strcmp(held, "ROT") == 0 &&
               pieceFits(piece_, (rot_ + 1) & 3, piece_x_, piece_y_)) {
      paintPiece(0);
      rot_ = (rot_ + 1) & 3;
      paintPiece(piece_ + 1);
    } else if (rose && held != nullptr && std::strcmp(held, "DROP") == 0) {
      paintPiece(0);
      while (pieceFits(piece_, rot_, piece_x_, piece_y_ + 1)) piece_y_ += 1;
      lockPiece();
      return;
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
  Preferences prefs_;
  bool ready_ = false;
  bool visible_ = false;
  bool sprite_ok_ = false;
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
  bool foe_shot_ = false;
  int piece_ = 0;
  int rot_ = 0;
  int piece_x_ = 4;
  int piece_y_ = 0;
  int fall_ = 0;
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
