#include <Arduino.h>
#include <SPI.h>

#include "aether.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "hal/gpio_ll.h"

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
constexpr uint32_t kSpiHz = 80000000;
constexpr int kBandRows = 16;
constexpr int kBacklightDuty = 820;  // 10-bit PWM. The backlight is active high.

constexpr uint8_t kGammaPositive[14] = {0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29,
                                         0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30};
constexpr uint8_t kGammaNegative[14] = {0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28,
                                         0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32};

AetherRgb g_band[kBandRows * kAetherWidth];
uint16_t g_pixels[kBandRows * kAetherWidth];

void lcd_write(uint8_t value, bool data) {
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(kPinCs, LOW);
  digitalWrite(kPinDc, data ? HIGH : LOW);
  SPI.transfer(value);
  digitalWrite(kPinCs, HIGH);
  SPI.endTransaction();
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
  ledcAttach(kPinBacklight, 5000, 10);
  ledcWrite(kPinBacklight, 0);

  SPI.begin(kPinSclk, kPinMiso, kPinMosi, -1);

  digitalWrite(kPinRst, HIGH);
  delay(10);
  digitalWrite(kPinRst, LOW);
  delay(20);
  digitalWrite(kPinRst, HIGH);
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
  static uint8_t discard[4000];
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(kPinCs, LOW);
  digitalWrite(kPinDc, HIGH);
  const uint8_t* bytes = (const uint8_t*)pixels;
  size_t remaining = count * 2;
  while (remaining) {
    size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
    SPI.transferBytes(bytes, discard, chunk);
    bytes += chunk;
    remaining -= chunk;
  }
  digitalWrite(kPinCs, HIGH);
  SPI.endTransaction();
}

uint16_t pack565(AetherRgb color) {
  uint16_t value =
      (uint16_t)(((color.r & 0xF8) << 8) | ((color.g & 0xFC) << 3) | (color.b >> 3));
  return (uint16_t)((value << 8) | (value >> 8));
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

void fade_backlight() {
  for (int duty = 0; duty <= kBacklightDuty; duty += 40) {
    ledcWrite(kPinBacklight, duty);
    delay(12);
  }
  ledcWrite(kPinBacklight, kBacklightDuty);
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
  Serial.begin(115200);
  pinMode(kPinRgb, OUTPUT);
  digitalWrite(kPinRgb, LOW);
  pinMode(kPinBoot, INPUT_PULLUP);
  lcd_init();
  push_frame(0);
  fade_backlight();
  lamp_show(40, 12, 4);
  Serial.println("aether ready");
}

void loop() {
  uint32_t now = millis();
  poll_boot(now);
  uint32_t time_ms = now + g_skip_ms;
  push_frame(time_ms);
  AetherLamp lamp = aether_lamp(time_ms);
  lamp_show(lamp.r, lamp.g, lamp.b);
}
