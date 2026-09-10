# Shotclock — drahtlose Wurfuhr für Lacrosse

Autarke Shotclock-Anlage: Ein Tablet steuert über WLAN einen Controller, der per
LoRa-Funk eine große LED-Anzeige und ein Horn ansteuert. **Kein Router, kein
Raspberry Pi, kein Internet** — die Anlage macht ihr eigenes WLAN auf und läuft
an einer Powerbank.

```
   Tablet
     │ WLAN (Hotspot "Shotclock", 192.168.4.1)
     ▼
┌──────────────┐   LoRa 868 MHz   ┌──────────────────┐
│  CONTROLLER  │─────────┬───────►│ PANEL-EMPFÄNGER  │  32×32 LED, OLED, Buzzer
│  Web-App     │         │        └──────────────────┘
│  Timer-Logik │         │
└──────────────┘         └───────►┌──────────────────┐
                                  │   HORN-MODUL     │  MOSFET → Signalhorn
                                  └──────────────────┘
```

Der Controller ist die Zeitreferenz. Panel und Horn zählen bzw. reagieren
**lokal** — kurze Funkaussetzer sind unkritisch, ein `SYNC` alle 10 s korrigiert
eventuelle Abweichungen.

---

## 1. Bedienung

### Tablet verbinden (einmalig)

1. WLAN **`Shotclock`**, Passwort **`shotclock`**
2. Die App öffnet sich meist automatisch (Captive Portal), sonst im Browser
   **`http://192.168.4.1`** — die IP ist fest.
3. „Kein Internet"-Warnung ignorieren, das Netz ist absichtlich offline.

### Als App im Vollbild

* **Android/Chrome:** Menü ⋮ → *Zum Startbildschirm hinzufügen*
* **iPad/Safari:** Teilen → *Zum Home-Bildschirm*
* Alternativ der Vollbild-Knopf oben rechts.

Die App hält den Bildschirm wach und zeigt oben den Verbindungsstatus.

### Hauptseite

| Element | Funktion |
|---|---|
| **START / STOP** | Umschalter, folgt dem Zustand (grün ↔ rot) |
| **RESET** | Uhr **läuft** → auf Maximum und **läuft sofort weiter**<br>Uhr **gestoppt** → auf Maximum, bleibt stehen |
| **🔊 HORN** (oben) | 1,5 s Hornsignal von Hand |
| **Zahl antippen** | Ziffernblock: Zeit manuell setzen (1–99 s, ändert das Modus-Maximum **nicht**) |
| **⚙ / Modus-Badge** | Einstellungen |

Bei ≤ 5 s pulsiert die Anzeige; bei 0 stoppt alles automatisch, Buzzer und Horn
lösen aus.

### Einstellungen

* **Spielmodus:** SIXES (30 s) · FIELD (80 s) · eigene Zeit (1–99 s)
* **Hintergrund:** Herren / Damen / Aus (wird pro Tablet gespeichert)
* **Horn:** Auto-Horn bei Ablauf ein/aus · Horn testen
* **System:** WLAN, IP, verbundene Geräte, LoRa-Status, gesendete Pakete,
  Laufzeit, Firmware-Version

---

## 2. Module

### `shotclock_ctrl` — Controller (FW 1.4.0)

Herzstück: WLAN-Hotspot, Web-App, Timer-Logik, LoRa-Sender.
Hardware: **LilyGO T3-S3 (SX1262, OLED-Variante)**.
Das OLED zeigt SSID, IP, Anzahl verbundener Tablets, Modus und Restzeit.

Wichtige `#define`s ganz oben in der Datei: `AP_SSID`, `AP_PASS`,
`DEFAULT_MAX`, `HORN_MANUAL_MS`, `HORN_EXPIRE_MS`, `TX_PHASE_MS`.

### `shotclock_rx` — Panel-Empfänger

Treibt die LED-Anzeige, führt einen eigenen lokalen Countdown, Buzzer bei ≤ 5 s
und bei 0. Hardware: **T3-S3 (OLED-Variante)** + 2× P10-Panel.
Mehrere Empfänger können parallel laufen (alle zeigen dasselbe).

### `shotclock_horn` — Horn-Modul

Schaltet über ein MOSFET-Modul ein Signalhorn. Reagiert auf `CMD:HORN` und auf
die **BOOT-Taste** am Board (manueller Blast ohne Tablet).
Hardware: **T3-S3 E-PAPER-Variante** ⚠️ (andere Belegung, siehe unten).

### Nicht mehr im Betrieb (Archiv)

| Ordner | Zweck |
|---|---|
| `shotclock_tx` | altes USB-Relay für den Pi-Betrieb |
| `shotclock_pi` | alter Raspberry-Pi-Stack (Flask + Timer + Serial) |
| `shotclock_paneltest` | Testbaukasten, mit dem das P10-Pixelmapping ermittelt wurde |

---

## 3. Funkprotokoll

Klartext, Format **`CMD:<befehl>:<wert>`**. Zustandswechsel werden **3×** mit
200 ms Abstand gesendet (Redundanz), `SYNC` und `HORN` seltener. Empfänger
ignorieren Befehle, die sie nichts angehen.

| Befehl | Wert | Wirkung |
|---|---|---|
| `START` | Sekunden | Countdown ab Wert starten |
| `STOP` | Sekunden | anhalten, Wert anzeigen |
| `RESET` | Sekunden | Wert setzen, gestoppt |
| `SYNC` | Sekunden | Driftkorrektur (nur bei Abweichung > 1 s) |
| `HORN` | Millisekunden | Hornsignal, max. 5000 |

**Funkparameter (müssen auf allen Modulen identisch sein):**
868.125 MHz · BW 125 kHz · SF 8 · CR 5 · Sync 0x12 · Preamble 12

**HTTP-API des Controllers:**

```
GET  /status   → {"state","seconds","max","remms","autohorn","connected"}
GET  /info     → {"ssid","ip","stations","lora","tx","uptime","fw"}
POST /cmd?c=START|STOP|RESET|HORN|SETMAX 30|SETSEC 17|AUTOHORN 0
```

---

## 4. Hardware & Verdrahtung

### Panel-Empfänger → P10-Anzeige

Zwei P10-Module (32×16, 1/4-Scan, Treiber ICN2037BP) **übereinander** = 32×32.

📄 **Ausführliche Anleitung:
[`shotclock_rx/HUB75_Verdrahtung_T3-S3.md`](shotclock_rx/HUB75_Verdrahtung_T3-S3.md)**
— Steckerbelegung mit Diagramm, Header-Position der GPIOs am Board,
Panel-Kette, Stromversorgung, Pixelmapping und Checkliste vor dem Einschalten.

| HUB75 | GPIO | | HUB75 | GPIO |
|---|---|---|---|---|
| R1 | 15 | | A | 36 |
| G1 | 16 | | B | 38 |
| B1 | 12 | | CLK | 39 |
| R2 | 48 | | LAT | 40 |
| G2 | 47 | | OE | 41 |
| B2 | 35 | | C/D/E | ungenutzt |

* ⚠️ **GPIO 18 nicht verwenden** (Onboard-OLED-SDA). SD-Karte belegt 2/11/13/14,
  LoRa 3/5/6/7/8/33/34.
* ⚠️ **Ribbon muss am EINGANG (IN) des Panels stecken.** Am OUT bleibt alles
  dunkel (~100 mA Ruhestrom) — das hat uns einmal Stunden gekostet.
* Jedes Panel bekommt **eigene 5 V** vom Netzteil (Meanwell LRS-100-5), nicht
  durchs Ribbon. **Gemeinsame Masse** ESP ↔ Netzteil ist Pflicht.

### Horn-Modul → MOSFET → Horn

MOSFET-Treibermodul **AOD4184** (15 A, 5–36 V DC, aktiv HIGH, 3,3 V-Steuerung).

```
ESP GPIO 12 ──► SIG        Netzteil + ──► V+      Horn + ──► OUT+
ESP GND     ──► GND        Netzteil − ──► V−      Horn − ──► OUT−
```

* ⚠️ **GPIO 12, nicht GPIO 2** — auf der E-Paper-Variante ist GPIO 2 die
  SD-Karten-MISO-Leitung.
* ⚠️ **Freilaufdiode** (1N5408 / SB560) antiparallel direkt am Horn,
  **Kathode/Ring an +**. Ein Horn ist induktiv; die Abschaltspitze zerstört
  sonst den MOSFET. Verpolt eingebaut = Kurzschluss über dem Netzteil.
* Empfohlen: 10 kΩ Pulldown von SIG nach GND (verhindert Horn-Blipsen beim Boot).
* Gemeinsame Masse ESP ↔ Netzteil erforderlich (der MOSFET schaltet low-side).

---

## 5. Bauen & Flashen

Voraussetzung: PlatformIO Core.

```bash
export PATH=$PATH:~/.platformio/penv/bin

cd shotclock_ctrl        # bzw. shotclock_rx / shotclock_horn
pio run                                        # bauen
pio run --target upload --upload-port /dev/ttyACM0
```

**Vor dem Flashen prüfen, welches Board hängt** — alle sehen gleich aus:

```bash
ls -l /dev/serial/by-id/
```

| MAC | Rolle | Firmware |
|---|---|---|
| `1C:DB:D4:80:9F:9C` | Controller | `shotclock_ctrl` |
| `1C:DB:D4:80:9E:1C` | Panel-Empfänger | `shotclock_rx` |
| `1C:DB:D4:80:1D:10` | zweiter Empfänger | `shotclock_rx` |
| `24:EC:4A:27:2B:E4` | Horn (E-Paper-Board) | `shotclock_horn` |

Serielle Ausgabe ansehen: `pio device monitor` (braucht ein echtes Terminal).

⚠️ **Nie zwei Controller gleichzeitig betreiben** — beide funken dasselbe WLAN
und senden konkurrierende Befehle.

---

## 6. Fallstricke (teuer erkauft)

**Vollflächige Farben taugen nicht als Panel-Test.** Sie sehen bei *jedem*
Pixelmapping korrekt aus. Immer mit Rahmen, Diagonale oder Ziffern testen.

**Kein eingebauter Scan-Typ der HUB75-Library passt zu diesen P10-Panels.**
Sie verschachteln in 8-Pixel-Blöcken; ein Zeilenpaar (y, y+4) teilt sich eine
elektrische Zeile — obere Zeile = gerade Blöcke, untere = ungerade. Das eigene
Mapping steckt in `p10Pixel()` in `shotclock_rx`. Das obere Panel ist zudem
180° gedreht montiert.

**`double_buff = true` ohne `flipDMABuffer()` = schwarzes Panel.** Deshalb steht
es überall auf `false`.

**T3-S3 E-Paper-Variante: `RADIO_POW_PIN` (GPIO 35) muss HIGH sein**, sonst ist
der SX1262 stromlos und `radio.begin()` scheitert. Danach ~1,5 s warten. Auf der
OLED-Variante gibt es diesen Pin nicht. Autoritative Belegung steht in
`Xinyuan-LilyGO/Lilygo-LoRa-Epaper-series` (nicht `LilyGo-LoRa-Series`); die
Wiki-Seiten dazu sind teils falsch.

**Browser/Panel-Synchronität ist bewusst aufwendig gelöst** — nicht
„vereinfachen". `/status` liefert mit `remms` die Restzeit in Millisekunden; der
Browser rechnet die HTTP-Latenz heraus und tickt auf einer Deadline
(150 ms Totband gegen Poll-Jitter). Der Controller-Tick ist zusätzlich um
`TX_PHASE_MS` (120 ms Funklaufzeit) verschoben, damit er auf der Panel-Phase
liegt. Reines Sekunden-Polling war sichtbar asynchron.

**Der Empfänger ignoriert START-Duplikate innerhalb von 600 ms**, sonst zieht
jede Redundanz-Wiederholung die Tick-Phase nach hinten (~0,6 s Panel-Delay). Das
Fenster darf nicht größer werden, sonst wird ein RESET-im-Lauf verschluckt.

**Im Web-UI braucht `#ghost` ein `pointer-events:none`** — das dekorative „88"
liegt sonst über der Zahl und schluckt alle Taps.

---

## 7. Offene Punkte

* E-Paper des Horn-Moduls wird noch nicht genutzt (böte sich als Statusanzeige an)
* In den Einstellungen als *GEPLANT* markiert: Panel-Helligkeit, Panel-Init,
  RSSI-Funktest
* Nach dem letzten RESET-Verhalten-Update müssen Controller und beide Empfänger
  neu geflasht werden
