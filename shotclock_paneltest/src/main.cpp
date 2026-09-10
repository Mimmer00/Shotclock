/*
 * P10 32x16 1/4S Outdoor (ICN2037BP) - FINALER MAPPER (aus Foto-Dekodierung)
 *
 * Spalten: 8er-Bloecke, Pixel IM Block rueckwaerts.
 *   Zeilenpaar (y, y+4) teilt sich eine elektrische Zeile:
 *   obere Zeile -> gerade Bloecke (0,2,4,6), untere -> ungerade (1,3,5,7).
 * Zeilen:  ey = (ly>>3)*4 + (ly&3)   (Kanal1 = Zeilen 0..7, Kanal2 = 8..15)
 * Panels:  Block0 (ex 0..63) = OBERES Panel (180 Grad gedreht),
 *          Block1 (ex 64..127) = UNTERES Panel.
 *
 * Testbild: weisser Rahmen 32x32 + gruene Diagonale + rote "8".
 */
#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ---- Kalibrier-Schalter ------------------------------------------------------
#define EVEN_ON_UPPER 1   // 1: obere Paar-Zeile=gerade Bloecke | 0: umgekehrt
#define REV_IN_BLOCK  0   // Panel dreht selbst im Block um -> Mapper NICHT nochmal
#define TOP_ROT180    1   // oberes Panel 180 Grad gedreht
#define BOT_ROT180    0
#define TOP_BLOCK     0   // elektrischer Block des OBEREN Panels (0 oder 1)

#define PANEL_W 32
#define PANEL_H 16

#define R1_PIN 15
#define G1_PIN 16
#define B1_PIN 12
#define R2_PIN 48
#define G2_PIN 47
#define B2_PIN 35
#define A_PIN 36
#define B_PIN 38
#define C_PIN 42
#define D_PIN -1
#define E_PIN -1
#define CLK_PIN 39
#define LAT_PIN 40
#define OE_PIN 41

MatrixPanel_I2S_DMA *dma = nullptr;

// Ein Panel: logisch (lx 0..31, ly 0..15) -> elektrisch (ex 0..63, ey 0..7)
static inline void mapPanel(int lx, int ly, int &ex, int &ey) {
  int blk = lx / 8;                 // logischer 8er-Block 0..3
  int o   = lx % 8;
#if REV_IN_BLOCK
  o = 7 - o;
#endif
  int upper = ((ly & 4) == 0);      // obere Zeile des Paars?
#if EVEN_ON_UPPER
  int s = upper ? 0 : 1;
#else
  int s = upper ? 1 : 0;
#endif
  ex = (2 * blk + s) * 8 + o;
  ey = ((ly >> 3) & 1) * 4 + (ly & 3);
}

// logisch 32x32 -> DMA 128x8
static inline void px(int x, int y, uint16_t c) {
  if (x < 0 || x >= 32 || y < 0 || y >= 32) return;
  int block, lx, ly;
  if (y < PANEL_H) {
    block = TOP_BLOCK; lx = x; ly = y;
#if TOP_ROT180
    lx = PANEL_W - 1 - lx; ly = PANEL_H - 1 - ly;
#endif
  } else {
    block = 1 - TOP_BLOCK; lx = x; ly = y - PANEL_H;
#if BOT_ROT180
    lx = PANEL_W - 1 - lx; ly = PANEL_H - 1 - ly;
#endif
  }
  int ex, ey;
  mapPanel(lx, ly, ex, ey);
  dma->drawPixel(block * 64 + ex, ey, c);
}

static inline void fillrect(int x, int y, int w, int h, uint16_t c) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, c);
}

const uint8_t SEG[10] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
void digit(int d, int xOff, int yOff, int w, int h, int t, uint16_t col) {
  uint8_t s = SEG[d]; int midY = h/2 - 1;
  if (s&0x01) fillrect(xOff+t, yOff, w-2*t, t, col);
  if (s&0x40) fillrect(xOff+t, yOff+midY, w-2*t, t, col);
  if (s&0x08) fillrect(xOff+t, yOff+h-t, w-2*t, t, col);
  if (s&0x20) fillrect(xOff, yOff+t, t, midY-t, col);
  if (s&0x10) fillrect(xOff, yOff+midY+t, t, h-midY-2*t, col);
  if (s&0x02) fillrect(xOff+w-t, yOff+t, t, midY-t, col);
  if (s&0x04) fillrect(xOff+w-t, yOff+midY+t, t, h-midY-2*t, col);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.printf("\n=== FINAL-MAPPER evenUpper=%d rev=%d ===\n", EVEN_ON_UPPER, REV_IN_BLOCK);

  HUB75_I2S_CFG::i2s_pins pins = {R1_PIN,G1_PIN,B1_PIN,R2_PIN,G2_PIN,B2_PIN,
    A_PIN,B_PIN,C_PIN,D_PIN,E_PIN,LAT_PIN,OE_PIN,CLK_PIN};
  HUB75_I2S_CFG mxconfig(64, 8, 2, pins);
  mxconfig.clkphase = false;
  mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_8M;
  mxconfig.double_buff = false;

  dma = new MatrixPanel_I2S_DMA(mxconfig);
  if (!dma->begin()) { Serial.println("begin FAIL"); while(1) delay(1000); }
  dma->setBrightness8(120);
  dma->clearScreen();

  uint16_t W = dma->color565(255,255,255);
  uint16_t R = dma->color565(255,0,0);
  uint16_t G = dma->color565(0,255,0);

  for (int x = 0; x < 32; x++) { px(x, 0, W); px(x, 31, W); }   // Rahmen
  for (int y = 0; y < 32; y++) { px(0, y, W); px(31, y, W); }
  for (int i = 0; i < 32; i++) px(i, i, G);                      // Diagonale
  digit(8, 8, 1, 16, 30, 3, R);                                  // grosse "8"

  Serial.println("Rahmen + Diagonale + 8 gezeichnet.");
}

void loop() { delay(500); }
