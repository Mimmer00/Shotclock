/*
 * Shotclock T3S3 Empfaenger
 * Empfaengt LoRa-Pakete und fuehrt einen eigenen lokalen Countdown.
 *
 * Protokoll (empfangen):
 *   CMD:START:28  -> Starte Timer ab 28 Sekunden
 *   CMD:STOP:15   -> Stoppe Timer bei 15 Sekunden
 *   CMD:RESET:30  -> Setze Timer auf 30 Sekunden (gestoppt)
 *   CMD:SYNC:22   -> Korrigiere nur wenn lokale Abweichung > 1s
 */
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// --- Pins T3S3 ---
#define PIN_SCK    5
#define PIN_MISO   3
#define PIN_MOSI   6
#define PIN_CS     7
#define PIN_RST    8
#define PIN_DIO1   33
#define PIN_BUSY   34
#define OLED_SDA   18
#define OLED_SCL   17
#define PIN_BUZZER 2
#define PIN_LED    37

// --- HUB75 P10-Display (2x 32x16 gestapelt = 32x32) ---
#define HUB_R1  15
#define HUB_G1  16
#define HUB_B1  12
#define HUB_R2  48
#define HUB_G2  47
#define HUB_B2  35
#define HUB_A   36
#define HUB_B   38
#define HUB_C   42   // ungenutzt bei 1/4-Scan
#define HUB_CLK 39
#define HUB_LAT 40
#define HUB_OE  41

#define P10_BRIGHTNESS 150   // 0..255 (eigenes 5V-Netzteil vorausgesetzt)

// --- LoRa-Parameter (identisch mit Sender!) ---
#define LORA_FREQ      868.125f
#define LORA_BW        125.0f
#define LORA_SF        8
#define LORA_CR        5
#define LORA_SYNC      0x12
#define LORA_POWER     14
#define LORA_PREAMBLE  12

// IRQ-Maske: NUR RX_DONE und CRC_ERR auf DIO1 - kein PREAMBLE_DETECTED!
// PREAMBLE wuerde DIO1 HIGH halten und den naechsten RX_DONE blockieren.
#define IRQ_FLAGS (RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR)

// --- Zustaende des lokalen Timers ---
enum ClockState : uint8_t { IDLE = 0, RUNNING = 1, STOPPED = 2 };

SPIClass spi(FSPI);
SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
MatrixPanel_I2S_DMA *p10 = nullptr;

// ============================================================
//  P10-Mapping (verifiziert, siehe shotclock_paneltest)
//  Panel: 8px-Block-Interleave, Zeilenpaar (y,y+4):
//  obere Zeile = gerade Bloecke, untere = ungerade.
//  Oberes Panel = DMA-Block 0, 180 Grad gedreht; unteres = Block 1.
// ============================================================
static inline void p10Pixel(int x, int y, uint16_t c) {
    if (x < 0 || x >= 32 || y < 0 || y >= 32) return;
    int block, lx, ly;
    if (y < 16) {                       // oberes Panel: 180 Grad gedreht
        block = 0;
        lx = 31 - x;
        ly = 15 - y;
    } else {                            // unteres Panel: normal
        block = 1;
        lx = x;
        ly = y - 16;
    }
    int blk = lx / 8, o = lx % 8;
    int s   = ((ly & 4) == 0) ? 0 : 1;
    int ex  = (2 * blk + s) * 8 + o;
    int ey  = ((ly >> 3) & 1) * 4 + (ly & 3);
    p10->drawPixel(block * 64 + ex, ey, c);
}

static inline void p10Rect(int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            p10Pixel(x + i, y + j, c);
}

// 7-Segment: Bit 0=oben 1=re-oben 2=re-unten 3=unten 4=li-unten 5=li-oben 6=mitte
const uint8_t P10_SEGS[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};
#define P10_SEG_MINUS 0x40   // nur Mittelsegment ("-")
#define P10_SEG_E     0x79   // Buchstabe E (Fehler)

// Zeichnet ein 7-Segment-Zeichen (14 breit, 28 hoch, Strichstaerke 3)
void p10Cell(uint8_t s, int xOff, int yOff, uint16_t col) {
    const int w = 14, h = 28, t = 3, midY = h / 2 - 1;
    if (s & 0x01) p10Rect(xOff + t,     yOff,             w - 2*t, t, col);
    if (s & 0x40) p10Rect(xOff + t,     yOff + midY,      w - 2*t, t, col);
    if (s & 0x08) p10Rect(xOff + t,     yOff + h - t,     w - 2*t, t, col);
    if (s & 0x20) p10Rect(xOff,         yOff + t,         t, midY - t, col);
    if (s & 0x10) p10Rect(xOff,         yOff + midY + t,  t, h - midY - 2*t, col);
    if (s & 0x02) p10Rect(xOff + w - t, yOff + t,         t, midY - t, col);
    if (s & 0x04) p10Rect(xOff + w - t, yOff + midY + t,  t, h - midY - 2*t, col);
}

// Grosse Anzeige: 2 Zeichen zentriert (x=1 und x=17, y=2)
void drawP10(int sec, bool hasSignal, bool error = false) {
    p10->clearScreen();
    uint16_t red = p10->color565(255, 0, 0);
    if (error) {
        p10Cell(P10_SEG_E, 1, 2, red);
        p10Cell(P10_SEG_E, 17, 2, red);
    } else if (!hasSignal) {
        p10Cell(P10_SEG_MINUS, 1, 2, red);      // "--" = kein Signal
        p10Cell(P10_SEG_MINUS, 17, 2, red);
    } else {
        if (sec < 0) sec = 0;
        if (sec > 99) sec = 99;
        p10Cell(P10_SEGS[(sec / 10) % 10], 1, 2, red);
        p10Cell(P10_SEGS[sec % 10],       17, 2, red);
    }
}

// ISR-Flag fuer LoRa-Empfang
volatile bool rxFlag = false;
void IRAM_ATTR onReceive() { rxFlag = true; }

// Lokaler Timer-Zustand
ClockState    clockState   = IDLE;
unsigned long clockStartMs  = 0;     // millis() bei START
int           startValue   = 30;    // Sekundenwert bei START
int           displaySec   = 30;    // Aktuell anzuzeigender Wert
int           lastShown    = -1;    // Letzter Anzeigewert (verhindert unnoetige Updates)
bool          lastSignal   = false; // Letzter Signal-Status (erzwingt Redraw bei Wechsel)
float         lastRssi     = 0.0f;
unsigned long lastRxMs     = 0;     // Zeitstempel des letzten Paketempfangs

// Buzzer-Steuerung
bool buzzedZero  = false;
int  lastWarnSec = 99;

// Kurzer Buzzer
void beep(int ms) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(ms);
    digitalWrite(PIN_BUZZER, LOW);
}

// Berechnet die aktuellen lokalen Sekunden aus millis()
int getLocalSec() {
    if (clockState != RUNNING) return displaySec;
    int elapsed = (int)((millis() - clockStartMs) / 1000UL);
    return max(0, startValue - elapsed);
}

// OLED-Anzeige
void drawOLED(int sec, ClockState state) {
    bool hasSignal = (lastRxMs > 0);

    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(2, 9, "SHOTCLOCK");
    display.drawHLine(0, 11, 128);

    if (!hasSignal) {
        // Noch kein Signal empfangen
        display.setFont(u8g2_font_9x18B_tf);
        display.drawStr(18, 42, "NO SIGNAL");
    } else {
        // Grosse Sekundenanzeige
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", sec);
        display.setFont(u8g2_font_inb33_mn);
        int w = display.getStrWidth(buf);
        display.drawStr((128 - w) / 2, 56, buf);
    }

    // Statuszeile: Zustand links, RSSI rechts
    display.setFont(u8g2_font_6x10_tf);
    const char* stateStr;
    switch (state) {
        case RUNNING: stateStr = "RUNNING"; break;
        case STOPPED: stateStr = "STOP";    break;
        default:      stateStr = "IDLE";    break;
    }
    display.drawStr(2, 63, hasSignal ? stateStr : "---");

    if (hasSignal) {
        char rssiStr[12];
        snprintf(rssiStr, sizeof(rssiStr), "%.0fdBm", lastRssi);
        int rw = display.getStrWidth(rssiStr);
        display.drawStr(126 - rw, 63, rssiStr);
    }

    display.sendBuffer();
}

// Paket parsen: "CMD:START:28" -> cmd="START", sec=28
// Gibt true zurueck wenn Paket gueltig
bool parsePacket(const char* raw, char* cmd, int* sec) {
    if (strncmp(raw, "CMD:", 4) != 0) return false;
    const char* colon = strchr(raw + 4, ':');
    if (!colon) return false;
    int cmdLen = (int)(colon - (raw + 4));
    if (cmdLen <= 0 || cmdLen > 8) return false;
    strncpy(cmd, raw + 4, cmdLen);
    cmd[cmdLen] = '\0';
    *sec = atoi(colon + 1);
    return true;
}

// Empfangenes Paket verarbeiten
void handlePacket(const char* cmd, int sec) {
    lastRxMs = millis();

    if (strcmp(cmd, "START") == 0) {
        // Redundanz-Duplikat ignorieren: gleiche Sekundenzahl kurz nach dem
        // ersten START wuerde sonst die Tick-Phase verschieben (Panel-Delay!)
        if (clockState == RUNNING && sec == startValue &&
            (millis() - clockStartMs) < 1500UL) {
            return;
        }
        // Lokalen Timer ab empfangenem Sekundenwert starten
        startValue  = sec;
        clockStartMs  = millis();
        clockState  = RUNNING;
        displaySec  = sec;
        buzzedZero  = false;
        lastWarnSec = sec + 1; // Reset Warnlogik

    } else if (strcmp(cmd, "STOP") == 0) {
        // Timer stoppen, Sekundenwert vom Sender uebernehmen
        displaySec = sec;
        clockState = STOPPED;

    } else if (strcmp(cmd, "RESET") == 0) {
        // Auf Ausgangswert zuruecksetzen
        displaySec  = sec;
        startValue  = sec;
        clockState  = STOPPED;
        buzzedZero  = false;
        lastWarnSec = 99;

    } else if (strcmp(cmd, "SYNC") == 0) {
        // Nur korrigieren wenn Abweichung > 1 Sekunde
        if (clockState == RUNNING) {
            int localSec = getLocalSec();
            int diff = abs(localSec - sec);
            if (diff > 1) {
                // timerStart anpassen sodass getLocalSec() == sec ergibt
                clockStartMs = millis() - (unsigned long)(startValue - sec) * 1000UL;
                Serial.printf("[SYNC] Korrigiert: lokal=%d empfangen=%d\n", localSec, sec);
            }
        }
        return; // Kein Serial-Log fuer normales SYNC
    }

    Serial.printf("[RX] CMD:%s:%d rssi=%.1f\n", cmd, sec, lastRssi);
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    digitalWrite(PIN_LED, LOW);

    // P10-Display initialisieren (DMA elektrisch 64x8, Kette=2)
    {
        HUB75_I2S_CFG::i2s_pins pins = {
            HUB_R1, HUB_G1, HUB_B1, HUB_R2, HUB_G2, HUB_B2,
            HUB_A, HUB_B, HUB_C, -1, -1, HUB_LAT, HUB_OE, HUB_CLK
        };
        HUB75_I2S_CFG mxconfig(64, 8, 2, pins);
        mxconfig.clkphase    = false;
        mxconfig.i2sspeed    = HUB75_I2S_CFG::HZ_8M;
        mxconfig.double_buff = false;   // Pflicht: kein flipDMABuffer im Code
        p10 = new MatrixPanel_I2S_DMA(mxconfig);
        if (!p10->begin()) {
            Serial.println("[P10] FEHLER: DMA-Init fehlgeschlagen!");
        }
        p10->setBrightness8(P10_BRIGHTNESS);
        p10->clearScreen();
        drawP10(0, false);              // "--" bis erstes Paket
    }

    // OLED initialisieren
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin();
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(10, 32, "Init LoRa...");
    display.sendBuffer();

    // LoRa initialisieren
    spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                         LORA_SYNC, LORA_POWER, LORA_PREAMBLE, 1.8f);
    if (st != RADIOLIB_ERR_NONE) {
        display.clearBuffer();
        display.setFont(u8g2_font_6x10_tf);
        char errBuf[28];
        snprintf(errBuf, sizeof(errBuf), "LoRa Fehler: %d", st);
        display.drawStr(2, 32, errBuf);
        display.sendBuffer();
        drawP10(0, false, true);        // "EE" auf dem Panel
        while (true) { beep(300); delay(700); }
    }

    // RF-Switch und ISR konfigurieren
    radio.setDio2AsRfSwitch();
    radio.setDio1Action(onReceive);

    // Empfang starten: NUR RX_DONE und CRC_ERR auf DIO1
    radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, IRQ_FLAGS, IRQ_FLAGS);

    Serial.println("[RX] Bereit. Warte auf LoRa-Pakete...");
    drawOLED(30, IDLE);
    beep(80); delay(100); beep(80); // Bereit-Signal
}

void loop() {
    // --- LoRa-Empfang verarbeiten ---
    if (rxFlag) {
        rxFlag = false;
        uint16_t irq = radio.getIrqStatus();

        if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
            uint8_t buf[64] = {0};
            int len = radio.getPacketLength();
            int err = radio.readData(buf, min(len, (int)sizeof(buf) - 1));
            buf[sizeof(buf) - 1] = '\0';

            if (err == RADIOLIB_ERR_NONE && len > 0) {
                lastRssi = radio.getRSSI();
                float snr = radio.getSNR();

                char cmd[16] = {0};
                int  sec = 0;
                if (parsePacket((char*)buf, cmd, &sec)) {
                    // Quittung: LED kurz aufleuchten
                    digitalWrite(PIN_LED, HIGH);
                    handlePacket(cmd, sec);
                    Serial.printf("[RX] CMD:%s:%d rssi=%.1f snr=%.1f\n",
                                  cmd, sec, lastRssi, snr);
                    delay(20);
                    digitalWrite(PIN_LED, LOW);
                } else {
                    Serial.printf("[RX_UNKNOWN] '%s'\n", (char*)buf);
                }
            }
        } else if (irq & RADIOLIB_SX126X_IRQ_CRC_ERR) {
            Serial.println("[CRC_ERR] Paket verworfen");
        }

        // Empfang neu starten
        radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, IRQ_FLAGS, IRQ_FLAGS);
    }

    // --- Lokalen Timer aktualisieren ---
    int sec = getLocalSec();

    // Timer bei 0 Sekunden automatisch stoppen
    if (clockState == RUNNING && sec == 0) {
        clockState = STOPPED;
        displaySec = 0;
        if (!buzzedZero) {
            buzzedZero = true;
            beep(300); // Langer Buzzer bei Ablauf
        }
        Serial.println("[LOCAL] Zeit abgelaufen -> STOPPED");
    }

    // Warnung bei <= 5 Sekunden: kurzer Buzz pro Sekunde
    if (clockState == RUNNING && sec <= 5 && sec > 0 && sec != lastWarnSec) {
        lastWarnSec = sec;
        beep(50);
    }

    // Anzeigen aktualisieren wenn Sekundenwert ODER Signal-Status wechselt
    int  secToShow = (clockState == RUNNING) ? sec : displaySec;
    bool hasSignal = (lastRxMs > 0);
    if (secToShow != lastShown || hasSignal != lastSignal) {
        lastShown  = secToShow;
        lastSignal = hasSignal;
        drawOLED(secToShow, clockState);
        drawP10(secToShow, hasSignal);
        if (clockState == RUNNING) {
            Serial.printf("[LOCAL] %ds RUNNING\n", secToShow);
        }
    }

    delay(50); // 20 Hz Loop - ausreichend fuer 1s-Timer
}
