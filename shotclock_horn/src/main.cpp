/*
 * Shotclock HORN-Modul  -  LilyGO T3-S3 **E-PAPER** Variante (V1.0, SX1262)
 *
 * Lauscht per LoRa auf CMD:HORN:<ms> und schaltet den Horn-Ausgang fuer
 * <ms> Millisekunden. Zusaetzlich manueller Ausloeser ueber BOOT-Taste.
 *
 *   - CMD:HORN:1500  -> Horn 1.5s  (manuell vom Tablet)
 *   - CMD:HORN:2000  -> Horn 2.0s  (automatisch bei Countdown 0)
 *   Redundanz-Duplikate (Wiederholungen < 500ms) werden ignoriert.
 *   Alle anderen CMDs (START/STOP/RESET/SYNC) werden ignoriert.
 *
 * WICHTIG zur E-Paper-Variante (Pins aus offiziellem LilyGO-Repo verifiziert):
 *   - RADIO_POW_PIN (GPIO 35) muss HIGH sein, sonst ist der SX1262 stromlos!
 *     Danach ~1.5s warten, bevor radio.begin() aufgerufen wird.
 *   - GPIO 2 ist SD-Karten-MISO -> NICHT als Ausgang verwenden.
 *   - Kein I2C-OLED; das E-Paper haengt an SPI (48/47/16/15/14/11), hier
 *     nicht genutzt. Statusanzeige laeuft ueber Serial + Onboard-LED.
 *
 * HORN-AUSGANG: GPIO 12 -> MOSFET-Treibermodul AOD4184 (15A, 5-36V DC),
 * aktiv HIGH (SIG HIGH = MOSFET leitet = Horn an). 3.3V Steuerung ok.
 * Beim Boot garantiert AUS.
 *
 * VERDRAHTUNG:
 *   ESP GPIO 12 -> SIG/PWM+   |   ESP GND -> GND/PWM-  (gemeinsame Masse!)
 *   Netzteil +   -> V+        |   Netzteil - -> V-
 *   Horn +       -> OUT+      |   Horn -     -> OUT-
 *   Freilaufdiode (1N5408/SB560) antiparallel zum Horn - Horn ist induktiv,
 *   die Abschaltspitze killt sonst den MOSFET. Kathode(Ring) an +.
 *   Empfohlen: 10k Pulldown von SIG nach GND (GPIO floatet kurz beim Boot).
 */
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// ---------------- Konfiguration ----------------
#define HORN_PIN         12     // MOSFET-Modul SIG  (GPIO 2 waere SD-MISO!)
#define HORN_ACTIVE_HIGH 1      // AOD4184-Modul: HIGH = an
#define HORN_BTN_PIN     0      // BOOT-Taste, aktiv LOW
#define HORN_BTN_MS      1500   // Blastdauer bei lokalem Taster
#define HORN_MAX_MS      5000   // Sicherheitslimit pro Blast

// --- Pins T3S3 E-Paper (verifiziert) ---
#define RADIO_SCLK_PIN   5
#define RADIO_MISO_PIN   3
#define RADIO_MOSI_PIN   6
#define RADIO_CS_PIN     7
#define RADIO_RST_PIN    8
#define RADIO_DIO1_PIN   33
#define RADIO_BUSY_PIN   34
#define RADIO_POW_PIN    35    // Funkmodul-Stromversorgung, HIGH = an
#define BOARD_LED        37

// --- LoRa-Parameter (identisch mit Controller!) ---
#define LORA_FREQ      868.125f
#define LORA_BW        125.0f
#define LORA_SF        8
#define LORA_CR        5
#define LORA_SYNC      0x12
#define LORA_POWER     14
#define LORA_PREAMBLE  12

#define IRQ_FLAGS (RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_CRC_ERR)

SPIClass spi(FSPI);
SX1262 radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN,
                          RADIO_BUSY_PIN, spi);

volatile bool rxFlag = false;
void IRAM_ATTR onReceive() { rxFlag = true; }

// Horn-Zustand
bool          hornOn        = false;
unsigned long hornOffMs     = 0;     // wann ausschalten
unsigned long lastTriggerMs = 0;     // Dedupe der Funk-Redundanz
int           hornCount     = 0;
float         lastRssi      = 0.0f;
unsigned long lastRxMs      = 0;

static inline void hornWrite(bool on) {
#if HORN_ACTIVE_HIGH
    digitalWrite(HORN_PIN, on ? HIGH : LOW);
#else
    digitalWrite(HORN_PIN, on ? LOW : HIGH);
#endif
    digitalWrite(BOARD_LED, on ? HIGH : LOW);
}

void hornTrigger(int ms, const char* src) {
    if (ms < 100)         ms = 100;
    if (ms > HORN_MAX_MS) ms = HORN_MAX_MS;
    hornOn    = true;
    hornOffMs = millis() + (unsigned long)ms;
    hornCount++;
    hornWrite(true);
    Serial.printf("[HORN] AN %dms (%s, #%d)\n", ms, src, hornCount);
}

// "CMD:HORN:1500" -> cmd="HORN", val=1500
bool parsePacket(const char* raw, char* cmd, int* val) {
    if (strncmp(raw, "CMD:", 4) != 0) return false;
    const char* colon = strchr(raw + 4, ':');
    if (!colon) return false;
    int cmdLen = (int)(colon - (raw + 4));
    if (cmdLen <= 0 || cmdLen > 8) return false;
    strncpy(cmd, raw + 4, cmdLen);
    cmd[cmdLen] = '\0';
    *val = atoi(colon + 1);
    return true;
}

void setup() {
    // Horn SOFORT auf AUS, bevor irgendwas anderes passiert
    pinMode(HORN_PIN, OUTPUT);
    pinMode(BOARD_LED, OUTPUT);
    hornWrite(false);
    pinMode(HORN_BTN_PIN, INPUT_PULLUP);

    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== SHOTCLOCK HORN (T3-S3 E-Paper) ===");
    Serial.printf("Horn-Ausgang: GPIO %d, aktiv %s\n",
                  HORN_PIN, HORN_ACTIVE_HIGH ? "HIGH" : "LOW");

    // Funkmodul mit Strom versorgen (E-Paper-Variante!) und stabilisieren
    pinMode(RADIO_POW_PIN, OUTPUT);
    digitalWrite(RADIO_POW_PIN, HIGH);
    delay(1500);

    spi.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);
    int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                         LORA_SYNC, LORA_POWER, LORA_PREAMBLE, 1.8f);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[HORN] LoRa FEHLER: %d - Neustart in 5s\n", st);
        delay(5000);
        ESP.restart();
    }
    radio.setDio2AsRfSwitch();
    radio.setDio1Action(onReceive);
    radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, IRQ_FLAGS, IRQ_FLAGS);

    Serial.println("[HORN] LoRa OK. Bereit - warte auf CMD:HORN...");
    Serial.println("[HORN] BOOT-Taste = manueller Blast (Trockentest)");
}

unsigned long btnDownMs   = 0;
unsigned long lastAliveMs = 0;

void loop() {
    unsigned long now = millis();

    // --- LoRa-Empfang ---
    if (rxFlag) {
        rxFlag = false;
        uint16_t irq = radio.getIrqStatus();
        if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
            uint8_t buf[64] = {0};
            int len = radio.getPacketLength();
            int err = radio.readData(buf, min(len, (int)sizeof(buf) - 1));
            if (err == RADIOLIB_ERR_NONE && len > 0) {
                lastRssi = radio.getRSSI();
                lastRxMs = now;
                char cmd[16] = {0};
                int  val = 0;
                if (parsePacket((char*)buf, cmd, &val)) {
                    if (strcmp(cmd, "HORN") == 0) {
                        // Funk-Redundanz (Wiederholung < 500ms) nicht neu triggern
                        if (lastTriggerMs == 0 || now - lastTriggerMs >= 500UL) {
                            lastTriggerMs = now;
                            hornTrigger(val, "LoRa");
                        }
                    } else {
                        Serial.printf("[RX] %s:%d (ignoriert) %.0fdBm\n",
                                      cmd, val, lastRssi);
                    }
                }
            }
        }
        radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, IRQ_FLAGS, IRQ_FLAGS);
    }

    // --- lokaler Taster (aktiv LOW, einfache Entprellung) ---
    if (digitalRead(HORN_BTN_PIN) == LOW) {
        if (btnDownMs == 0) {
            btnDownMs = now;
        } else if (now - btnDownMs > 30 &&
                   (lastTriggerMs == 0 || now - lastTriggerMs >= 500UL)) {
            lastTriggerMs = now;
            hornTrigger(HORN_BTN_MS, "Taster");
        }
    } else {
        btnDownMs = 0;
    }

    // --- Horn ausschalten wenn Zeit um ---
    if (hornOn && (long)(now - hornOffMs) >= 0) {
        hornOn = false;
        hornWrite(false);
        Serial.println("[HORN] AUS");
    }

    // --- Lebenszeichen alle 10s ---
    if (now - lastAliveMs >= 10000) {
        lastAliveMs = now;
        if (lastRxMs > 0)
            Serial.printf("[HORN] bereit | Funk OK %.0fdBm | Ausloesungen %d\n",
                          lastRssi, hornCount);
        else
            Serial.printf("[HORN] bereit | noch kein Funkpaket | Ausloesungen %d\n",
                          hornCount);
    }

    delay(5);
}
