#include "board.h"

#include "cat_faces.h"
#include "hardware_config.h"
#include "resistive_touch.h"
#include "touch_map.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>

#include <cstdio>
#include <cstring>

namespace {

// Partial updates: each walking face is decoded into a sprite once. Later frames
// copy that sprite and erase only the sliver the picture left behind. The happy
// face is a fourth sprite, used only while the cat is petted, and is skipped if
// the heap cannot hold it. Bars, cards, and the away screen redraw when they
// change, not on every step. The SPI write clock stays at the rate that already
// drew a stable picture.
class PanelDisplay : public lgfx::LGFX_Device, public Display {
 public:
  PanelDisplay()
      : idle_sprite_(this), blink_sprite_(this), mouth_sprite_(this), happy_sprite_(this) {
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
    // Leave room for ArduinoJson. A failed happy sprite still pets via the PNG.
    happy_ok_ = sprite_ok_ && ESP.getFreeHeap() > 48000 && bake(happy_sprite_, kCatHappy);
    const CatFace& face = cat_frame(kCatIdle);
    cat_w_ = face.width;
    cat_h_ = face.height;
    fillScreen(TFT_BLACK);
    visible_ = false;
    // One boot line. happy=0 means the pet face is drawn from the PNG instead of a sprite.
    Serial.printf("FACE sprites=%d happy=%d\n", sprite_ok_ ? 1 : 0, happy_ok_ ? 1 : 0);
    return true;
  }

  void show(const char* expression) override {
    (void)expression;
    if (!ready_) {
      visible_ = false;
      return;
    }
    greet_pending_ = mode_ == Mode::Away;
    beginWander();
    visible_ = true;
  }

  void hide() override {
    pending_ = Pending::None;
    petting_ = false;
    placed_ = false;
    shot_ = false;
    banner_ = false;
    bar_held_ = false;
    restoreBrightness();
    visible_ = false;
    mode_ = Mode::Away;
    away_drawn_ = false;
    if (ready_) {
      drawAway(false);
      away_drawn_ = true;
    }
  }

  void setPresence(const char* presence) override {
    const Presence next = parsePresence(presence);
    if (next == presence_) return;
    presence_ = next;
    if (next == Presence::Away || !ready_ || !visible_) return;
    if (mode_ == Mode::Wander) {
      if (presence_ == Presence::Leaving) showLeaveBanner();
      else hideLeaveBanner();
      drawWanderBar(bar_held_);
      return;
    }
    if (mode_ == Mode::Home || mode_ == Mode::Calendar) {
      drawStatusStrip();
      return;
    }
    if (mode_ == Mode::Play) drawScore();
  }

  bool isVisible() const override { return visible_; }

  void tick() override {
    if (!ready_) return;
    const uint32_t now = millis();
    if (bright_until_ != 0 && static_cast<int32_t>(now - bright_until_) >= 0) {
      bright_until_ = 0;
      setBrightness(255);
    }
    if (!visible_) {
      if (mode_ == Mode::Away) tickAway(now);
      return;
    }
    uint32_t pace = kStepMs;
    if (mode_ == Mode::Wander) {
      pace = attention_ < 40 ? 28 : kWalkMs;
    } else if (mode_ == Mode::Home || mode_ == Mode::Calendar || mode_ == Mode::Over) {
      pace = kWalkMs;
    }
    if (step_at_ != 0 && static_cast<int32_t>(now - step_at_) < static_cast<int32_t>(pace)) {
      return;
    }
    step_at_ = now;
    if (greet_until_ != 0 && static_cast<int32_t>(now - greet_until_) >= 0) {
      greet_until_ = 0;
      if (mode_ == Mode::Wander) drawWanderBar(bar_held_);
      else if (mode_ == Mode::Home || mode_ == Mode::Calendar) drawStatusStrip();
    }
    decayAttention(now);
    if (pending_ != Pending::None && static_cast<int32_t>(now - pending_at_) >= 0) {
      firePending();
      return;
    }

    int tx = 0;
    int ty = 0;
    TouchRaw raw;
    const bool down = touch_read(raw, tx, ty);
    const char* button = "none";
    if (down && inBar(ty)) {
      if (mode_ == Mode::Wander) {
        button = "MENU";
      } else {
        const int slot = barSlot(tx);
        if (slot < 0) {
          button = "none";
        } else if (mode_ == Mode::Home) {
          button = slot == 0 ? "INVADERS" : slot == 1 ? "CALENDAR" : "WALK";
        } else if (mode_ == Mode::Calendar) {
          button = slot == 0 ? "HOME" : slot == 2 ? "WALK" : "SOON";
        } else if (mode_ == Mode::Over) {
          button = slot == 0 ? "HOME" : slot == 1 ? "AGAIN" : "WALK";
        } else if (mode_ == Mode::Play) {
          button = slot == 0 ? "LEFT" : slot == 1 ? "FIRE" : "RIGHT";
        }
      }
    }
    touch_log(raw, tx, ty, button, down && inBar(ty) && strcmp(button, "none") != 0);
    const bool rose = down && !touch_down_;
    touch_down_ = down;
    if (mode_ == Mode::Wander) {
      if (presence_ != Presence::Leaving && pending_ == Pending::None) {
        const bool held = down && inBar(ty);
        if (held != bar_held_) {
          bar_held_ = held;
          drawWanderBar(held);
        }
        if (rose && held) {
          arm(Pending::Home);
        } else if (rose && touch_hits(raw, cat_x_, cat_y_ - last_lift_, cat_w_, cat_h_)) {
          if (petting_) {
            beginHome();
            return;
          }
          startPet(now);
        }
      }
      if (presence_ != Presence::Leaving) stepWalk(now);
      return;
    }
    if (mode_ == Mode::Home || mode_ == Mode::Calendar || mode_ == Mode::Over) {
      tickChoice(down, rose, tx, ty);
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
    }
  }

 private:
  enum class Mode : uint8_t { Wander, Home, Calendar, Play, Advance, Over, Away };
  enum class Presence : uint8_t { Solo, Linked, Leaving, Away };
  enum class Pending : uint8_t { None, Home, Invaders, Calendar, Walk, Again };

  static constexpr uint32_t kBlinkEveryMs = 3400;
  static constexpr uint32_t kBlinkAtMs = 1600;
  static constexpr uint32_t kBlinkHoldMs = 140;
  static constexpr uint32_t kMouthEveryMs = 7600;
  static constexpr uint32_t kMouthAtMs = 2200;
  static constexpr uint32_t kMouthHoldMs = 380;
  static constexpr uint32_t kWalkMs = 16;
  static constexpr uint32_t kStepMs = 32;
  static constexpr int kBarH = 72;
  static constexpr int kStatusH = 26;
  static constexpr int kBannerH = 28;
  static constexpr int kCardX = 16;
  static constexpr int kCardW = kPanelWidth - 32;
  static constexpr int kCardH = 96;
  static constexpr int kCard1Y = 104;
  static constexpr int kCard2Y = 216;
  static constexpr uint32_t kPetMs = 720;
  static constexpr uint32_t kDecayMs = 20000;
  static constexpr int kLevels = 100;
  static constexpr int kAliens = 8;
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

  int barSlot(int x) const { return touch_bar_slot(x, kPanelWidth); }

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

  // Black rect clipped to the screen and kept off the bottom bar, so a sliver
  // from the cat or a shot cannot erase MENU or the score buttons.
  void wipe(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) {
      w += x;
      x = 0;
    }
    if (y < 0) {
      h += y;
      y = 0;
    }
    if (x >= kPanelWidth || y >= kPanelHeight - kBarH) return;
    if (x + w > kPanelWidth) w = kPanelWidth - x;
    if (y + h > kPanelHeight - kBarH) h = kPanelHeight - kBarH - y;
    if (w <= 0 || h <= 0) return;
    fillRect(x, y, w, h, TFT_BLACK);
  }

  // Erase the part of the old box that the new box does not cover. Call this
  // after the new picture is already on the panel, so the overlap never goes black.
  void uncover(int ox, int oy, int nx, int ny, int w, int h) {
    if (w <= 0 || h <= 0 || (ox == nx && oy == ny)) {
      return;
    }
    if (ox < nx) {
      wipe(ox, oy, nx - ox, h);
    } else if (nx < ox) {
      wipe(nx + w, oy, ox - nx, h);
    }
    if (oy < ny) {
      const int x0 = ox > nx ? ox : nx;
      const int x1 = (ox + w) < (nx + w) ? (ox + w) : (nx + w);
      if (x1 > x0) wipe(x0, oy, x1 - x0, ny - oy);
    } else if (ny < oy) {
      const int x0 = ox > nx ? ox : nx;
      const int x1 = (ox + w) < (nx + w) ? (ox + w) : (nx + w);
      if (x1 > x0) wipe(x0, ny + h, x1 - x0, oy - ny);
    }
  }

  void paintCat(int x, int y, CatFrame frame) {
    if (frame == kCatHappy) {
      if (happy_ok_) {
        happy_sprite_.pushSprite(x, y);
        return;
      }
      const CatFace& face = cat_frame(kCatHappy);
      drawPng(face.png, face.length, x, y);
      return;
    }
    if (sprite_ok_) {
      spriteFor(frame).pushSprite(x, y);
      return;
    }
    const CatFace& face = cat_frame(frame);
    drawPng(face.png, face.length, x, y);
  }

  void drawLabel(const char* text, int x, int y, int w, int h, uint16_t bg, uint16_t fg) {
    fillRect(x, y, w, h, bg);
    setTextSize(1);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(fg);
    drawString(text, x + w / 2, y + h / 2);
  }

  Presence parsePresence(const char* presence) const {
    if (presence != nullptr && strcmp(presence, "linked") == 0) return Presence::Linked;
    if (presence != nullptr && strcmp(presence, "leaving") == 0) return Presence::Leaving;
    if (presence != nullptr && strcmp(presence, "away") == 0) return Presence::Away;
    return Presence::Solo;
  }

  const char* moodWord() const {
    if (attention_ >= 75) return "happy";
    if (attention_ >= 40) return "ok";
    return "lonely";
  }

  int pipCount() const { return attention_ / 20; }

  uint16_t moodColor() const {
    if (attention_ >= 75) return color565(255, 196, 96);
    if (attention_ >= 40) return color565(176, 210, 160);
    return color565(214, 140, 120);
  }

  bool greeting() const {
    return greet_until_ != 0 && static_cast<int32_t>(millis() - greet_until_) < 0;
  }

  const char* linkWord() const {
    if (greeting()) return "back";
    if (presence_ == Presence::Linked) return "laptop";
    if (presence_ == Presence::Leaving) return "leaving";
    if (presence_ == Presence::Away) return "away";
    return "solo";
  }

  uint16_t linkColor() const {
    if (greeting()) return color565(255, 214, 120);
    if (presence_ == Presence::Linked) return color565(120, 200, 140);
    if (presence_ == Presence::Leaving) return color565(150, 176, 230);
    if (presence_ == Presence::Away) return color565(180, 170, 150);
    return color565(150, 142, 126);
  }

  void drawPips(int x, int y) {
    const int n = pipCount();
    const uint16_t on = moodColor();
    const uint16_t off = color565(48, 44, 40);
    for (int i = 0; i < 5; ++i) {
      fillRect(x + i * 12, y - 4, 8, 8, i < n ? on : off);
    }
  }

  void drawStatusStrip() {
    fillRect(0, 0, kPanelWidth, kStatusH, color565(16, 14, 12));
    setTextSize(1);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(moodColor());
    drawString(moodWord(), 8, kStatusH / 2);
    drawPips(56, kStatusH / 2);
    setTextDatum(lgfx::middle_right);
    setTextColor(linkColor());
    drawString(linkWord(), kPanelWidth - 8, kStatusH / 2);
  }

  void drawWanderBar(bool held) {
    bar_held_ = held;
    const int y = kPanelHeight - kBarH;
    fillRect(0, y, kPanelWidth, kBarH, TFT_BLACK);
    const uint16_t bg = held ? color565(82, 114, 176) : color565(28, 32, 40);
    fillRoundRect(6, y + 6, kPanelWidth - 12, kBarH - 12, 10, bg);
    setTextSize(1);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(moodColor());
    drawString(moodWord(), 18, y + 24);
    drawPips(18, y + 48);
    setTextDatum(lgfx::middle_right);
    setTextColor(TFT_WHITE);
    drawString("MENU", kPanelWidth - 18, y + 26);
    setTextColor(linkColor());
    drawString(linkWord(), kPanelWidth - 18, y + 48);
  }

  void drawSlots(const char* a, const char* b, const char* c, int held, int dead = -1) {
    const char* labels[3] = {a, b, c};
    const int y = kPanelHeight - kBarH;
    for (int i = 0; i < 3; ++i) {
      int x = 0;
      int w = 0;
      touch_bar_rect(i, kPanelWidth, x, w);
      const bool dim = i == dead;
      const uint16_t bg =
          dim ? color565(14, 16, 18) : (held == i) ? color565(82, 114, 176) : color565(28, 32, 40);
      const uint16_t fg = dim ? color565(110, 106, 98) : TFT_WHITE;
      drawLabel(labels[i], x, y, w, kBarH, bg, fg);
    }
    bar_slot_ = held;
  }

  void drawCard(const char* title, const char* sub, int y, uint16_t bg, bool on) {
    fillRoundRect(kCardX, y, kCardW, kCardH, 10, bg);
    if (on) drawRoundRect(kCardX + 2, y + 2, kCardW - 4, kCardH - 4, 8, TFT_WHITE);
    setTextSize(1);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString(title, kCardX + kCardW / 2, y + kCardH / 2 - 14);
    setTextColor(color565(214, 208, 196));
    drawString(sub, kCardX + kCardW / 2, y + kCardH / 2 + 16);
  }

  uint16_t cardColor(int index, bool on) const {
    if (index == 0) return on ? color565(78, 118, 188) : color565(36, 64, 112);
    return on ? color565(64, 124, 100) : color565(36, 72, 64);
  }

  void paintHome(int sel) {
    choice_sel_ = sel;
    drawCard("INVADERS", "100 levels, bosses", kCard1Y, cardColor(0, sel == 0), sel == 0);
    drawCard("CALENDAR", "date on this panel", kCard2Y, cardColor(1, sel == 1), sel == 1);
    drawSlots("INVADERS", "CALENDAR", "WALK", sel);
  }

  void showLeaveBanner() {
    banner_ = true;
    fillRect(0, 0, kPanelWidth, kBannerH, color565(32, 44, 72));
    setTextSize(1);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString("To the laptop", kPanelWidth / 2, kBannerH / 2);
  }

  void hideLeaveBanner() {
    if (!banner_) return;
    banner_ = false;
    fillRect(0, 0, kPanelWidth, kBannerH, TFT_BLACK);
    if (placed_ && cat_y_ < kBannerH + cat_h_) paintCat(cat_x_, cat_y_ - last_lift_, frame_);
  }

  void drawAway(bool pulseOnly) {
    if (!pulseOnly) {
      fillScreen(TFT_BLACK);
      setTextSize(1);
      setFont(&fonts::Font4);
      setTextDatum(lgfx::middle_center);
      setTextColor(TFT_WHITE);
      drawString("JITTERS", kPanelWidth / 2, 118);
      setFont(&fonts::Font2);
      setTextColor(color565(232, 214, 190));
      drawString("is on the laptop", kPanelWidth / 2, 164);
      setTextColor(color565(180, 170, 150));
      drawString("Use the laptop window", kPanelWidth / 2, 210);
      drawString("to send the cat back.", kPanelWidth / 2, 234);
    }
    const uint16_t fur = away_phase_ ? color565(206, 122, 52) : color565(132, 74, 34);
    fillRoundRect(128, 300, 64, 36, 8, fur);
    fillRect(146, 310, 8, 8, TFT_BLACK);
    fillRect(166, 310, 8, 8, TFT_BLACK);
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

  void restoreBrightness() {
    if (bright_until_ == 0) return;
    bright_until_ = 0;
    setBrightness(255);
  }

  void decayAttention(uint32_t now) {
    if (decay_at_ == 0) decay_at_ = now + kDecayMs;
    if (static_cast<int32_t>(now - decay_at_) < 0) return;
    decay_at_ = now + kDecayMs;
    if (attention_ <= 20) return;
    const int before = pipCount();
    const char* word = moodWord();
    attention_ -= 4;
    if (attention_ < 20) attention_ = 20;
    if (pipCount() == before && moodWord() == word) return;
    if (mode_ == Mode::Wander) drawWanderBar(bar_held_);
    else if (mode_ == Mode::Home || mode_ == Mode::Calendar) drawStatusStrip();
  }

  void startPet(uint32_t now) {
    petting_ = true;
    pet_started_ = now;
    pet_until_ = now + kPetMs;
    attention_ += 40;
    if (attention_ > 100) attention_ = 100;
    setBrightness(150);
    bright_until_ = now + 130;
    drawWanderBar(bar_held_);
  }

  void arm(Pending pending) {
    if (pending_ != Pending::None) return;
    pending_ = pending;
    pending_at_ = millis() + 130;
  }

  void firePending() {
    const Pending pending = pending_;
    pending_ = Pending::None;
    if (pending == Pending::Home) beginHome();
    else if (pending == Pending::Invaders || pending == Pending::Again) beginGame();
    else if (pending == Pending::Calendar) beginCalendar();
    else if (pending == Pending::Walk) beginWander();
  }

  int homeSelection(int tx, int ty) const {
    if (ty >= kCard1Y && ty < kCard1Y + kCardH && tx >= kCardX && tx < kCardX + kCardW) return 0;
    if (ty >= kCard2Y && ty < kCard2Y + kCardH && tx >= kCardX && tx < kCardX + kCardW) return 1;
    if (inBar(ty)) return barSlot(tx);
    return -1;
  }

  void tickChoice(bool down, bool rose, int tx, int ty) {
    if (pending_ != Pending::None) return;
    int sel = -1;
    if (down) {
      if (mode_ == Mode::Home) sel = homeSelection(tx, ty);
      else if (inBar(ty)) sel = barSlot(tx);
    }
    if (mode_ == Mode::Calendar && sel == 1) sel = -1;
    if (sel != choice_sel_) {
      choice_sel_ = sel;
      if (mode_ == Mode::Home) paintHome(sel);
      else if (mode_ == Mode::Calendar) drawSlots("HOME", "SOON", "WALK", sel, 1);
      else drawSlots("HOME", "AGAIN", "WALK", sel);
    }
    if (!(rose && sel >= 0)) return;
    if (mode_ == Mode::Home) {
      arm(sel == 0 ? Pending::Invaders : sel == 1 ? Pending::Calendar : Pending::Walk);
    } else if (mode_ == Mode::Calendar) {
      arm(sel == 0 ? Pending::Home : Pending::Walk);
    } else {
      arm(sel == 0 ? Pending::Home : sel == 1 ? Pending::Again : Pending::Walk);
    }
  }

  void drawGameBar(int held) { drawSlots("LEFT", "FIRE", "RIGHT", held); }

  void drawScore() {
    fillRect(0, 0, kPanelWidth, kStatusH, TFT_BLACK);
    setTextSize(1);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_left);
    setTextColor(TFT_WHITE);
    char left[24];
    char right[24];
    if (boss_) {
      snprintf(left, sizeof left, "BOSS %d", level_);
      snprintf(right, sizeof right, "HP %d  %d", boss_hp_, score_);
    } else {
      snprintf(left, sizeof left, "LV %d/%d", level_, kLevels);
      snprintf(right, sizeof right, "SCORE %d", score_);
    }
    drawString(left, 8, kStatusH / 2);
    setTextDatum(lgfx::middle_right);
    if (presence_ == Presence::Leaving) {
      setTextColor(linkColor());
      drawString("LEAVING", kPanelWidth - 8, kStatusH / 2);
      return;
    }
    drawString(right, kPanelWidth - 22, kStatusH / 2);
    fillRoundRect(kPanelWidth - 16, 8, 10, 10, 2, linkColor());
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
    petting_ = false;
    pending_ = Pending::None;
    banner_ = false;
    bar_held_ = false;
    last_lift_ = 0;
    frame_ = kCatIdle;
    cat_x_ = 40;
    cat_y_ = 80;
    vx_ = 1;
    vy_ = 1;
    walk_tick_ = 0;
    shown_at_ = millis();
    step_at_ = 0;
    greet_until_ = greet_pending_ ? millis() + 900 : 0;
    greet_pending_ = false;
    restoreBrightness();
    fillScreen(TFT_BLACK);
    drawWanderBar(false);
  }

  void beginHome() {
    mode_ = Mode::Home;
    shot_ = false;
    placed_ = false;
    petting_ = false;
    pending_ = Pending::None;
    banner_ = false;
    choice_sel_ = -1;
    restoreBrightness();
    fillScreen(TFT_BLACK);
    drawStatusStrip();
    setTextSize(1);
    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString("JITTERS", kPanelWidth / 2, 58);
    setFont(&fonts::Font2);
    setTextColor(color565(180, 170, 150));
    drawString("pick one", kPanelWidth / 2, 84);
    paintHome(-1);
    setFont(&fonts::Font2);
    setTextDatum(lgfx::middle_center);
    setTextColor(color565(150, 144, 132));
    drawString("Same choices on the bar", kPanelWidth / 2, 340);
  }

  void beginCalendar() {
    mode_ = Mode::Calendar;
    pending_ = Pending::None;
    choice_sel_ = -1;
    banner_ = false;
    fillScreen(TFT_BLACK);
    drawStatusStrip();
    setTextSize(1);
    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString("CALENDAR", kPanelWidth / 2, 78);
    setFont(&fonts::Font2);
    drawString(__DATE__, kPanelWidth / 2, 124);
    setTextColor(color565(220, 180, 90));
    drawString("calendar not connected", kPanelWidth / 2, 170);
    setTextColor(color565(180, 170, 150));
    drawString("No local calendar", kPanelWidth / 2, 214);
    drawString("is authorized yet.", kPanelWidth / 2, 238);
    drawSlots("HOME", "SOON", "WALK", -1, 1);
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
    pending_ = Pending::None;
    banner_ = false;
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
    pending_ = Pending::None;
    advance_at_ = millis() + 800;
    fillRect(16, 150, kPanelWidth - 32, 88, TFT_BLACK);
    setTextSize(1);
    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    char line[24];
    const int next = level_ + 1;
    if (next % 10 == 0) {
      snprintf(line, sizeof line, "BOSS %d", next);
    } else {
      snprintf(line, sizeof line, "LEVEL %d", next);
    }
    drawString(line, kPanelWidth / 2, 180);
    setFont(&fonts::Font2);
    setTextColor(color565(180, 170, 150));
    drawString("get ready", kPanelWidth / 2, 214);
  }

  void endRound(bool won) {
    mode_ = Mode::Over;
    shot_ = false;
    pending_ = Pending::None;
    choice_sel_ = -1;
    fillRect(12, 108, kPanelWidth - 24, 140, TFT_BLACK);
    setTextSize(1);
    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(TFT_WHITE);
    drawString(won ? "CLEARED" : "LANDED", kPanelWidth / 2, 146);
    setFont(&fonts::Font2);
    char line[32];
    snprintf(line, sizeof line, "SCORE %d", score_);
    drawString(line, kPanelWidth / 2, 186);
    setTextColor(color565(180, 170, 150));
    drawString(won ? "all 100 levels" : "they reached you", kPanelWidth / 2, 214);
    drawSlots("HOME", "AGAIN", "WALK", -1);
  }

  void stepWalk(uint32_t now) {
    int lift = 0;
    if (petting_) {
      if (static_cast<int32_t>(now - pet_until_) >= 0) {
        petting_ = false;
      } else {
        const uint32_t into = now - pet_started_;
        const bool up = into < 160 || (into >= 320 && into < 480);
        lift = up ? 5 : 0;
      }
    }
    if (lift > cat_y_) lift = cat_y_;
    const CatFrame next = petting_ ? kCatHappy : faceAt(now);
    int nx = cat_x_;
    int ny = cat_y_;
    if (!petting_) {
      const int max_x = kPanelWidth - cat_w_;
      const int max_y = kPanelHeight - kBarH - cat_h_;
      walk_tick_ += 1;
      const int dy = (walk_tick_ & 1) == 0 ? vy_ : 0;
      nx = cat_x_ + vx_;
      ny = cat_y_ + dy;
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
    }
    if (placed_ && nx == cat_x_ && ny == cat_y_ && next == frame_ && lift == last_lift_) {
      return;
    }
    const int py = ny - lift;
    const int old_py = cat_y_ - last_lift_;
    startWrite();
    paintCat(nx, py, next);
    if (placed_) uncover(cat_x_, old_py, nx, py, cat_w_, cat_h_);
    endWrite();
    cat_x_ = nx;
    cat_y_ = ny;
    frame_ = next;
    last_lift_ = lift;
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
    // Slot 0 is the left label. touch_to_screen already unmirrors raw X.
    if (slot == 0) {
      player_x_ -= 5;
    } else if (slot == 2) {
      player_x_ += 5;
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
      if (ny < kStatusH || hitAlien(shot_x_, ny)) {
        wipe(ox, oy, kShotW, kShotH);
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
  lgfx::LGFX_Sprite happy_sprite_;
  bool ready_ = false;
  bool visible_ = false;
  bool sprite_ok_ = false;
  bool happy_ok_ = false;
  bool placed_ = false;
  bool touch_down_ = false;
  bool petting_ = false;
  bool banner_ = false;
  bool bar_held_ = false;
  bool greet_pending_ = false;
  bool away_drawn_ = false;
  bool away_phase_ = false;
  bool shot_ = false;
  bool alive_[kAliens] = {};
  Mode mode_ = Mode::Wander;
  Presence presence_ = Presence::Solo;
  Pending pending_ = Pending::None;
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
  int choice_sel_ = -1;
  int attention_ = 72;
  int last_lift_ = 0;
  bool boss_ = false;
  uint32_t shown_at_ = 0;
  uint32_t step_at_ = 0;
  uint32_t advance_at_ = 0;
  uint32_t pet_started_ = 0;
  uint32_t pet_until_ = 0;
  uint32_t decay_at_ = 0;
  uint32_t greet_until_ = 0;
  uint32_t bright_until_ = 0;
  uint32_t away_at_ = 0;
  uint32_t pending_at_ = 0;
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
