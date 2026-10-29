#include "board.h"

#include "cat_faces.h"
#include "hardware_config.h"
#include "resistive_touch.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>

#include <cstdio>

namespace {

// Partial updates: each face is decoded into a sprite once. Later frames copy
// that sprite and erase only the sliver the picture left behind. The SPI write
// clock stays at the rate that already drew a stable picture.
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
    sprite_ok_ = bake(idle_sprite_, kCatIdle) && bake(blink_sprite_, kCatBlink) &&
                 bake(mouth_sprite_, kCatMouth);
    const CatFace& face = cat_frame(kCatIdle);
    cat_w_ = face.width;
    cat_h_ = face.height;
    fillScreen(TFT_BLACK);
    visible_ = false;
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
    mode_ = Mode::Wander;
    placed_ = false;
    shot_ = false;
    if (ready_) {
      fillScreen(TFT_BLACK);
    }
    visible_ = false;
  }

  bool isVisible() const override { return visible_; }

  void tick() override {
    if (!visible_ || !ready_) {
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
    const char* button = "none";
    if (down && inBar(ty)) {
      if (mode_ == Mode::Home) {
        const int slot = barSlot(tx);
        button = slot == 0 ? "INVADERS" : slot == 1 ? "CALENDAR" : "WALK";
      } else if (mode_ == Mode::Calendar || mode_ == Mode::Over) {
        button = "HOME";
      } else if (mode_ == Mode::Play) {
        const int slot = barSlot(tx);
        button = slot == 0 ? "LEFT" : slot == 1 ? "FIRE" : "RIGHT";
      }
    }
    touch_log(raw, tx, ty, button, down && inBar(ty));
    const bool rose = down && !touch_down_;
    touch_down_ = down;
    if (mode_ == Mode::Wander) {
      if (rose && touch_hits(raw, cat_x_, cat_y_, cat_w_, cat_h_)) {
        beginHome();
        return;
      }
      stepWalk(now);
      return;
    }
    if (mode_ == Mode::Home) {
      if (rose) {
        pickHome(tx, ty);
      }
      return;
    }
    if (mode_ == Mode::Calendar) {
      if (rose && inBar(ty)) {
        beginHome();
      }
      return;
    }
    if (mode_ == Mode::Advance) {
      if (static_cast<int32_t>(now - advance_at_) >= 0) {
        level_ += 1;
        beginLevel();
      }
      return;
    }
    if (mode_ == Mode::Play) {
      stepGame(down, rose, tx, ty);
      return;
    }
    if (rose && inBar(ty)) {
      beginHome();
    }
  }

 private:
  enum class Mode : uint8_t { Wander, Home, Calendar, Play, Advance, Over };

  static constexpr uint32_t kBlinkEveryMs = 3400;
  static constexpr uint32_t kBlinkAtMs = 1600;
  static constexpr uint32_t kBlinkHoldMs = 140;
  static constexpr uint32_t kMouthEveryMs = 7600;
  static constexpr uint32_t kMouthAtMs = 2200;
  static constexpr uint32_t kMouthHoldMs = 380;
  static constexpr uint32_t kWalkMs = 16;
  static constexpr uint32_t kStepMs = 32;
  static constexpr int kBarH = 72;
  static constexpr int kLevels = 100;
  static constexpr int kAliens = 8;
  static constexpr int kHomeRowY = 150;
  static constexpr int kHomeRowH = 78;
  static constexpr int kAlienW = 24;
  static constexpr int kAlienH = 16;
  static constexpr int kPitch = 34;
  static constexpr int kPlayerW = 36;
  static constexpr int kPlayerH = 14;
  static constexpr int kShotW = 4;
  static constexpr int kShotH = 10;

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

  bool inBar(int y) const { return y >= kPanelHeight - kBarH; }

  int barSlot(int x) const {
    if (x < kPanelWidth / 3) {
      return 0;
    }
    if (x < (2 * kPanelWidth) / 3) {
      return 1;
    }
    return 2;
  }

  int playerY() const { return kPanelHeight - kBarH - 22; }

  CatFrame faceAt(uint32_t now) const {
    const uint32_t elapsed = now - shown_at_;
    const uint32_t blink_pos = elapsed % kBlinkEveryMs;
    const uint32_t mouth_pos = elapsed % kMouthEveryMs;
    if (blink_pos >= kBlinkAtMs && blink_pos < kBlinkAtMs + kBlinkHoldMs) {
      return kCatBlink;
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

  // Erase the part of the old box that the new box does not cover. Call this
  // after the new picture is already on the panel, so the overlap never goes black.
  void uncover(int ox, int oy, int nx, int ny, int w, int h) {
    if (w <= 0 || h <= 0 || (ox == nx && oy == ny)) {
      return;
    }
    if (ox < nx) {
      fillRect(ox, oy, nx - ox, h, TFT_BLACK);
    } else if (nx < ox) {
      fillRect(nx + w, oy, ox - nx, h, TFT_BLACK);
    }
    if (oy < ny) {
      fillRect(ox, oy, w, ny - oy, TFT_BLACK);
    } else if (ny < oy) {
      fillRect(ox, ny + h, w, oy - ny, TFT_BLACK);
    }
  }

  void paintCat(int x, int y, CatFrame frame) {
    if (sprite_ok_) {
      spriteFor(frame).pushSprite(x, y);
      return;
    }
    const CatFace& face = cat_frame(frame);
    drawPng(face.png, face.length, x, y);
  }

  void drawLabel(const char* text, int x, int y, int w, int h, uint16_t bg) {
    fillRect(x, y, w, h, bg);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString(text, x + w / 2, y + h / 2);
  }

  void drawHomeBar() {
    const char* labels[3] = {"INVADERS", "CALENDAR", "WALK"};
    const int y = kPanelHeight - kBarH;
    const int slot_w = kPanelWidth / 3;
    for (int i = 0; i < 3; ++i) {
      const int x = i * slot_w;
      const int w = (i == 2) ? kPanelWidth - x : slot_w - 4;
      drawLabel(labels[i], x, y, w, kBarH, color565(28, 32, 40));
    }
  }

  void drawHomeButton(const char* text) {
    drawLabel(text, 0, kPanelHeight - kBarH, kPanelWidth, kBarH, color565(28, 32, 40));
  }

  void drawGameBar(int held) {
    const char* labels[3] = {"LEFT", "FIRE", "RIGHT"};
    const int y = kPanelHeight - kBarH;
    const int slot_w = kPanelWidth / 3;
    for (int i = 0; i < 3; ++i) {
      const int x = i * slot_w;
      const int w = (i == 2) ? kPanelWidth - x : slot_w - 4;
      const uint16_t bg = (held == i) ? color565(70, 96, 150) : color565(28, 32, 40);
      drawLabel(labels[i], x, y, w, kBarH, bg);
    }
    bar_slot_ = held;
  }

  void drawScore() {
    fillRect(0, 0, kPanelWidth, 26, TFT_BLACK);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(TFT_WHITE);
    char line[40];
    if (boss_) {
      snprintf(line, sizeof line, "BOSS %d  HP %d  %d", level_, boss_hp_, score_);
    } else {
      snprintf(line, sizeof line, "LV %d/%d  %d", level_, kLevels, score_);
    }
    drawString(line, 8, 13);
  }

  void drawAlienAt(int x, int y) {
    fillRect(x, y, kAlienW, kAlienH, color565(70, 190, 90));
    fillRect(x + 4, y + 4, 4, 4, TFT_BLACK);
    fillRect(x + kAlienW - 8, y + 4, 4, 4, TFT_BLACK);
  }

  void drawBossAt(int x, int y) {
    fillRect(x, y, boss_w_, boss_h_, color565(70, 190, 90));
    int inner = boss_max_ > 0 ? (boss_w_ - 8) * boss_hp_ / boss_max_ : 4;
    if (inner < 4) {
      inner = 4;
    }
    fillRect(x + 4, y + boss_h_ - 8, inner, 4, color565(255, 214, 64));
  }

  void drawPlayerAt(int x) {
    const int y = playerY();
    fillRect(x, y, kPlayerW, kPlayerH, color565(214, 224, 238));
    fillRect(x + 6, y + 5, kPlayerW - 12, kPlayerH - 5, color565(36, 64, 112));
  }

  void beginWander() {
    mode_ = Mode::Wander;
    shot_ = false;
    placed_ = false;
    frame_ = kCatIdle;
    cat_x_ = 40;
    cat_y_ = 80;
    vx_ = 1;
    vy_ = 1;
    walk_tick_ = 0;
    shown_at_ = millis();
    step_at_ = 0;
    fillScreen(TFT_BLACK);
  }

  void beginHome() {
    mode_ = Mode::Home;
    shot_ = false;
    placed_ = false;
    fillScreen(TFT_BLACK);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString("JITTERS", kPanelWidth / 2, 48);
    setTextColor(color565(180, 170, 150));
    drawString("pick a game", kPanelWidth / 2, 78);
    drawLabel("INVADERS", 16, kHomeRowY, kPanelWidth - 32, kHomeRowH, color565(36, 64, 112));
    drawLabel("CALENDAR", 16, kHomeRowY + kHomeRowH + 16, kPanelWidth - 32, kHomeRowH,
              color565(36, 72, 64));
    drawHomeBar();
  }

  void beginCalendar() {
    mode_ = Mode::Calendar;
    fillScreen(TFT_BLACK);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString("CALENDAR", kPanelWidth / 2, 70);
    drawString(__DATE__, kPanelWidth / 2, 130);
    setTextColor(color565(220, 180, 90));
    drawString("calendar not connected", kPanelWidth / 2, 180);
    drawHomeButton("HOME");
  }

  void pickHome(int tx, int ty) {
    const int calendar_y = kHomeRowY + kHomeRowH + 16;
    if (ty >= kHomeRowY && ty < kHomeRowY + kHomeRowH) {
      beginGame();
      return;
    }
    if (ty >= calendar_y && ty < calendar_y + kHomeRowH) {
      beginCalendar();
      return;
    }
    if (!inBar(ty)) {
      return;
    }
    const int slot = barSlot(tx);
    if (slot == 0) {
      beginGame();
    } else if (slot == 1) {
      beginCalendar();
    } else {
      beginWander();
    }
  }

  void tuneLevel() {
    const int tier = (level_ - 1) / 10;
    boss_ = (level_ % 10) == 0;
    step_px_ = 1 + tier / 4;
    if (step_px_ > 3) {
      step_px_ = 3;
    }
    move_every_ = tier < 5 ? 2 : 1;
    drop_px_ = 8 + tier;
    if (drop_px_ > 16) {
      drop_px_ = 16;
    }
    if (boss_) {
      count_ = 1;
      boss_max_ = 2 + level_ / 10;
      boss_hp_ = boss_max_;
      boss_w_ = 78;
      boss_h_ = 28;
      step_px_ = 1 + tier / 5;
      move_every_ = 2;
      alive_[0] = true;
      return;
    }
    count_ = 4 + ((level_ - 1) % 10) / 2;
    if (count_ > kAliens) {
      count_ = kAliens;
    }
    for (int i = 0; i < kAliens; ++i) {
      alive_[i] = i < count_;
    }
  }

  void beginLevel() {
    mode_ = Mode::Play;
    shot_ = false;
    bar_slot_ = -2;
    move_wait_ = 0;
    form_dir_ = 1;
    form_y_ = 40;
    player_x_ = (kPanelWidth - kPlayerW) / 2;
    tuneLevel();
    if (boss_) {
      form_x_ = (kPanelWidth - boss_w_) / 2;
    } else {
      const int span = (count_ - 1) * kPitch + kAlienW;
      form_x_ = (kPanelWidth - span) / 2;
    }
    fillScreen(TFT_BLACK);
    drawScore();
    drawGameBar(-1);
    startWrite();
    if (boss_) {
      drawBossAt(form_x_, form_y_);
    } else {
      for (int i = 0; i < count_; ++i) {
        drawAlienAt(form_x_ + i * kPitch, form_y_);
      }
    }
    drawPlayerAt(player_x_);
    endWrite();
  }

  void beginGame() {
    score_ = 0;
    level_ = 1;
    beginLevel();
  }

  void beginAdvance() {
    mode_ = Mode::Advance;
    shot_ = false;
    advance_at_ = millis() + 800;
    fillRect(24, 168, kPanelWidth - 48, 52, TFT_BLACK);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    char line[24];
    const int next = level_ + 1;
    if (next % 10 == 0) {
      snprintf(line, sizeof line, "BOSS %d", next);
    } else {
      snprintf(line, sizeof line, "LEVEL %d", next);
    }
    drawString(line, kPanelWidth / 2, 194);
  }

  void endRound(bool won) {
    mode_ = Mode::Over;
    shot_ = false;
    fillRect(24, 160, kPanelWidth - 48, 56, TFT_BLACK);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString(won ? "CLEARED" : "CAUGHT", kPanelWidth / 2, 188);
    drawHomeButton("HOME");
  }

  void stepWalk(uint32_t now) {
    const CatFrame next = faceAt(now);
    const int max_x = kPanelWidth - cat_w_;
    const int max_y = kPanelHeight - cat_h_;
    walk_tick_ += 1;
    const int dy = (walk_tick_ & 1) == 0 ? vy_ : 0;
    int nx = cat_x_ + vx_;
    int ny = cat_y_ + dy;
    if (nx < 0 || nx > max_x) {
      vx_ = -vx_;
      nx = cat_x_ + vx_;
    }
    if (ny < 0 || ny > max_y) {
      vy_ = -vy_;
      ny = cat_y_ + vy_;
    }
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > max_x) nx = max_x;
    if (ny > max_y) ny = max_y;
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

  bool overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) const {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
  }

  void addScore(int points) {
    score_ += points;
    if (score_ > 99999) {
      score_ = 99999;
    }
    drawScore();
  }

  bool hitAlien(int sx, int sy) {
    if (boss_) {
      if (!alive_[0] || !overlap(sx, sy, kShotW, kShotH, form_x_, form_y_, boss_w_, boss_h_)) {
        return false;
      }
      boss_hp_ -= 1;
      addScore(10);
      if (boss_hp_ <= 0) {
        fillRect(form_x_, form_y_, boss_w_, boss_h_, TFT_BLACK);
        alive_[0] = false;
      } else {
        drawBossAt(form_x_, form_y_);
      }
      return true;
    }
    for (int i = 0; i < count_; ++i) {
      if (!alive_[i]) {
        continue;
      }
      const int ax = form_x_ + i * kPitch;
      if (!overlap(sx, sy, kShotW, kShotH, ax, form_y_, kAlienW, kAlienH)) {
        continue;
      }
      fillRect(ax, form_y_, kAlienW, kAlienH, TFT_BLACK);
      alive_[i] = false;
      addScore(10);
      return true;
    }
    return false;
  }

  bool anyAlive() const {
    const int n = boss_ ? 1 : count_;
    for (int i = 0; i < n; ++i) {
      if (alive_[i]) {
        return true;
      }
    }
    return false;
  }

  void moveFormation() {
    if (move_wait_ > 0) {
      move_wait_ -= 1;
      return;
    }
    move_wait_ = move_every_;
    const int old_x = form_x_;
    const int old_y = form_y_;
    const int body_w = boss_ ? boss_w_ : kAlienW;
    int left_x = form_x_;
    int right_x = form_x_ + body_w;
    if (!boss_) {
      int left = -1;
      int right = -1;
      for (int i = 0; i < count_; ++i) {
        if (!alive_[i]) {
          continue;
        }
        if (left < 0) {
          left = i;
        }
        right = i;
      }
      if (left < 0) {
        return;
      }
      left_x = form_x_ + left * kPitch;
      right_x = form_x_ + right * kPitch + kAlienW;
    } else if (!alive_[0]) {
      return;
    }
    if (form_dir_ > 0 && right_x + step_px_ >= kPanelWidth - 4) {
      form_dir_ = -1;
      form_y_ += drop_px_;
    } else if (form_dir_ < 0 && left_x - step_px_ <= 4) {
      form_dir_ = 1;
      form_y_ += drop_px_;
    } else {
      form_x_ += form_dir_ * step_px_;
    }
    if (boss_) {
      drawBossAt(form_x_, form_y_);
      uncover(old_x, old_y, form_x_, form_y_, boss_w_, boss_h_);
      return;
    }
    for (int i = 0; i < count_; ++i) {
      if (!alive_[i]) {
        continue;
      }
      const int ox = old_x + i * kPitch;
      const int nx = form_x_ + i * kPitch;
      drawAlienAt(nx, form_y_);
      uncover(ox, old_y, nx, form_y_, kAlienW, kAlienH);
    }
  }

  void stepGame(bool down, bool rose, int tx, int ty) {
    const int slot = (down && inBar(ty)) ? barSlot(tx) : -1;
    if (slot != bar_slot_) {
      drawGameBar(slot);
    }
    const int old_player = player_x_;
    // The panel's horizontal axis is mirrored relative to the labels.
    if (slot == 0) {
      player_x_ += 5;
    } else if (slot == 2) {
      player_x_ -= 5;
    }
    if (player_x_ < 0) {
      player_x_ = 0;
    }
    if (player_x_ > kPanelWidth - kPlayerW) {
      player_x_ = kPanelWidth - kPlayerW;
    }
    startWrite();
    if (player_x_ != old_player) {
      drawPlayerAt(player_x_);
      uncover(old_player, playerY(), player_x_, playerY(), kPlayerW, kPlayerH);
    }
    if (rose && slot == 1 && !shot_) {
      shot_ = true;
      shot_x_ = player_x_ + kPlayerW / 2 - kShotW / 2;
      shot_y_ = playerY() - kShotH - 1;
      fillRect(shot_x_, shot_y_, kShotW, kShotH, color565(255, 214, 64));
    } else if (shot_) {
      const int ox = shot_x_;
      const int oy = shot_y_;
      const int ny = shot_y_ - 10;
      if (ny < 28 || hitAlien(shot_x_, ny)) {
        fillRect(ox, oy, kShotW, kShotH, TFT_BLACK);
        shot_ = false;
        if (boss_ && alive_[0]) {
          drawBossAt(form_x_, form_y_);
        }
      } else {
        shot_y_ = ny;
        fillRect(shot_x_, shot_y_, kShotW, kShotH, color565(255, 214, 64));
        uncover(ox, oy, shot_x_, shot_y_, kShotW, kShotH);
      }
    }
    if (mode_ == Mode::Play) {
      moveFormation();
    }
    endWrite();
    if (mode_ != Mode::Play) {
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
    const int body_h = boss_ ? boss_h_ : kAlienH;
    if (form_y_ + body_h >= playerY()) {
      endRound(false);
    }
  }

  lgfx::Bus_SPI bus_;
  lgfx::Panel_ST7796 panel_;
  lgfx::Light_PWM light_;
  lgfx::LGFX_Sprite idle_sprite_;
  lgfx::LGFX_Sprite blink_sprite_;
  lgfx::LGFX_Sprite mouth_sprite_;
  bool ready_ = false;
  bool visible_ = false;
  bool sprite_ok_ = false;
  bool placed_ = false;
  bool touch_down_ = false;
  bool shot_ = false;
  bool alive_[kAliens] = {};
  Mode mode_ = Mode::Wander;
  CatFrame frame_ = kCatIdle;
  int cat_w_ = 122;
  int cat_h_ = 92;
  int cat_x_ = 0;
  int cat_y_ = 0;
  int vx_ = 1;
  int vy_ = 1;
  int walk_tick_ = 0;
  int form_x_ = 0;
  int form_y_ = 0;
  int form_dir_ = 1;
  int player_x_ = 0;
  int shot_x_ = 0;
  int shot_y_ = 0;
  int score_ = 0;
  int level_ = 1;
  int count_ = 4;
  int step_px_ = 1;
  int drop_px_ = 8;
  int move_every_ = 2;
  int move_wait_ = 0;
  int boss_w_ = 78;
  int boss_h_ = 28;
  int boss_hp_ = 1;
  int boss_max_ = 1;
  int bar_slot_ = -2;
  bool boss_ = false;
  uint32_t shown_at_ = 0;
  uint32_t step_at_ = 0;
  uint32_t advance_at_ = 0;
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
