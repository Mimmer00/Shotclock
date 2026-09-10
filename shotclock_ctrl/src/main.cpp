/*
 * Shotclock CONTROLLER (Standalone, ersetzt Pi + TX-Relay)
 *
 * - WLAN-Hotspot "Shotclock" (feste IP 192.168.4.1) + Captive Portal
 * - ASYNC-Webserver: Tablet-App (Vollbild/PWA) mit Scoreboard-Design
 *     Hauptseite:  grosse LED-Ziffern, START/STOP-Toggle, RESET,
 *                  Modus SIXES (30s) / FIELD (80s)
 *     /settings:   Modus + eigene Zeit, System-Info, geplante Funktionen
 * - Timer-Logik lokal: 1s-Tick, SYNC alle 10s, Auto-Stop bei 0
 * - LoRa-TX ueber Sendequeue (nicht blockierend im Request-Handler)
 * - OLED zeigt SSID/IP/Tablets/Countdown
 *
 * API:  GET  /status  -> {"state":..,"seconds":..,"max":..}
 *       POST /cmd?c=START|STOP|RESET|SETMAX%2030
 *       GET  /info    -> {"ssid":..,"stations":..,"lora":..,"tx":..,"uptime":..,"fw":..}
 */
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include "bg_images.h"

// ---------------- Konfiguration ----------------
#define FW_VERSION  "1.4.0"
#define AP_SSID     "Shotclock"
#define AP_PASS     "shotclock"     // min. 8 Zeichen
#define DEFAULT_MAX 30              // Startmodus: SIXES
#define HORN_MANUAL_MS 1500         // Blastdauer manueller Horn-Button
#define HORN_EXPIRE_MS 2000         // Blastdauer bei Countdown 0
// Funklaufzeit + RX-Verarbeitung: Controller-Tick auf die Panel-Phase schieben,
// damit Browser und Panel gleichzeitig umklappen.
#define TX_PHASE_MS 120

// --- Pins T3S3 ---
#define PIN_SCK   5
#define PIN_MISO  3
#define PIN_MOSI  6
#define PIN_CS    7
#define PIN_RST   8
#define PIN_DIO1  33
#define PIN_BUSY  34
#define PIN_LED   37
#define OLED_SDA  18
#define OLED_SCL  17

// --- LoRa-Parameter (identisch mit Empfaenger!) ---
#define LORA_FREQ      868.125f
#define LORA_BW        125.0f
#define LORA_SF        8
#define LORA_CR        5
#define LORA_SYNC      0x12
#define LORA_POWER     22
#define LORA_PREAMBLE  12

SPIClass spi(FSPI);
SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spi);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
AsyncWebServer server(80);
DNSServer dns;
bool oledOk = false;
bool loraOk = false;

// ---------------- Timer-Zustand (mux-geschuetzt) ----------------
enum ClockState : uint8_t { ST_STOP = 0, ST_START = 1 };
portMUX_TYPE clkMux = portMUX_INITIALIZER_UNLOCKED;
ClockState    clkState   = ST_STOP;
int           clkSeconds = DEFAULT_MAX;
int           clkMax     = DEFAULT_MAX;
unsigned long nextTickMs = 0;
bool          autoHorn   = true;    // Horn automatisch bei Countdown 0

// ---------------- LoRa-Sendequeue ----------------
// Request-Handler laufen im Async-Task -> nie direkt funken (blockiert!).
// Jobs landen in einer FreeRTOS-Queue, loop() sendet mit 200ms-Abstand.
struct TxJob { char cmd[8]; int16_t sec; uint8_t repeat; };
QueueHandle_t txQueue;
TxJob         txCur;
uint8_t       txRepeatsLeft = 0;
unsigned long txNextMs      = 0;
int           txCount       = 0;

void queueLora(const char* cmd, int sec, uint8_t repeat) {
    TxJob j;
    strncpy(j.cmd, cmd, sizeof(j.cmd) - 1);
    j.cmd[sizeof(j.cmd) - 1] = '\0';
    j.sec = sec;
    j.repeat = repeat;
    xQueueSend(txQueue, &j, 0);      // Queue voll -> Job verwerfen (kommt nicht vor)
}

void processLoraQueue() {
    if (txRepeatsLeft == 0) {
        if (xQueueReceive(txQueue, &txCur, 0) == pdTRUE) {
            txRepeatsLeft = txCur.repeat;
            txNextMs = millis();
        }
        return;
    }
    if (millis() < txNextMs) return;

    char pkt[24];
    snprintf(pkt, sizeof(pkt), "CMD:%s:%d", txCur.cmd, txCur.sec);
    digitalWrite(PIN_LED, HIGH);
    int st = radio.transmit((uint8_t*)pkt, strlen(pkt));   // ~70ms, einzeln ok
    digitalWrite(PIN_LED, LOW);
    if (st == RADIOLIB_ERR_NONE) txCount++;
    else Serial.printf("[TX_ERROR] %s err=%d\n", pkt, st);

    txRepeatsLeft--;
    txNextMs = millis() + 200;
    if (txRepeatsLeft == 0) Serial.printf("[TX_LORA] %s (#%d)\n", pkt, txCount);
}

// ---------------- Timer-Kommandos (aus Async-Task aufrufbar) ----------------
void cmdStart() {
    portENTER_CRITICAL(&clkMux);
    clkState   = ST_START;
    nextTickMs = millis() + 1000 + TX_PHASE_MS;   // Phase = Panel-Phase
    int sec = clkSeconds;
    portEXIT_CRITICAL(&clkMux);
    queueLora("START", sec, 3);
}
void cmdStop() {
    portENTER_CRITICAL(&clkMux);
    clkState = ST_STOP;
    int sec = clkSeconds;
    portEXIT_CRITICAL(&clkMux);
    queueLora("STOP", sec, 3);
}
void cmdReset() {
    portENTER_CRITICAL(&clkMux);
    clkState   = ST_STOP;
    clkSeconds = clkMax;
    int sec = clkSeconds;
    portEXIT_CRITICAL(&clkMux);
    queueLora("RESET", sec, 3);
}
void cmdHorn(int ms) {
    queueLora("HORN", ms, 2);   // 2x Redundanz; Horn-Modul dedupliziert
}
// Manuelle Zeitkorrektur: setzt nur die laufende Zeit, Modus-Maximum bleibt
void cmdSetSec(int v) {
    if (v < 0)  v = 0;
    if (v > 99) v = 99;
    portENTER_CRITICAL(&clkMux);
    clkSeconds = v;
    clkState   = ST_STOP;
    portEXIT_CRITICAL(&clkMux);
    queueLora("RESET", v, 3);   // Empfaenger: Wert setzen + gestoppt
}
void cmdSetMax(int v) {
    if (v < 1)  v = 1;
    if (v > 99) v = 99;
    portENTER_CRITICAL(&clkMux);
    clkMax     = v;
    clkSeconds = v;
    clkState   = ST_STOP;
    portEXIT_CRITICAL(&clkMux);
    queueLora("RESET", v, 3);
}

// 1s-Tick (im loop)
void tickTimer() {
    bool sendStop = false, sendSync = false;
    int  sec = 0;

    portENTER_CRITICAL(&clkMux);
    if (clkState == ST_START && millis() >= nextTickMs) {
        nextTickMs += 1000;
        clkSeconds = max(0, clkSeconds - 1);
        sec = clkSeconds;
        if (clkSeconds == 0) {
            clkState = ST_STOP;
            sendStop = true;
        } else if (clkSeconds % 10 == 0) {
            sendSync = true;
        }
    }
    portEXIT_CRITICAL(&clkMux);

    if (sendStop) {
        queueLora("STOP", 0, 3);
        if (autoHorn) cmdHorn(HORN_EXPIRE_MS);   // Ablauf -> Hornsignal
    }
    if (sendSync) queueLora("SYNC", sec, 1);
}

// ============================================================
//  WEB-UI  -  Hauptseite (Scoreboard)
// ============================================================
const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#000000">
<link rel="manifest" href="/manifest.json">
<title>Shotclock</title>
<style>
:root{--red:#ff2f2f;--amber:#ffb02f}
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
html{background:#000}
html,body{height:100%;color:#eee;font-family:system-ui,sans-serif;overflow:hidden}
body{background:transparent;display:flex;flex-direction:column;
     padding:max(10px,env(safe-area-inset-top)) 16px max(12px,env(safe-area-inset-bottom))}
#bg{position:fixed;inset:0;z-index:-1;background:center/cover no-repeat;
    filter:invert(1) grayscale(1);opacity:.22}
#top{display:flex;justify-content:space-between;align-items:center;gap:10px}
.pill{display:flex;align-items:center;gap:7px;border:1px solid #262630;border-radius:999px;
      padding:6px 13px;background:#0b0b0f;font-size:14px;color:#99a}
#dot{width:9px;height:9px;border-radius:50%;background:#555}
#dot.on{background:#2ecc40;box-shadow:0 0 8px #2ecc40}
#mode{font-weight:800;letter-spacing:.14em;color:var(--amber)}
#state{font-weight:700;letter-spacing:.08em}
#state.run{color:#2ecc40}
.icon{border:1px solid #262630;background:#0b0b0f;color:#99a;border-radius:12px;
      width:42px;height:42px;font-size:20px;line-height:1;display:flex;align-items:center;justify-content:center;text-decoration:none}
#stage{flex:1;position:relative;display:flex;align-items:center;justify-content:center;min-height:0}
#ghost,#sec{font-weight:800;font-variant-numeric:tabular-nums;letter-spacing:.05em;
            font-size:min(46vw,50vh);line-height:1}
#ghost{position:absolute;color:rgba(255,47,47,.06);user-select:none;pointer-events:none}
#sec{position:relative;z-index:1;color:var(--red);
     text-shadow:0 0 28px rgba(255,47,47,.55),0 0 90px rgba(255,47,47,.22)}
#sec.stopped{color:#9c1f1f;text-shadow:none}
#sec.warn{animation:pulse .9s ease-in-out infinite}
@keyframes pulse{50%{text-shadow:0 0 40px rgba(255,47,47,.95),0 0 130px rgba(255,47,47,.5)}}
#hint{position:absolute;bottom:2px;left:0;right:0;text-align:center;font-size:12px;
      color:#4a4a58;letter-spacing:.12em;pointer-events:none}
#bar{height:6px;border-radius:3px;background:#15151b;overflow:hidden;margin:2px 2px 14px}
#fill{height:100%;width:100%;border-radius:3px;background:linear-gradient(90deg,#ff2f2f,#ff8a2f);
      transition:width .3s linear}
#btns{display:grid;grid-template-columns:1.5fr 1fr;gap:12px}
button.big{border:none;border-radius:24px;height:clamp(120px,19vh,170px);
           font-size:clamp(30px,5vh,42px);font-weight:900;color:#fff;letter-spacing:.1em}
#bMain{background:linear-gradient(180deg,#22c85e,#12813c);box-shadow:0 8px 26px rgba(34,200,94,.22)}
#bMain.running{background:linear-gradient(180deg,#ff5749,#bf2a1e);box-shadow:0 8px 26px rgba(255,87,73,.22)}
#bReset{background:linear-gradient(180deg,#3d78f2,#2450b8);box-shadow:0 8px 26px rgba(61,120,242,.2)}
button.big:active{filter:brightness(1.25)}
#bHorn{border:none;border-radius:12px;height:42px;padding:0 18px;
       font-size:16px;font-weight:900;color:#1a1200;letter-spacing:.1em;
       background:linear-gradient(180deg,#ffc94d,#e08c14);box-shadow:0 4px 14px rgba(255,176,47,.25)}
#bHorn:active{filter:brightness(1.25)}
#bHorn.blast{animation:hornflash .45s ease-out}
@keyframes hornflash{0%{filter:brightness(2.2)}100%{filter:brightness(1)}}
a.pill{text-decoration:none}
#sec{cursor:pointer}
/* --- Zeit-Eingabe --- */
#ovl{position:fixed;inset:0;z-index:10;background:rgba(0,0,0,.86);
     display:none;align-items:center;justify-content:center}
#ovl.on{display:flex}
#box{background:#0b0b0f;border:1px solid #262630;border-radius:22px;padding:22px;
     width:min(92vw,360px)}
#ttl{font-size:13px;letter-spacing:.18em;color:#667;text-align:center;margin-bottom:8px}
#val{font-size:64px;font-weight:900;color:var(--red);text-align:center;
     font-variant-numeric:tabular-nums;line-height:1.1;margin-bottom:16px}
#pad{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
#pad button{border:1px solid #262630;background:#15151b;color:#eee;border-radius:14px;
            height:64px;font-size:26px;font-weight:800}
#pad button:active{filter:brightness(1.4)}
#pad .ok{background:linear-gradient(180deg,#22c85e,#12813c);border:none;color:#fff;font-size:20px}
#pad .clr{background:#2a1a1a;color:#ff8a80;font-size:20px}
#cancel{width:100%;margin-top:10px;height:52px;border:1px solid #262630;background:#111117;
        color:#889;border-radius:14px;font-size:16px;font-weight:700}
</style></head><body>
<div id="bg"></div>
<div id="top">
  <span class="pill"><span id="dot"></span><span id="conn">verbinde...</span></span>
  <a class="pill" href="/settings"><span id="mode">--</span></a>
  <span class="pill"><span id="state">--</span></span>
  <span style="display:flex;gap:8px;align-items:center">
    <button id="bHorn" onclick="horn()">&#128266; HORN</button>
    <button class="icon" id="fs" onclick="fullscreen()">&#x26F6;</button>
    <a class="icon" href="/settings">&#9881;</a>
  </span>
</div>
<div id="stage"><span id="ghost">88</span><span id="sec" onclick="openEdit()">--</span>
  <span id="hint">ZAHL ANTIPPEN = ZEIT SETZEN</span></div>
<div id="bar"><div id="fill"></div></div>
<div id="btns">
  <button class="big" id="bMain" onclick="mainBtn()">START</button>
  <button class="big" id="bReset" onclick="cmd('RESET')">RESET</button>
</div>

<div id="ovl">
  <div id="box">
    <div id="ttl">ZEIT SETZEN (1-99 s)</div>
    <div id="val">--</div>
    <div id="pad">
      <button onclick="dig('1')">1</button><button onclick="dig('2')">2</button><button onclick="dig('3')">3</button>
      <button onclick="dig('4')">4</button><button onclick="dig('5')">5</button><button onclick="dig('6')">6</button>
      <button onclick="dig('7')">7</button><button onclick="dig('8')">8</button><button onclick="dig('9')">9</button>
      <button class="clr" onclick="clrDig()">C</button>
      <button onclick="dig('0')">0</button>
      <button class="ok" onclick="okDig()">OK</button>
    </div>
    <button id="cancel" onclick="closeEdit()">Abbrechen</button>
  </div>
</div>
<script>
// Hintergrund aus localStorage (Auswahl in /settings)
(function(){
  const v=localStorage.getItem('bg')||'m';
  if(v!=='off')document.getElementById('bg').style.backgroundImage='url(/bg_'+v+'.jpg)';
})();
let last=null,lastOk=0,deadline=0,busy=false;

async function cmd(c){
  try{await fetch('/cmd?c='+encodeURIComponent(c),{method:'POST'});}catch(e){}
  poll();
}
function mainBtn(){cmd(last&&last.state==='START'?'STOP':'START');}
function horn(){
  const b=document.getElementById('bHorn');
  b.classList.remove('blast');void b.offsetWidth;b.classList.add('blast');
  if(navigator.vibrate)navigator.vibrate(60);
  cmd('HORN');
}

// --- Zeit-Eingabe ueber eigenen Ziffernblock ---
let buf='',editing=false;
function openEdit(){buf='';editing=true;showBuf();document.getElementById('ovl').classList.add('on');}
function closeEdit(){editing=false;document.getElementById('ovl').classList.remove('on');}
function showBuf(){document.getElementById('val').textContent=buf===''?'--':buf;}
function dig(d){if(buf.length<2){buf=(buf==='0'?'':buf)+d;showBuf();}}
function clrDig(){buf='';showBuf();}
function okDig(){
  const v=parseInt(buf);
  if(v>=1&&v<=99)cmd('SETSEC '+v);
  closeEdit();
}

async function poll(){
  if(busy)return;            // nie mehrere Polls parallel (Socket-Stau!)
  busy=true;
  const ac=new AbortController();
  const t=setTimeout(()=>ac.abort(),900);
  const t0=Date.now();
  try{
    const r=await fetch('/status',{cache:'no-store',signal:ac.signal});
    const j=await r.json();
    const t1=Date.now();
    // Serverstand ~ Mitte der Anfrage -> HTTP-Latenz herausrechnen
    const dl=t1-(t1-t0)/2+j.remms;
    // Totband: Poll-Jitter nicht nachziehen, echte Aenderungen sofort
    if(!last||last.state!==j.state||last.max!==j.max||Math.abs(dl-deadline)>150)
      deadline=dl;
    last=j;lastOk=t1;
  }catch(e){}
  clearTimeout(t);busy=false;
}
setInterval(poll,300);poll();

function render(){
  const el=document.getElementById('sec'),st=document.getElementById('state');
  const dot=document.getElementById('dot'),conn=document.getElementById('conn');
  if(!last){el.textContent='--';return;}
  const run=last.state==='START';
  const remms=run?Math.max(0,deadline-Date.now()):last.seconds*1000;
  const s=run?Math.ceil(remms/1000):last.seconds;   // tickt exakt auf Controller-Phase
  el.textContent=String(s).padStart(2,'0');
  el.className=(run?'':'stopped')+((run&&s<=5)?' warn':'');
  st.textContent=run?'LÄUFT':'GESTOPPT';
  st.className=run?'run':'';
  document.getElementById('fill').style.width=(100*remms/1000/Math.max(1,last.max))+'%';
  const age=Date.now()-lastOk;
  const ok=age<1500;
  dot.className=ok?'on':'';
  conn.textContent=ok?'verbunden':'OFFLINE';
  const m=document.getElementById('mode');
  m.textContent=(last.max===30)?'SIXES':(last.max===80)?'FIELD':(last.max+' s');
  const bm=document.getElementById('bMain');
  bm.textContent=run?'STOP':'START';
  bm.className='big'+(run?' running':'');
}
setInterval(render,100);

function fullscreen(){
  const d=document.documentElement;
  if(!document.fullscreenElement&&d.requestFullscreen)d.requestFullscreen();
  else if(document.exitFullscreen)document.exitFullscreen();
}
if(matchMedia('(display-mode: standalone),(display-mode: fullscreen)').matches)
  document.getElementById('fs').style.display='none';
let wl=null;
async function wake(){try{wl=await navigator.wakeLock.request('screen');}catch(e){}}
document.addEventListener('visibilitychange',()=>{if(!document.hidden)wake();});
wake();
</script></body></html>)HTML";

// ============================================================
//  WEB-UI  -  Einstellungen
// ============================================================
const char SETTINGS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#000000">
<title>Shotclock Einstellungen</title>
<style>
:root{--red:#ff2f2f;--amber:#ffb02f}
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
html,body{min-height:100%;background:#000;color:#eee;font-family:system-ui,sans-serif}
body{padding:max(10px,env(safe-area-inset-top)) 16px 24px;max-width:640px;margin:0 auto}
#top{display:flex;align-items:center;gap:12px;margin-bottom:16px}
#back{border:1px solid #262630;background:#0b0b0f;color:#99a;border-radius:12px;
      padding:10px 16px;font-size:16px;text-decoration:none}
h1{font-size:19px;letter-spacing:.12em;color:#ccd}
.card{background:#0b0b0f;border:1px solid #1d1d26;border-radius:16px;padding:16px;margin-bottom:14px}
.card h2{font-size:13px;letter-spacing:.16em;color:#667;margin-bottom:12px}
#modes{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:12px}
.mbtn{border:1px solid #262630;background:#111117;color:#778;border-radius:12px;
      padding:14px 0;font-size:16px;font-weight:800;letter-spacing:.1em}
.mbtn small{display:block;font-size:12px;font-weight:600;color:#556;margin-top:2px}
.mbtn.sel{border-color:var(--amber);color:var(--amber)}
#custom{display:flex;gap:8px;align-items:center;color:#889;font-size:15px}
#maxin{width:70px;background:#111117;border:1px solid #262630;color:#eee;border-radius:10px;
       padding:10px;font-size:17px;text-align:center}
.chip{border:1px solid #262630;background:#111117;color:#ccd;border-radius:10px;padding:10px 16px;font-size:15px;font-weight:700}
.chip.sel{border-color:var(--amber);color:var(--amber)}
#bgrow{display:flex;gap:10px}
table{width:100%;border-collapse:collapse;font-size:15px}
td{padding:7px 0;color:#99a}
td:last-child{text-align:right;color:#dde;font-variant-numeric:tabular-nums}
.ok{color:#2ecc40!important}.err{color:#ff5749!important}
.todo{display:flex;justify-content:space-between;align-items:center;padding:12px 0;
      border-top:1px solid #17171f;color:#556;font-size:15px}
.todo:first-of-type{border-top:none}
.badge{border:1px solid #33333f;border-radius:999px;padding:3px 10px;font-size:12px;color:#667;letter-spacing:.08em}
</style></head><body>
<div id="top"><a id="back" href="/">&#8592; Zur&uuml;ck</a><h1>EINSTELLUNGEN</h1></div>

<div class="card"><h2>SPIELMODUS</h2>
  <div id="modes">
    <button class="mbtn" id="mSixes" onclick="cmd('SETMAX 30')">SIXES<small>30 Sekunden</small></button>
    <button class="mbtn" id="mField" onclick="cmd('SETMAX 80')">FIELD<small>80 Sekunden</small></button>
  </div>
  <div id="custom">
    <span>Eigene Zeit:</span>
    <input id="maxin" type="number" min="1" max="99" placeholder="__">
    <button class="chip" onclick="setCustom()">&Uuml;bernehmen</button>
    <span id="curmax" style="margin-left:auto"></span>
  </div>
</div>

<div class="card"><h2>HINTERGRUND</h2>
  <div id="bgrow">
    <button class="chip" id="bg_m"   onclick="setBg('m')">Herren</button>
    <button class="chip" id="bg_w"   onclick="setBg('w')">Damen</button>
    <button class="chip" id="bg_off" onclick="setBg('off')">Aus</button>
  </div>
</div>

<div class="card"><h2>SYSTEM</h2>
  <table>
    <tr><td>WLAN</td><td id="iSsid">--</td></tr>
    <tr><td>IP-Adresse</td><td id="iIp">--</td></tr>
    <tr><td>Verbundene Ger&auml;te</td><td id="iSta">--</td></tr>
    <tr><td>LoRa-Funk</td><td id="iLora">--</td></tr>
    <tr><td>Gesendete Pakete</td><td id="iTx">--</td></tr>
    <tr><td>Laufzeit</td><td id="iUp">--</td></tr>
    <tr><td>Firmware</td><td id="iFw">--</td></tr>
  </table>
</div>

<div class="card"><h2>HORN</h2>
  <div class="todo"><span>Auto-Horn bei Ablauf (0 s)</span>
    <button class="chip" id="ahBtn" onclick="toggleAh()">--</button></div>
  <div class="todo"><span>Horn testen (1,5 s)</span>
    <button class="chip" onclick="cmd('HORN')">Ausl&ouml;sen</button></div>
</div>

<div class="card"><h2>GER&Auml;TE &amp; PANEL</h2>
  <div class="todo"><span>Panel-Init erneut senden</span><span class="badge">GEPLANT</span></div>
  <div class="todo"><span>Panel-Helligkeit</span><span class="badge">GEPLANT</span></div>
  <div class="todo"><span>Funktest / RSSI-Anzeige</span><span class="badge">GEPLANT</span></div>
</div>

<script>
let curMax=null,curAh=true;
function toggleAh(){cmd('AUTOHORN '+(curAh?0:1));}
function setBg(v){localStorage.setItem('bg',v);renderBg();}
function renderBg(){
  const v=localStorage.getItem('bg')||'m';
  ['m','w','off'].forEach(k=>{
    document.getElementById('bg_'+k).className='chip'+(k===v?' sel':'');});
}
renderBg();
async function cmd(c){
  try{await fetch('/cmd?c='+encodeURIComponent(c),{method:'POST'});}catch(e){}
  pollStatus();
}
function setCustom(){
  const v=parseInt(document.getElementById('maxin').value);
  if(v>=1&&v<=99)cmd('SETMAX '+v);
}
async function pollStatus(){
  try{
    const r=await fetch('/status',{cache:'no-store'});
    const j=await r.json();
    curMax=j.max;
    document.getElementById('mSixes').className='mbtn'+(j.max===30?' sel':'');
    document.getElementById('mField').className='mbtn'+(j.max===80?' sel':'');
    document.getElementById('curmax').textContent='aktuell: '+j.max+' s';
    curAh=!!j.autohorn;
    const ab=document.getElementById('ahBtn');
    ab.textContent=curAh?'EIN':'AUS';
    ab.className='chip'+(curAh?' sel':'');
  }catch(e){}
}
async function pollInfo(){
  try{
    const r=await fetch('/info',{cache:'no-store'});
    const j=await r.json();
    document.getElementById('iSsid').textContent=j.ssid;
    document.getElementById('iIp').textContent=j.ip;
    document.getElementById('iSta').textContent=j.stations;
    const lo=document.getElementById('iLora');
    lo.textContent=j.lora?'OK':'FEHLER';
    lo.className=j.lora?'ok':'err';
    document.getElementById('iTx').textContent=j.tx;
    const u=j.uptime;
    document.getElementById('iUp').textContent=
      Math.floor(u/3600)+'h '+Math.floor(u%3600/60)+'m '+(u%60)+'s';
    document.getElementById('iFw').textContent=j.fw;
  }catch(e){}
}
setInterval(pollStatus,1000);pollStatus();
setInterval(pollInfo,2000);pollInfo();
</script></body></html>)HTML";

const char MANIFEST_JSON[] PROGMEM = R"JSON({
"name":"Shotclock","short_name":"Shotclock","start_url":"/","scope":"/",
"display":"fullscreen","orientation":"any",
"background_color":"#000000","theme_color":"#000000",
"icons":[{"src":"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Crect width='100' height='100' rx='20' fill='%23000'/%3E%3Ctext x='50' y='68' font-size='52' font-weight='bold' text-anchor='middle' fill='%23ff2f2f' font-family='sans-serif'%3E30%3C/text%3E%3C/svg%3E","sizes":"any","type":"image/svg+xml"}]
})JSON";

// ---------------- HTTP-Handler ----------------
void buildStatus(char* buf, size_t n) {
    portENTER_CRITICAL(&clkMux);
    int sec = clkSeconds, mx = clkMax;
    bool run = (clkState == ST_START);
    unsigned long nt = nextTickMs;
    portEXIT_CRITICAL(&clkMux);

    // Exakte Restzeit in ms -> Browser tickt phasengenau auf der Controller-Uhr
    long remms;
    if (run) {
        long toNext = (long)(nt - millis());
        if (toNext < 0)    toNext = 0;
        if (toNext > 1000 + TX_PHASE_MS) toNext = 1000 + TX_PHASE_MS;
        remms = (long)(sec - 1) * 1000L + toNext;
        if (remms < 0) remms = 0;
    } else {
        remms = (long)sec * 1000L;
    }
    snprintf(buf, n,
             "{\"state\":\"%s\",\"seconds\":%d,\"max\":%d,\"remms\":%ld,"
             "\"autohorn\":%s,\"connected\":true}",
             run ? "START" : "STOP", sec, mx, remms, autoHorn ? "true" : "false");
}

void handleStatus(AsyncWebServerRequest* req) {
    char buf[160];
    buildStatus(buf, sizeof(buf));
    AsyncWebServerResponse* res = req->beginResponse(200, "application/json", buf);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

void handleCmd(AsyncWebServerRequest* req) {
    if (req->hasParam("c")) {
        String c = req->getParam("c")->value();
        c.toUpperCase();
        if (c.startsWith("SETMAX"))      cmdSetMax(c.substring(6).toInt());
        else if (c.startsWith("SETSEC")) cmdSetSec(c.substring(6).toInt());
        else if (c == "START")           cmdStart();
        else if (c == "STOP")            cmdStop();
        else if (c == "RESET")           cmdReset();
        else if (c == "HORN")            cmdHorn(HORN_MANUAL_MS);
        else if (c.startsWith("AUTOHORN")) autoHorn = (c.substring(8).toInt() != 0);
    }
    handleStatus(req);
}

void handleInfo(AsyncWebServerRequest* req) {
    char buf[192];
    unsigned long up = millis() / 1000UL;
    snprintf(buf, sizeof(buf),
             "{\"ssid\":\"%s\",\"ip\":\"192.168.4.1\",\"stations\":%d,"
             "\"lora\":%s,\"tx\":%d,\"uptime\":%lu,\"fw\":\"%s\"}",
             AP_SSID, WiFi.softAPgetStationNum(),
             loraOk ? "true" : "false", txCount, up, FW_VERSION);
    AsyncWebServerResponse* res = req->beginResponse(200, "application/json", buf);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

// ---------------- OLED ----------------
void drawOled() {
    if (!oledOk) return;
    portENTER_CRITICAL(&clkMux);
    int sec = clkSeconds, mx = clkMax;
    bool run = (clkState == ST_START);
    portEXIT_CRITICAL(&clkMux);

    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 9,  "SHOTCLOCK CTRL");
    oled.drawHLine(0, 11, 128);
    char l[24];
    snprintf(l, sizeof(l), "WLAN: %s", AP_SSID);
    oled.drawStr(0, 24, l);
    oled.drawStr(0, 35, "IP: 192.168.4.1");
    snprintf(l, sizeof(l), "Tablets: %d %s", WiFi.softAPgetStationNum(),
             loraOk ? "" : "LORA-ERR!");
    oled.drawStr(0, 46, l);
    const char* mode = (mx == 30) ? "SIXES" : (mx == 80) ? "FIELD" : "CUSTOM";
    snprintf(l, sizeof(l), "%2d s %s %s", sec, run ? "RUN " : "STOP", mode);
    oled.setFont(u8g2_font_9x18B_tf);
    oled.drawStr(0, 63, l);
    oled.sendBuffer();
}

// ---------------- Setup / Loop ----------------
void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    txQueue = xQueueCreate(16, sizeof(TxJob));

    // OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    oledOk = oled.begin();

    // LoRa
    spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                         LORA_SYNC, LORA_POWER, LORA_PREAMBLE, 1.8f);
    loraOk = (st == RADIOLIB_ERR_NONE);
    if (loraOk) radio.setDio2AsRfSwitch();
    Serial.printf("[CTRL] LoRa %s (%d)\n", loraOk ? "OK" : "FEHLER", st);

    // WLAN Access Point
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1),
                      IPAddress(255,255,255,0));
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[CTRL] AP '%s' auf %s\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str());

    // DNS: alle Anfragen auf uns (Captive Portal)
    dns.start(53, "*", WiFi.softAPIP());

    // Async-Webserver
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res =
            req->beginResponse_P(200, "text/html", PAGE_HTML);
        res->addHeader("Cache-Control", "no-cache");
        req->send(res);
    });
    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res =
            req->beginResponse_P(200, "text/html", SETTINGS_HTML);
        res->addHeader("Cache-Control", "no-cache");
        req->send(res);
    });
    server.on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "application/manifest+json", MANIFEST_JSON);
    });
    server.on("/bg_m.jpg", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res =
            req->beginResponse_P(200, "image/jpeg", BG_M, BG_M_LEN);
        res->addHeader("Cache-Control", "max-age=86400");
        req->send(res);
    });
    server.on("/bg_w.jpg", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res =
            req->beginResponse_P(200, "image/jpeg", BG_W, BG_W_LEN);
        res->addHeader("Cache-Control", "max-age=86400");
        req->send(res);
    });
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/info",   HTTP_GET, handleInfo);
    server.on("/cmd", HTTP_POST, handleCmd);
    server.onNotFound([](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    });
    server.begin();
    Serial.println("[CTRL] Async-Webserver bereit. Warte auf Tablet...");

    drawOled();
}

unsigned long lastOledMs = 0;

void loop() {
    dns.processNextRequest();
    tickTimer();
    processLoraQueue();

    unsigned long now = millis();
    if (now - lastOledMs >= 1000) {
        lastOledMs = now;
        drawOled();
    }
    delay(2);
}
