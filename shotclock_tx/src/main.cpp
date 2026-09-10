/*
 * Shotclock T3S3 Sender
 * Liest CMD-Pakete vom Pi (USB-Serial) und sendet sie per LoRa weiter.
 * Protokoll: "CMD:START:28\n", "CMD:STOP:15\n", "CMD:RESET:30\n", "CMD:SYNC:22\n"
 */
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// --- Pins T3S3 ---
#define PIN_SCK   5
#define PIN_MISO  3
#define PIN_MOSI  6
#define PIN_CS    7
#define PIN_RST   8
#define PIN_DIO1  33
#define PIN_BUSY  34
#define PIN_LED   37

// --- LoRa-Parameter ---
#define LORA_FREQ      868.125f
#define LORA_BW        125.0f
#define LORA_SF        8
#define LORA_CR        5
#define LORA_SYNC      0x12
#define LORA_POWER     22
#define LORA_PREAMBLE  12

SPIClass spi(FSPI);
SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);

int txCount = 0;

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(50); // Kurzes Timeout damit readStringUntil nicht 1s wartet

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

    int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                         LORA_SYNC, LORA_POWER, LORA_PREAMBLE, 1.8f);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[ERROR] Radio init fehlgeschlagen: %d\n", st);
        // Fehler-Blinken
        while (true) {
            digitalWrite(PIN_LED, HIGH); delay(200);
            digitalWrite(PIN_LED, LOW);  delay(200);
        }
    }
    radio.setDio2AsRfSwitch();

    Serial.println("[TX] Bereit. Warte auf CMD-Pakete vom Pi...");
}

void loop() {
    if (!Serial.available()) return;

    String line = Serial.readStringUntil('\n');
    line.trim();

    // Nur gueltige CMD-Pakete weiterleiten
    if (!line.startsWith("CMD:") || line.length() < 8) return;

    // LED waehrend Transmission einschalten
    digitalWrite(PIN_LED, HIGH);

    int st = radio.transmit((uint8_t*)line.c_str(), line.length());

    digitalWrite(PIN_LED, LOW);

    if (st == RADIOLIB_ERR_NONE) {
        txCount++;
        Serial.printf("[TX_LORA] %s (#%d)\n", line.c_str(), txCount);
    } else {
        Serial.printf("[TX_ERROR] %s err=%d\n", line.c_str(), st);
    }
}
