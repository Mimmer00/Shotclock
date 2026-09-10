# Verdrahtung HUB75 → LilyGO T3-S3 (SX1262, V1.2)

Für **2× P10 RGB Outdoor, 32×16, 1/4 Scan**, gestapelt zu 32×32.
Verifiziert gegen das offizielle T3-S3 V1.2 Schaltbild.

> **Geltungsbereich:** Dieses Dokument beschreibt die **physische Verdrahtung**
> (Steckerbelegung, Header, Strom). Es ist älter als der aktuelle Code.
> **Bei Widersprüchen gilt immer der Code** in `src/main.cpp` — insbesondere für
> Pixelmapping, Panel-Zuordnung und Software-Verhalten.

---

## 1. Verdrahtungstabelle

| HUB75 Pin | Signal    | → ESP32 GPIO | Funktion                          |
|:---------:|-----------|:------------:|-----------------------------------|
| 1         | R1        | **15**       | Rot, obere Hälfte                 |
| 2         | G1        | **16**       | Grün, obere Hälfte                |
| 3         | B1        | **12**       | Blau, obere Hälfte                |
| 4         | GND       | GND          | Masse                             |
| 5         | R2        | **48**       | Rot, untere Hälfte                |
| 6         | G2        | **47**       | Grün, untere Hälfte               |
| 7         | B2        | **35**       | Blau, untere Hälfte               |
| 8         | E         | GND          | bei 1/4 Scan nicht genutzt        |
| 9         | A         | **36**       | Zeilenadresse Bit 0               |
| 10        | B         | **38**       | Zeilenadresse Bit 1               |
| 11        | C         | GND          | bei 1/4 Scan nicht genutzt        |
| 12        | D         | GND          | bei 1/4 Scan nicht genutzt        |
| 13        | CLK       | **39**       | Shift Clock                       |
| 14        | LAT (STB) | **40**       | Latch / Strobe                    |
| 15        | OE        | **41**       | Output Enable (Helligkeit/PWM)    |
| 16        | GND       | GND          | Masse                             |

**11 Signalleitungen + Masse.** Fünf HUB75-Pins (4, 8, 11, 12, 16) gehen auf GND.

---

## 2. Der HUB75-Stecker (Panel-Eingang, Draufsicht)

16-Pin IDC, 2 Reihen à 8. Pin 1 ist auf der Platine meist mit Dreieck, Pfeil oder „1" markiert.

```
              HUB75 IN
        ┌───────────────────┐
   R1 ──│  1 ●        ● 2   │── G1
   B1 ──│  3 ●        ● 4   │── GND
   R2 ──│  5 ●        ● 6   │── G2
   B2 ──│  7 ●        ● 8   │── E   → GND
    A ──│  9 ●        ● 10  │── B
    C ──│ 11 ●        ● 12  │── D          (C → GND, D → GND)
  CLK ──│ 13 ●        ● 14  │── LAT
   OE ──│ 15 ●        ● 16  │── GND
        └───────────────────┘
          ungerade   gerade
```

Mit den GPIO-Nummern eingetragen:

```
        ┌───────────────────┐
   15 ──│  1 ●        ● 2   │── 16
   12 ──│  3 ●        ● 4   │── GND
   48 ──│  5 ●        ● 6   │── 47
   35 ──│  7 ●        ● 8   │── GND
   36 ──│  9 ●        ● 10  │── 38
  GND ──│ 11 ●        ● 12  │── GND
   39 ──│ 13 ●        ● 14  │── 40
   41 ──│ 15 ●        ● 16  │── GND
        └───────────────────┘
```

---

## 3. Wo die GPIOs am T3-S3 liegen

Das Board hat zwei unbestückte 13-Pin-Header. **Nur JP1 (links) führt Strom.**

### JP1 (links, „L") — verifiziert

| JP1 Pin | Netz          | Verwendung im Projekt         |
|:-------:|---------------|-------------------------------|
| 1       | **+5V**       | Versorgung ESP (Einspeisung)  |
| 2       | **GND**       | Masse                         |
| 3       | VCC3V3        | ⚠️ 3,3V-Ausgang – **nie 5V anlegen** |
| 4       | GND           | Masse                         |
| 5       | IO42          | frei (Reserve)                |
| 6       | IO46          | Strap-Pin – **nicht nutzen**  |
| 7       | IO45          | Strap-Pin – **nicht nutzen**  |
| 8       | IO41          | **OE**  → HUB75 Pin 15        |
| 9       | IO40          | **LAT** → HUB75 Pin 14        |
| 10      | IO39          | **CLK** → HUB75 Pin 13        |
| 11      | GPIO44 (RX)   | frei                          |
| 12      | GPIO43 (TX)   | frei                          |
| 13      | IO38          | **B**   → HUB75 Pin 10        |

### JP2 (rechts, „R")

Hier liegen die restlichen Signale: **12, 15, 16, 35, 36, 47, 48**.
Die Pin-Reihenfolge auf JP2 am Board ablesen (Beschriftung auf der Platine),
sie ist im Schaltbild nicht eindeutig lesbar.

### Tabu-Pins (nicht fürs Display verwenden)

| GPIO       | Grund                                   |
|------------|-----------------------------------------|
| 3, 5, 6, 7, 8, 33, 34 | LoRa SX1262 (SPI, DIO1, BUSY, RST, CS) |
| 17, 18     | OLED (I2C)                              |
| 37         | Onboard-LED (grün)                      |
| 0, 45, 46  | Strapping-/Boot-Pins                    |
| 2, 11, 13, 14 | SD-Karte (frei, falls SD ungenutzt)  |

---

## 4. Panel-Kette (2 Panels übereinander)

```
   ┌───────────────┐
   │   T3-S3       │
   └───────┬───────┘
           │ 11 Signale + GND (Jumper-Kabel, < 15 cm)
           ▼
   ┌───────────────────────┐
   │  Panel A (erstes)     │
   │  HUB75 IN   HUB75 OUT ├──┐
   └───────────────────────┘  │ Flachbandkabel (liegt bei)
                              ▼
   ┌───────────────────────┐
   │  Panel B (zweites)    │
   │  HUB75 IN             │
   └───────────────────────┘
```

Welches der beiden physisch oben bzw. unten hängt, ist für die Verdrahtung
egal — **das regelt das Mapping im Code** (`p10Pixel()`).

Die Kette läuft immer **IN → OUT → IN**.

> ⚠️ **Das Flachbandkabel muss am EINGANG (IN) des Panels stecken.** Steckt es
> am OUT, bleibt das Panel komplett dunkel und zieht nur ~100 mA Ruhestrom.
> Diese Verwechslung hat beim Aufbau Stunden gekostet.

### Verifizierte Zuordnung (empirisch ausgemessen)

| Elektrischer Block | Physisches Panel |
|---|---|
| Block 0 (DMA-Spalten 0–63) | **oberes** Panel, **180° gedreht** montiert |
| Block 1 (DMA-Spalten 64–127) | **unteres** Panel, normal |

Das steckt so in `p10Pixel()` in `src/main.cpp`. Wenn du die Panels anders
verkabelst oder anders herum montierst, müssen dort `block` und die
180°-Spiegelung angepasst werden.

---

## 5. Stromversorgung

```
   5V-Netzteil (z.B. Meanwell LRS-100-5, 5V/20A)
        │
        ├── dicke Leitung ──► Panel 1  (5V / GND)
        ├── dicke Leitung ──► Panel 2  (5V / GND)
        └── eigene Leitung ─► T3-S3 JP1 Pin 1 (+5V) / Pin 2 (GND)
                              + Elko 470–1000 µF direkt am Header
```

- **Jedes Panel bekommt eine eigene 5V-Leitung.** Strom nie über das Flachbandkabel führen.
- **ESP eigene Leitung vom Netzteil**, nicht vom Panel abgezweigt – sonst Brownout bei Panel-Strompulsen.
- **Gemeinsame Masse** zwischen Netzteil, Panels und ESP ist Pflicht.
- Verbrauch Timer-Betrieb (rot, ~20 % Pixel): ca. 5–8 W gesamt. Maximum (Vollweiß): bis 76 W.

### Verhalten ohne USB

| Speisung      | Blaue LED (Lade-LED) | Grüne LED (GPIO37) | ESP läuft |
|---------------|:--------------------:|:------------------:|:---------:|
| USB-C         | leuchtet             | nur per Code       | ja        |
| JP1 +5V       | **dunkel** (normal)  | nur per Code       | ja        |

Die blaue LED hängt an VBUS und ist ohne USB immer aus – kein Fehler.

---

## 6. Code-Defines (Kopiervorlage)

```cpp
// HUB75 – T3-S3 Belegung (verifiziert)
#define R1_PIN  15
#define G1_PIN  16
#define B1_PIN  12
#define R2_PIN  48
#define G2_PIN  47
#define B2_PIN  35
#define A_PIN   36
#define B_PIN   38
#define C_PIN   42   // Dummy, HUB75-Pin 11 liegt auf GND
#define D_PIN   -1   // HUB75-Pin 12 liegt auf GND
#define E_PIN   -1   // HUB75-Pin 8  liegt auf GND
#define CLK_PIN 39
#define LAT_PIN 40
#define OE_PIN  41

// LoRa SX1262 – fest verdrahtet, nicht ändern
// CS=7  DIO1=33  RST=8  BUSY=34  SCK=5  MISO=3  MOSI=6
```

---

## 6b. Pixelmapping ⚠️ Sonderfall

**Kein eingebauter Scan-Typ der ESP32-HUB75-Library funktioniert mit diesen
Panels** – weder `FOUR_SCAN_16PX_HIGH` noch `FOUR_SCAN_32PX_HIGH` noch die
Rechts-nach-links-Variante. Alle liefern ein zerstückeltes Bild.

Diese Panels verschachteln in **8-Pixel-Blöcken**, und ein Zeilenpaar (y, y+4)
teilt sich eine elektrische Zeile: die obere Zeile belegt die geraden Blöcke,
die untere die ungeraden. Ermittelt wurde das durch Roh-DMA-Sonden und Fotos
des Panels.

```cpp
// pro Panel: logisch (lx 0..31, ly 0..15) -> elektrisch (ex 0..63, ey 0..7)
int blk = lx / 8, o = lx % 8;            // KEINE Umkehr innerhalb des Blocks
int s   = ((ly & 4) == 0) ? 0 : 1;       // obere Paar-Zeile = gerade Blöcke
ex = (2 * blk + s) * 8 + o;
ey = ((ly >> 3) & 1) * 4 + (ly & 3);     // Kanal 1 = Zeilen 0–7, Kanal 2 = 8–15
```

Die DMA wird dabei als `HUB75_I2S_CFG(64, 8, 2)` konfiguriert und **direkt**
per `drawPixel()` beschrieben – die Virtual-Panel-Klasse der Library wird
bewusst *nicht* verwendet.

> **`double_buff` muss `false` bleiben.** Mit `true` und ohne `flipDMABuffer()`
> bleibt das Panel komplett schwarz.

> **Vollflächige Farben taugen nicht als Test.** Sie sehen bei *jedem* Mapping
> korrekt aus. Immer mit Rahmen, Diagonale oder Ziffern prüfen.

---

## 7. Checkliste vor dem Einschalten

- [ ] Pin 1 am Flachbandkabel (rote Ader) auf Pin 1 am Panel
- [ ] HUB75-Pins 4, 8, 11, 12, 16 auf GND
- [ ] Kein Draht an GPIO 0, 33, 34, 45, 46
- [ ] Kein 5V an JP1 Pin 3 (VCC3V3)
- [ ] Masse Netzteil ↔ ESP ↔ Panels verbunden
- [ ] Jumper-Kabel ESP → Panel kürzer als 15 cm
- [ ] Elko am ESP-Header gesetzt
- [ ] `Serial.setTxTimeoutMs(0);` im Setup (kein Blockieren ohne USB)
