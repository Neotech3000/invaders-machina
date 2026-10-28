#include "board.h"

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
    const char* word = (expression != nullptr && expression[0] != '\0')
                           ? expression
                           : "idle";
    constexpr int margin_x = 24;
    constexpr int margin_y = 48;
    const int x = margin_x;
    const int y = margin_y;
    const int w = kPanelWidth - margin_x * 2;
    const int h = kPanelHeight - margin_y * 2;
    const uint16_t fill = color565(0xC4, 0x7A, 0x3A);
    const uint16_t ink = color565(0x1A, 0x1A, 0x1A);
    fillScreen(TFT_BLACK);
    fillRect(x, y, w, h, fill);
    setFont(&fonts::Font4);
    setTextDatum(lgfx::middle_center);
    setTextColor(ink);
    drawString(word, x + w / 2, y + h / 2);
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
