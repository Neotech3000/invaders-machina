#include "board.h"

#include "cat_faces.h"
#include "hardware_config.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>

namespace {

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
    if (kPinSharedBusCsHoldHigh >= 0) {
      pinMode(kPinSharedBusCsHoldHigh, OUTPUT);
      digitalWrite(kPinSharedBusCsHoldHigh, HIGH);
    }
    ready_ = init();
    if (!ready_) {
      return false;
    }
    setRotation(kPanelRotation);
    setBrightness(255);
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
    const CatFace& face = cat_frame(kCatIdle);
    constexpr int kMaxEdge = 250;
    const float fit_w = static_cast<float>(kMaxEdge) / static_cast<float>(face.width);
    const float fit_h = static_cast<float>(kMaxEdge) / static_cast<float>(face.height);
    scale_ = fit_w < fit_h ? fit_w : fit_h;
    const int drawn_w = static_cast<int>(face.width * scale_);
    const int drawn_h = static_cast<int>(face.height * scale_);
    origin_x_ = (kPanelWidth - drawn_w) / 2;
    origin_y_ = (kPanelHeight - drawn_h) / 2;
    fillScreen(TFT_BLACK);
    paint(kCatIdle);
    shown_at_ = millis();
    visible_ = true;
  }

  void hide() override {
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
    const uint32_t elapsed = millis() - shown_at_;
    const uint32_t blink_pos = elapsed % kBlinkEveryMs;
    const uint32_t mouth_pos = elapsed % kMouthEveryMs;
    CatFrame next = kCatIdle;
    if (blink_pos >= kBlinkAtMs && blink_pos < kBlinkAtMs + kBlinkHoldMs) {
      next = kCatBlink;
    } else if (mouth_pos >= kMouthAtMs && mouth_pos < kMouthAtMs + kMouthHoldMs) {
      next = kCatMouth;
    }
    if (next == frame_) {
      return;
    }
    paint(next);
  }

 private:
  // Same rhythm as the desktop window. Idle first, then a short blink, then a small mouth.
  static constexpr uint32_t kBlinkEveryMs = 3400;
  static constexpr uint32_t kBlinkAtMs = 1600;
  static constexpr uint32_t kBlinkHoldMs = 140;
  static constexpr uint32_t kMouthEveryMs = 7600;
  static constexpr uint32_t kMouthAtMs = 2200;
  static constexpr uint32_t kMouthHoldMs = 380;

  void paint(CatFrame frame) {
    const CatFace& face = cat_frame(frame);
    drawPng(face.png, face.length, origin_x_, origin_y_, 0, 0, 0, 0, scale_, scale_);
    frame_ = frame;
  }

  lgfx::Bus_SPI bus_;
  lgfx::Panel_ST7796 panel_;
  lgfx::Light_PWM light_;
  bool ready_ = false;
  bool visible_ = false;
  CatFrame frame_ = kCatIdle;
  float scale_ = 1;
  int origin_x_ = 0;
  int origin_y_ = 0;
  uint32_t shown_at_ = 0;
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
