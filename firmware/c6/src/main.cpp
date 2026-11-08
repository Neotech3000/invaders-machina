#include <Arduino.h>

#include "aether.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "hal/gpio_ll.h"
#include "soc/io_mux_reg.h"
#include "soc/lp_aon_struct.h"

// ESP32-C6-LCD-1.47. Pinout and ST7789 sequence match Waveshare's Arduino
// demo for this board. GPIO12 and GPIO13 are the native USB pins and stay
// untouched, so the port used to program the board keeps working.

constexpr int kPinMosi = 6;
constexpr int kPinSclk = 7;
constexpr int kPinMiso = 5;
constexpr int kPinCs = 14;
constexpr int kPinDc = 15;
constexpr int kPinRst = 21;
constexpr int kPinBacklight = 22;
constexpr int kPinSdCs = 4;
constexpr int kPinRgb = 8;
constexpr int kPinBoot = 9;
constexpr int kColumnOffset = 34;
constexpr int kBandRows = 16;

constexpr uint8_t kGammaPositive[14] = {0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29,
                                         0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30};
constexpr uint8_t kGammaNegative[14] = {0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28,
                                         0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32};

AetherRgb g_band[kBandRows * kAetherWidth];
uint16_t g_pixels[kBandRows * kAetherWidth];

// The panel's clock and data pins are GPIO7 and GPIO6. On this chip those
// pads also sit on the default JTAG port, and the hardware SPI peripheral
// never clocks them. The RGB LED already proves direct GPIO writes work, so
// the display is driven the same way. Clock idles low. Data is sampled on
// the rising edge.
static inline void IRAM_ATTR hold_cycles(uint32_t cycles) {
  uint32_t start = esp_cpu_get_cycle_count();
  while ((esp_cpu_get_cycle_count() - start) < cycles) {
  }
}

static void IRAM_ATTR shift_byte(uint8_t value) {
  gpio_dev_t* hw = &GPIO;
  for (int bit = 0x80; bit != 0; bit >>= 1) {
    if (value & bit) {
      hw->out_w1ts.val = 1u << kPinMosi;
    } else {
      hw->out_w1tc.val = 1u << kPinMosi;
    }
    hold_cycles(8);
    hw->out_w1ts.val = 1u << kPinSclk;
    hold_cycles(12);
    hw->out_w1tc.val = 1u << kPinSclk;
  }
}

static void IRAM_ATTR shift_color(uint16_t color) {
  shift_byte((uint8_t)(color >> 8));
  shift_byte((uint8_t)color);
}

// GPIO6 and GPIO7 wake up as the JTAG clock and JTAG data pads, and they are
// also the chip's low-power pins. The LED is GPIO8, a normal digital pad, so
// it can change while these two stay on the function they booted with. The
// panel only sees a clock after the pad is handed to the GPIO register.
static void claim_output(int pin) {
  if (pin <= 7) {
    rtc_gpio_deinit((gpio_num_t)pin);
    LP_AON.gpio_mux.gpio_mux_sel &= ~(uint32_t)(1u << pin);
  }
  gpio_reset_pin((gpio_num_t)pin);
  pinMode(pin, OUTPUT);
  PIN_FUNC_SELECT(IO_MUX_GPIO0_REG + (pin * 4), PIN_FUNC_GPIO);
  gpio_ll_output_enable(&GPIO, (uint32_t)pin);
  gpio_ll_matrix_out_default(&GPIO, (uint32_t)pin);
}

void lcd_begin_bus() {
  claim_output(kPinMosi);
  claim_output(kPinSclk);
  claim_output(kPinCs);
  claim_output(kPinDc);
  claim_output(kPinRst);
  digitalWrite(kPinSclk, LOW);
  digitalWrite(kPinMosi, LOW);
  digitalWrite(kPinCs, HIGH);
  digitalWrite(kPinDc, HIGH);
}

void lcd_write(uint8_t value, bool data) {
  digitalWrite(kPinDc, data ? HIGH : LOW);
  digitalWrite(kPinCs, LOW);
  shift_byte(value);
  digitalWrite(kPinCs, HIGH);
}

void lcd_command(uint8_t command) { lcd_write(command, false); }

void lcd_data(uint8_t value) { lcd_write(value, true); }

void lcd_bytes(uint8_t command, const uint8_t* bytes, int count) {
  lcd_command(command);
  for (int i = 0; i < count; ++i) {
    lcd_data(bytes[i]);
  }
}

void lcd_init() {
  // Hold the microSD socket off the shared SPI bus.
  pinMode(kPinSdCs, OUTPUT);
  digitalWrite(kPinSdCs, HIGH);
  pinMode(kPinCs, OUTPUT);
  pinMode(kPinDc, OUTPUT);
  pinMode(kPinRst, OUTPUT);
  digitalWrite(kPinCs, HIGH);
  digitalWrite(kPinDc, HIGH);
  pinMode(kPinBacklight, OUTPUT);
  digitalWrite(kPinBacklight, HIGH);
  lcd_begin_bus();

  // Waveshare holds chip-select low across the reset pulse.
  digitalWrite(kPinCs, LOW);
  digitalWrite(kPinRst, HIGH);
  delay(10);
  digitalWrite(kPinRst, LOW);
  delay(20);
  digitalWrite(kPinRst, HIGH);
  digitalWrite(kPinCs, HIGH);
  delay(120);

  const uint8_t madctl[] = {0x00};
  const uint8_t colmod[] = {0x05};
  const uint8_t porch_b0[] = {0x00, 0xE8};
  const uint8_t porch_b2[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
  const uint8_t gate[] = {0x35};
  const uint8_t vcom[] = {0x35};
  const uint8_t lcm[] = {0x2C};
  const uint8_t vdv_en[] = {0x01};
  const uint8_t vrh[] = {0x13};
  const uint8_t vdv[] = {0x20};
  const uint8_t frame[] = {0x0F};
  const uint8_t power[] = {0xA4, 0xA1};
  const uint8_t cmd_d6[] = {0xA1};

  lcd_command(0x11);
  delay(120);
  lcd_bytes(0x36, madctl, 1);
  lcd_bytes(0x3A, colmod, 1);
  lcd_bytes(0xB0, porch_b0, 2);
  lcd_bytes(0xB2, porch_b2, 5);
  lcd_bytes(0xB7, gate, 1);
  lcd_bytes(0xBB, vcom, 1);
  lcd_bytes(0xC0, lcm, 1);
  lcd_bytes(0xC2, vdv_en, 1);
  lcd_bytes(0xC3, vrh, 1);
  lcd_bytes(0xC4, vdv, 1);
  lcd_bytes(0xC6, frame, 1);
  lcd_bytes(0xD0, power, 2);
  lcd_bytes(0xD6, cmd_d6, 1);
  lcd_bytes(0xE0, kGammaPositive, 14);
  lcd_bytes(0xE1, kGammaNegative, 14);
  lcd_command(0x21);
  lcd_command(0x11);
  delay(120);
  lcd_command(0x29);
}

void lcd_window(int x, int y, int x2, int y2) {
  x += kColumnOffset;
  x2 += kColumnOffset;
  uint8_t column[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x2 >> 8), (uint8_t)x2};
  uint8_t row[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y2 >> 8), (uint8_t)y2};
  lcd_bytes(0x2A, column, 4);
  lcd_bytes(0x2B, row, 4);
  lcd_command(0x2C);
}

void lcd_pixels(const uint16_t* pixels, size_t count) {
  digitalWrite(kPinDc, HIGH);
  digitalWrite(kPinCs, LOW);
  for (size_t i = 0; i < count; ++i) {
    shift_color(pixels[i]);
  }
  digitalWrite(kPinCs, HIGH);
}

void lcd_fill(uint16_t color) {
  lcd_window(0, 0, kAetherWidth - 1, kAetherHeight - 1);
  digitalWrite(kPinDc, HIGH);
  digitalWrite(kPinCs, LOW);
  size_t count = (size_t)kAetherWidth * (size_t)kAetherHeight;
  for (size_t i = 0; i < count; ++i) {
    shift_color(color);
  }
  digitalWrite(kPinCs, HIGH);
}

uint16_t pack565(AetherRgb color) {
  return (uint16_t)(((color.r & 0xF8) << 8) | ((color.g & 0xFC) << 3) | (color.b >> 3));
}

void push_frame(uint32_t time_ms) {
  for (int y = 0; y < kAetherHeight; y += kBandRows) {
    int rows = kAetherHeight - y;
    if (rows > kBandRows) {
      rows = kBandRows;
    }
    aether_render(time_ms, y, rows, g_band);
    int count = rows * kAetherWidth;
    for (int i = 0; i < count; ++i) {
      g_pixels[i] = pack565(g_band[i]);
    }
    lcd_window(0, y, kAetherWidth - 1, y + rows - 1);
    lcd_pixels(g_pixels, (size_t)count);
  }
}

// WS2812 on GPIO8. The bit loop stays in IRAM so a cache miss cannot stretch
// a pulse. The waits are cycle counts at 160 MHz.
static void IRAM_ATTR send_byte(uint8_t value) {
  gpio_dev_t* hw = &GPIO;
  for (int bit = 7; bit >= 0; --bit) {
    bool one = (value & (1 << bit)) != 0;
    uint32_t high_cycles = one ? 110 : 46;
    uint32_t low_cycles = one ? 50 : 108;
    gpio_ll_set_level(hw, kPinRgb, 1);
    uint32_t start = esp_cpu_get_cycle_count();
    while ((esp_cpu_get_cycle_count() - start) < high_cycles) {
    }
    gpio_ll_set_level(hw, kPinRgb, 0);
    start = esp_cpu_get_cycle_count();
    while ((esp_cpu_get_cycle_count() - start) < low_cycles) {
    }
  }
}

void lamp_show(uint8_t red, uint8_t green, uint8_t blue) {
  noInterrupts();
  send_byte(green);
  send_byte(red);
  send_byte(blue);
  interrupts();
  delayMicroseconds(280);
}

uint32_t g_skip_ms = 0;
bool g_boot_armed = true;
uint32_t g_boot_down_at = 0;

void poll_boot(uint32_t now) {
  bool down = digitalRead(kPinBoot) == LOW;
  if (!down) {
    g_boot_armed = true;
    g_boot_down_at = 0;
    return;
  }
  if (g_boot_down_at == 0) {
    g_boot_down_at = now;
  }
  if (g_boot_armed && (uint32_t)(now - g_boot_down_at) > 40) {
    uint32_t shown = now + g_skip_ms;
    uint32_t local = shown % kAetherSceneMs;
    g_skip_ms += kAetherSceneMs - local;
    g_boot_armed = false;
  }
}

void setup() {
  // Blink the backlight before anything else. The panel keeps its last
  // picture across a reboot, so this is the visible sign the new sketch runs.
  pinMode(kPinBacklight, OUTPUT);
  digitalWrite(kPinBacklight, LOW);
  delay(250);
  digitalWrite(kPinBacklight, HIGH);

  Serial.begin(115200);
  pinMode(kPinRgb, OUTPUT);
  digitalWrite(kPinRgb, LOW);
  pinMode(kPinBoot, INPUT_PULLUP);
  lcd_init();
  // Solid primaries first. The panel keeps its last picture until a write
  // actually lands, so three full-screen colors show whether the LCD moved.
  const AetherRgb proof[] = {
      {255, 0, 0},
      {0, 255, 0},
      {0, 0, 255},
  };
  for (int i = 0; i < 3; ++i) {
    lcd_fill(pack565(proof[i]));
    delay(400);
  }
  push_frame(0);
  lamp_show(40, 12, 4);
  Serial.printf("aether ready  mosi_fn=%lu sclk_fn=%lu lp_mux=0x%02lx\n",
                (unsigned long)REG_GET_FIELD(IO_MUX_GPIO6_REG, MCU_SEL),
                (unsigned long)REG_GET_FIELD(IO_MUX_GPIO7_REG, MCU_SEL),
                (unsigned long)(LP_AON.gpio_mux.gpio_mux_sel & 0xFFu));
}

void loop() {
  uint32_t now = millis();
  poll_boot(now);
  uint32_t time_ms = now + g_skip_ms;
  push_frame(time_ms);
  AetherLamp lamp = aether_lamp(time_ms);
  lamp_show(lamp.r, lamp.g, lamp.b);
}
