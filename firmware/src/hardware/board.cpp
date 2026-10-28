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
    if (!ready_) {
      visible_ = false;
      return;
    }
    const CatFace& face = cat_face_for(expression);
    fillScreen(TFT_BLACK);
    constexpr int kMaxEdge = 250;
    const float fit_w = static_cast<float>(kMaxEdge) / static_cast<float>(face.width);
    const float fit_h = static_cast<float>(kMaxEdge) / static_cast<float>(face.height);
    const float scale = fit_w < fit_h ? fit_w : fit_h;
    const int drawn_w = static_cast<int>(face.width * scale);
    const int drawn_h = static_cast<int>(face.height * scale);
    const int x = (kPanelWidth - drawn_w) / 2;
    const int y = (kPanelHeight - drawn_h) / 2;
    drawPng(face.png, face.length, x, y, 0, 0, 0, 0, scale, scale);
    visible_ = true;
  }

  void hide() override {
    if (ready_) {
      fillScreen(TFT_BLACK);
    }
    visible_ = false;
  }

  bool isVisible() const override { return visible_; }

 private:
  lgfx::Bus_SPI bus_;
  lgfx::Panel_ST7796 panel_;
  lgfx::Light_PWM light_;
  bool ready_ = false;
  bool visible_ = false;
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
