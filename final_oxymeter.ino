#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Wire.h>
#include "MAX30102.h"
#include "Pulse.h"
#include <avr/pgmspace.h>
#include <avr/sleep.h>

#ifndef cbi
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#endif
#ifndef sbi
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
#endif

#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

#define TFT_BG    ST77XX_BLACK
#define TFT_FG    ST77XX_WHITE
#define TFT_RED   ST77XX_RED
#define TFT_GREEN ST77XX_GREEN
#define TFT_CYAN  ST77XX_CYAN

MAX30102 sensor;
Pulse pulseIR;
Pulse pulseRed;
MAFilter bpm;

#define LED LED_BUILTIN

const uint8_t spo2_table[184] PROGMEM =
{ 95, 95, 95, 96, 96, 96, 97, 97, 97, 97, 97, 98, 98, 98, 98, 98, 99, 99, 99, 99,
  99, 99, 99, 99,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,
 100,100,100,100, 99, 99, 99, 99, 99, 99, 99, 99, 98, 98, 98, 98, 98, 98, 97, 97,
  97, 97, 96, 96, 96, 96, 95, 95, 95, 94, 94, 94, 93, 93, 93, 92, 92, 92, 91, 91,
  90, 90, 89, 89, 89, 88, 88, 87, 87, 86, 86, 85, 85, 84, 84, 83, 82, 82, 81, 81,
  80, 80, 79, 78, 78, 77, 76, 76, 75, 74, 74, 73, 72, 72, 71, 70, 69, 69, 68, 67,
  66, 66, 65, 64, 63, 62, 62, 61, 60, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50,
  49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 31, 30, 29,
  28, 27, 26, 25, 23, 22, 21, 20, 19, 17, 16, 15, 14, 12, 11, 10,  9,  7,  6,  5,
   3,  2,  1 };

#define MAXWAVE 128
#define WAVE_Y_OFFSET 80
#define WAVE_HEIGHT   32

class Waveform {
public:
  Waveform() { wavep = 0; }

  void record(int waveval) {
    waveval  = waveval / 8;
    waveval += 128;
    waveval  = waveval < 0 ? 0 : waveval;
    waveform[wavep] = (uint8_t)(waveval > 255 ? 255 : waveval);
    wavep = (wavep + 1) % MAXWAVE;
  }

  void scale() {
    uint8_t maxw = 0, minw = 255;
    for (int i = 0; i < MAXWAVE; i++) {
      if (waveform[i] > maxw) maxw = waveform[i];
      if (waveform[i] < minw) minw = waveform[i];
    }
    uint8_t scale8 = (maxw - minw) / 4 + 1;
    uint8_t index  = wavep;
    for (int i = 0; i < MAXWAVE; i++) {
      disp_wave[i] = (WAVE_HEIGHT - 1) - ((uint16_t)(waveform[index] - minw) * (WAVE_HEIGHT - 1)) / (scale8 * 4);
      index = (index + 1) % MAXWAVE;
    }
  }

  void draw() {
    tft.fillRect(0, WAVE_Y_OFFSET, MAXWAVE, WAVE_HEIGHT, TFT_BG);
    for (int i = 0; i < MAXWAVE; i++) {
      uint8_t y = disp_wave[i] + WAVE_Y_OFFSET;
      tft.drawPixel(i, y, TFT_GREEN);
      if (i < MAXWAVE - 1) {
        uint8_t nexty = disp_wave[i + 1] + WAVE_Y_OFFSET;
        if (nexty > y + 1)
          tft.drawFastVLine(i, y + 1, nexty - y - 1, TFT_GREEN);
        else if (y > nexty + 1)
          tft.drawFastVLine(i, nexty + 1, y - nexty - 1, TFT_GREEN);
      }
    }
  }

private:
  uint8_t waveform[MAXWAVE];
  uint8_t disp_wave[MAXWAVE];
  uint8_t wavep = 0;
} wave;

int     beatAvg       = 0;
int     SPO2          = 0;
int     SPO2f         = 0;
uint8_t sleep_counter = 0;
long    lastBeat      = 0;
long    displaytime   = 0;
bool    led_on        = false;
int     lastBeatAvg   = -1;
int     lastSPO2f     = -1;

void go_sleep() {
  tft.fillScreen(TFT_BG);
  delay(10);
  sensor.off();
  delay(10);
  cbi(ADCSRA, ADEN);
  delay(10);
  pinMode(0, INPUT);
  pinMode(2, INPUT);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_mode();
  setup();
}

void print_digit(int x, int y, long val, char c = ' ',
                 uint8_t field = 3, uint8_t size = 3,
                 uint16_t color = TFT_FG) {
  uint8_t charW = 6 * size;
  uint8_t ff    = field;
  long    v     = val;
  do {
    char ch = (v != 0 || ff == field) ? (v % 10) + '0' : c;
    int cx = x + charW * (ff - 1);
    tft.fillRect(cx, y, charW, 8 * size, TFT_BG);
    tft.setTextColor(color);
    tft.setTextSize(size);
    tft.setCursor(cx, y);
    tft.print(ch);
    v = v / 10;
    --ff;
  } while (ff > 0);
}

void draw_tft(int msg) {
  switch (msg) {

    case 0:
      tft.fillScreen(TFT_BG);
      tft.setTextColor(TFT_RED);
      tft.setTextSize(2);
      tft.setCursor(4, 60);
      tft.print(F("Device error"));
      break;

    case 1:
      tft.fillScreen(TFT_BG);
      tft.setTextColor(TFT_FG);
      tft.setTextSize(2);
      tft.setCursor(4, 40);
      tft.print(F("PLACE"));
      tft.setCursor(4, 62);
      tft.print(F("YOUR"));
      tft.setCursor(4, 84);
      tft.print(F("FINGER"));
      break;

    case 2:
      // ── BPM section (top 38px) ──
      tft.fillRect(0, 0, 128, 38, TFT_BG);
      tft.setTextColor(TFT_CYAN);
      tft.setTextSize(1);
      tft.setCursor(4, 4);
      tft.print(F("PULSE RATE (bpm)"));
      print_digit(74, 12, beatAvg, ' ', 3, 2, TFT_GREEN);

      // ── SpO2 section (middle 38px) ──
      tft.fillRect(0, 38, 128, 38, TFT_BG);
      tft.setTextColor(TFT_CYAN);
      tft.setTextSize(1);
      tft.setCursor(4, 42);
      tft.print(F("OXYGEN SAT (%)"));
      print_digit(74, 50, SPO2f, ' ', 3, 2, TFT_RED);
      break;

    case 3:
      tft.fillScreen(TFT_BG);
      tft.setTextColor(TFT_FG);
      tft.setTextSize(2);
      tft.setCursor(4, 50);
      tft.print(F("Pulse"));
      tft.setCursor(4, 74);
      tft.print(F("Oximeter"));
      break;

    case 4:
      tft.fillRect(0, 0, 128, 160, TFT_BG);
      tft.setTextColor(TFT_FG);
      tft.setTextSize(1);
      tft.setCursor(4, 76);
      tft.print(F("OFF IN "));
      tft.print((char)(10 - sleep_counter / 10 + '0'));
      tft.print(F("s"));
      break;
  }
}

void setup() {
  pinMode(LED, OUTPUT);
  Wire.begin();

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);
  tft.fillScreen(TFT_BG);

  draw_tft(3);
  delay(3000);

  if (!sensor.begin()) {
    draw_tft(0);
    while (1);
  }
  sensor.setup();
}

void loop() {
  sensor.check();
  long now = millis();
  if (!sensor.available()) return;

  uint32_t irValue  = sensor.getIR();
  uint32_t redValue = sensor.getRed();
  sensor.nextSample();

  if (irValue < 5000) {
    draw_tft(sleep_counter <= 50 ? 1 : 4);
    delay(200);
    ++sleep_counter;
    if (sleep_counter > 100) {
      go_sleep();
      sleep_counter = 0;
    }
  } else {
    sleep_counter = 0;

    int16_t IR_signal  = pulseIR.dc_filter(irValue);
    int16_t Red_signal = pulseRed.dc_filter(redValue);
    bool    beatIR     = pulseIR.isBeat(pulseIR.ma_filter(IR_signal));
    bool    beatRed    = pulseRed.isBeat(pulseRed.ma_filter(Red_signal));

    wave.record(-IR_signal);

    if (beatIR) {
      long btpm = 60000L / (now - lastBeat);
      if (btpm > 0 && btpm < 200) beatAvg = bpm.filter((int16_t)btpm);
      lastBeat = now;
      digitalWrite(LED, HIGH);
      led_on = true;

      long numerator   = (pulseRed.avgAC() * pulseIR.avgDC()) / 256;
      long denominator = (pulseRed.avgDC() * pulseIR.avgAC()) / 256;
      int  RX100       = (denominator > 0) ? (numerator * 100) / denominator : 999;
      SPO2f = (10400 - RX100 * 17 + 50) / 100;
      if (RX100 >= 0 && RX100 < 184)
        SPO2 = pgm_read_byte_near(&spo2_table[RX100]);
    }

    if (now - displaytime > 100) {
      displaytime = now;
      wave.scale();
      if (beatAvg != lastBeatAvg || SPO2f != lastSPO2f) {
        lastBeatAvg = beatAvg;
        lastSPO2f   = SPO2f;
        draw_tft(2);
      }
      wave.draw();
    }
  }

  if (led_on && (now - lastBeat) > 25) {
    digitalWrite(LED, LOW);
    led_on = false;
  }
}