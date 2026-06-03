/* =====================================================================
 *  Sleep Apnea Screener — integrated TEST firmware
 *  Board: Seeed Studio XIAO ESP32-S3   (Arduino-ESP32 core 3.x)
 *
 *  Merges: MAX30102 heart rate (I2C) + thermistor breath detector
 *          + MAX9814 mic snore/gasp detector + LiPo battery monitor,
 *          and streams everything live to your phone over WiFi.
 *
 *  The phone "app" is a web page this device serves. The XIAO joins
 *  your phone's hotspot, then you open the dashboard in the phone's
 *  browser. Heartbeat is pushed in real time over a WebSocket; the
 *  slower channels update a few times a second.
 *
 *  ---- LIBRARIES TO INSTALL (Library Manager) ----
 *    - "SparkFun MAX3010x Pulse and Proximity Sensor Library"
 *    - "ESP Async WebServer"  by ESP32Async   (NOT the old me-no-dev one)
 *    - "Async TCP"            by ESP32Async
 *    (the ESP32Async forks are the ones that compile on core 3.x)
 *
 *  ---- PINS (your map) ----
 *    Mic  (MAX9814 OUT)  -> A8  = GPIO7  (ADC1)
 *    Thermistor node     -> A9  = GPIO8  (ADC1)
 *    Battery divider tap -> A10 = GPIO9  (ADC1)
 *    I2C  SDA/SCL        -> D4/D5 = GPIO5/GPIO6  (MAX30102)
 *    User LED            -> GPIO21 (active LOW) -- proof-of-life blinker
 *
 *  ---- WIRING NOTES ----
 *    Thermistor:  3V3 --[10k fixed]--+-- A9 --[10k NTC]-- GND   (now 3V3!)
 *    Battery:     BAT+ --[100k]--+-- A10 --[100k]-- GND  (+0.1uF A10->GND)
 *    Mic:         VDD->3V3, GND->GND, A/R->GND, GAIN->GND(50dB), OUT->A8
 *
 *  NOT a medical device. The apnea/snore/gasp flags are crude heuristics
 *  meant to prove the data path end-to-end, not to diagnose anything.
 * ===================================================================== */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <math.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "secrets.h"          // provides: const char* ssid; const char* password;

// ----------------------------------------------------------------------
//  Config
// ----------------------------------------------------------------------
#define ENABLE_LOW_BATT_SLEEP 0      // 0 for bench testing (don't nap mid-test)
#define LOW_BATTERY_THRESHOLD 3.30   // volts
#define TIME_TO_SLEEP_SEC     1800
#define uS_TO_S_FACTOR        1000000ULL

// Pins
const int MIC_PIN   = 7;   // A8  GPIO7  ADC1_CH6
const int THERM_PIN = 8;   // A9  GPIO8  ADC1_CH7
const int VBAT_PIN  = 9;   // A10 GPIO9  ADC1_CH8

#ifndef LED_BUILTIN
#define LED_BUILTIN 21
#endif
const bool LED_ACTIVE_LOW = true;    // XIAO ESP32-S3 user LED is active LOW
inline void ledWrite(bool on) { digitalWrite(LED_BUILTIN, (on ^ LED_ACTIVE_LOW) ? HIGH : LOW); }

// ----------------------------------------------------------------------
//  Web server + WebSocket
// ----------------------------------------------------------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ----------------------------------------------------------------------
//  Heart rate (MAX30102) — from your working sketch
// ----------------------------------------------------------------------
MAX30105 sensor;
const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE];
byte  rateSpot = 0;
long  lastBeat = 0;
float beatsPerMinute = 0;
int   beatAvg = 0;
long  irValue = 0;
float dieTempC = 0;
bool  fingerPresent = false;
bool  maxOk = false;

// ----------------------------------------------------------------------
//  Thermistor breath detector (ported from bioe_sensor.ino v4)
// ----------------------------------------------------------------------
#define SERIES_RESISTOR  10000.0
#define NOMINAL_RES      10000.0
#define NOMINAL_TEMP     25.0
#define B_COEFFICIENT    3950.0
#define THERM_VSUP_MV    3300.0     // divider now fed from 3V3
#define THERM_SAMPLES    5
#define BREATH_PERIOD_MS 100        // 10 Hz

#define SLOPE_ARM          -0.5
#define DROP_COMMIT         0.8
#define ARM_TIMEOUT_MS      1200
#define SLOPE_EXHALE_EXIT  -0.25
#define DEV_SETTLED          0.20
#define RECOVERY_TIMEOUT_MS  3000
#define BASELINE_ALPHA       0.01
#define RESEED_ALPHA         0.05
#define SIGNAL_ALPHA         0.35

enum BreathState { IDLE, EXHALE, RECOVERY, INHALE_REST };
float tempFiltered = NAN, tempPrev = NAN, baseline = NAN, breathDeviation = 0, breathSlope = 0;
BreathState breathState = IDLE;
unsigned long lastBreathSample = 0;
unsigned long exhaleCount = 0;
bool  armed = false;
float armPeakTemp = 0;
unsigned long armTime = 0, recoveryStart = 0;

// respiration + apnea
unsigned long lastExhaleMs = 0;
float respRate = 0;                  // breaths/min (from exhale spacing)
#define APNEA_GAP_MS 10000UL         // no exhale this long -> flag apnea (crude)
bool  apneaFlagged = false;
unsigned long apneaCount = 0;

// ----------------------------------------------------------------------
//  Mic snore/gasp detector (MAX9814 envelope)
// ----------------------------------------------------------------------
const uint32_t MIC_PERIOD_US = 500;          // ~2 kHz sampling
const uint32_t MIC_WIN_MS    = 50;
const uint32_t MIC_WIN_N     = (1000000UL / MIC_PERIOD_US) * MIC_WIN_MS / 1000; // ~100
const float    MIC_THRESH_MULT = 4.0;
const float    MIC_MIN_THRESH  = 120.0;
const float    MIC_HYST        = 0.55;
const uint32_t MIC_MIN_EVENT_MS = 150;
const uint32_t MIC_REARM_MS     = 350;

float    micDcBias = 1950.0;
double   micSumSq = 0, micSumLin = 0;
uint32_t micCount = 0;
float    micRms = 0;
float    micEnterThresh = MIC_MIN_THRESH, micExitThresh = MIC_MIN_THRESH * MIC_HYST;
bool     micLoud = false;
uint32_t micLoudStart = 0, micLastEnd = 0;
float    micPeak = 0;
bool     micCounted = false;
unsigned long soundEventCount = 0;
unsigned long micSampleUs = 0;

// ----------------------------------------------------------------------
//  Battery
// ----------------------------------------------------------------------
float vbat = 0;
int   batPct = 0;
const float BAT_DIVIDER = 2.0;       // 100k / 100k

// ----------------------------------------------------------------------
//  Scheduler timestamps
// ----------------------------------------------------------------------
unsigned long lastHrTempMs = 0, lastBattMs = 0, lastTelemMs = 0, lastWsCleanMs = 0;

// ======================================================================
//  Dashboard (served to the phone)
// ======================================================================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Apnea Monitor</title>
<style>
  :root{--bg:#0c0f14;--card:#151b24;--line:#26303d;--txt:#e7eef6;--mut:#8a99ab;
        --ok:#37d39b;--warn:#ffcc55;--bad:#ff5d6c;--accent:#5aa9ff;}
  *{box-sizing:border-box} body{margin:0;font:15px/1.4 -apple-system,system-ui,sans-serif;
    background:var(--bg);color:var(--txt);padding:14px;max-width:560px;margin:auto}
  h1{font-size:18px;margin:0 0 2px} .sub{color:var(--mut);font-size:12px;margin-bottom:12px}
  .dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--bad);
    margin-right:6px;vertical-align:middle}
  .dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px}
  .card.wide{grid-column:1/-1}
  .label{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.04em}
  .big{font-size:34px;font-weight:700;line-height:1} .unit{font-size:14px;color:var(--mut)}
  .row{display:flex;justify-content:space-between;align-items:baseline;margin-top:6px}
  .small{font-size:13px;color:var(--mut)}
  .heart{font-size:26px;transition:transform .08s ease} .beat{transform:scale(1.5)}
  .bar{height:8px;border-radius:6px;background:#222c38;overflow:hidden;margin-top:8px}
  .fill{height:100%;width:0;background:var(--accent);transition:width .2s}
  .pill{display:inline-block;padding:2px 9px;border-radius:999px;font-size:12px;
    background:#222c38;color:var(--txt)}
  .pill.ex{background:rgba(90,169,255,.2);color:var(--accent)}
  #log{max-height:170px;overflow:auto;margin-top:8px;font-size:13px}
  #log div{padding:5px 0;border-bottom:1px solid var(--line)}
  #log .ap{color:var(--bad);font-weight:600}
  #log .gp{color:var(--warn)} canvas{width:100%;height:48px;display:block;margin-top:8px}
</style></head><body>
<h1>Sleep Apnea Monitor <span style="font-size:12px;color:var(--mut)">— test rig</span></h1>
<div class="sub"><span id="dot" class="dot"></span><span id="status">connecting…</span>
  &nbsp;·&nbsp;up <span id="up">0</span>s</div>

<div class="grid">
  <div class="card">
    <div class="label">Heart rate <span id="heart" class="heart">♥</span></div>
    <div class="row"><span class="big" id="bpm">--</span><span class="unit">bpm now</span></div>
    <div class="row small"><span>avg <b id="avg">--</b></span><span id="finger">no finger</span></div>
    <canvas id="hrcv" width="260" height="48"></canvas>
  </div>

  <div class="card">
    <div class="label">Breathing</div>
    <div class="row"><span class="big" id="rr">--</span><span class="unit">breaths/min</span></div>
    <div class="row small"><span id="bstate" class="pill">--</span>
      <span>exhales <b id="exh">0</b></span></div>
    <div class="row small"><span>temp <b id="tmp">--</b>°C</span>
      <span>dev <b id="dev">--</b></span></div>
  </div>

  <div class="card">
    <div class="label">Sound level</div>
    <div class="row"><span class="big" id="snd">0</span><span class="unit">rms</span></div>
    <div class="bar"><div class="fill" id="sndbar"></div></div>
    <div class="row small"><span>events <b id="snc">0</b></span><span id="lastsnd">—</span></div>
  </div>

  <div class="card">
    <div class="label">Battery</div>
    <div class="row"><span class="big" id="vb">--</span><span class="unit">V</span></div>
    <div class="bar"><div class="fill" id="vbbar" style="background:var(--ok)"></div></div>
    <div class="row small"><span id="vbpct">--%</span></div>
  </div>

  <div class="card wide">
    <div class="label">Apnea events <span id="apc" class="pill">0</span></div>
    <div id="log"><div class="small">waiting for data…</div></div>
  </div>
</div>

<script>
const $=id=>document.getElementById(id);
let ws, hr=[], firstLog=true;

function connect(){
  ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onopen =()=>{ $('dot').classList.add('on'); $('status').textContent='connected'; };
  ws.onclose=()=>{ $('dot').classList.remove('on'); $('status').textContent='reconnecting…';
                   setTimeout(connect,1500); };
  ws.onmessage=e=>{ let m; try{m=JSON.parse(e.data)}catch(_){return} handle(m); };
}
function handle(m){
  if(m.type==='beat'){ pulse(); $('bpm').textContent=Math.round(m.bpm);
                       $('avg').textContent=m.avg; pushHr(m.bpm); return; }
  if(m.type==='event'){ logEvent(m); return; }
  if(m.type==='telemetry'){
    $('up').textContent=m.uptime;
    $('avg').textContent=m.hrAvg; if(m.hr>0)$('bpm').textContent=Math.round(m.hr);
    $('finger').textContent=m.finger?'finger ✔':'no finger';
    $('rr').textContent=m.rr>0?m.rr.toFixed(1):'--';
    $('bstate').textContent=m.breath; $('bstate').className='pill'+(m.breath==='EXHALE'?' ex':'');
    $('exh').textContent=m.exhales; $('tmp').textContent=m.tempC.toFixed(2);
    $('dev').textContent=m.dev.toFixed(2);
    $('snd').textContent=Math.round(m.micRms);
    $('sndbar').style.width=Math.min(100,m.micRms/12)+'%';
    $('snc').textContent=m.sounds;
    $('vb').textContent=m.vbat.toFixed(2);
    $('vbpct').textContent=m.batPct+'%'; $('vbbar').style.width=m.batPct+'%';
    $('vbbar').style.background=m.batPct<15?'var(--bad)':m.batPct<40?'var(--warn)':'var(--ok)';
    $('apc').textContent=m.apnea;
    if(m.hr>0) pushHr(m.hr);
  }
}
function pulse(){ const h=$('heart'); h.classList.add('beat'); setTimeout(()=>h.classList.remove('beat'),90); }
function logEvent(m){
  if(firstLog){ $('log').innerHTML=''; firstLog=false; }
  const d=document.createElement('div');
  const cls=m.kind==='apnea'?'ap':(m.kind==='gasp'?'gp':'');
  d.className=cls;
  let txt=m.kind.toUpperCase()+' @ '+m.t+'s';
  if(m.kind==='apnea') txt+='  (gap '+m.gap+'s)';
  else if(m.dur!=null) txt+='  ('+m.dur+'ms, peak '+m.peak+')';
  d.textContent=txt;
  $('log').prepend(d);
}
function pushHr(v){ hr.push(v); if(hr.length>60)hr.shift(); drawHr(); }
function drawHr(){
  const c=$('hrcv'),x=c.getContext('2d'),w=c.width,h=c.height; x.clearRect(0,0,w,h);
  if(hr.length<2)return; const mn=Math.min(...hr)-3,mx=Math.max(...hr)+3,r=mx-mn||1;
  x.strokeStyle='#5aa9ff'; x.lineWidth=2; x.beginPath();
  hr.forEach((v,i)=>{ const px=i/(hr.length-1)*w, py=h-(v-mn)/r*h;
    i?x.lineTo(px,py):x.moveTo(px,py); }); x.stroke();
}
connect();
</script></body></html>
)HTML";

// ======================================================================
//  WebSocket helpers
// ======================================================================
void wsBroadcast(const char* json) {
  if (ws.count() > 0) ws.textAll(json);
}

void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS client %u connected\n", c->id());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS client %u left\n", c->id());
  }
}

void sendBeat() {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"type\":\"beat\",\"bpm\":%.1f,\"avg\":%d}",
           beatsPerMinute, beatAvg);
  wsBroadcast(buf);
}

void sendEvent(const char* kind, long arg1, long arg2) {
  char buf[160];
  unsigned long t = millis() / 1000;
  if (strcmp(kind, "apnea") == 0)
    snprintf(buf, sizeof(buf),
             "{\"type\":\"event\",\"kind\":\"apnea\",\"t\":%lu,\"gap\":%ld}", t, arg1);
  else
    snprintf(buf, sizeof(buf),
             "{\"type\":\"event\",\"kind\":\"%s\",\"t\":%lu,\"dur\":%ld,\"peak\":%ld}",
             kind, t, arg1, arg2);
  wsBroadcast(buf);
}

void sendTelemetry() {
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"type\":\"telemetry\",\"uptime\":%lu,"
    "\"hr\":%.1f,\"hrAvg\":%d,\"finger\":%s,\"ir\":%ld,"
    "\"rr\":%.1f,\"breath\":\"%s\",\"exhales\":%lu,\"tempC\":%.2f,\"dev\":%.2f,"
    "\"micRms\":%.0f,\"sounds\":%lu,"
    "\"vbat\":%.2f,\"batPct\":%d,\"apnea\":%lu}",
    millis() / 1000,
    beatsPerMinute, beatAvg, fingerPresent ? "true" : "false", irValue,
    respRate, breathStateName(), exhaleCount, tempFiltered, breathDeviation,
    micRms, soundEventCount,
    vbat, batPct, apneaCount);
  wsBroadcast(buf);
}

const char* breathStateName() {
  switch (breathState) {
    case EXHALE:      return "EXHALE";
    case RECOVERY:    return "RECOVERY";
    case INHALE_REST: return "REST";
    default:          return "IDLE";
  }
}

// ======================================================================
//  Sensors
// ======================================================================
float readTempC() {
  uint32_t acc = 0;
  for (int i = 0; i < THERM_SAMPLES; i++) acc += analogReadMilliVolts(THERM_PIN);
  float mv = acc / (float)THERM_SAMPLES;
  if (mv >= THERM_VSUP_MV) mv = THERM_VSUP_MV - 1;        // guard divide-by-zero
  float resistance = SERIES_RESISTOR * mv / (THERM_VSUP_MV - mv);
  float s = log(resistance / NOMINAL_RES) / B_COEFFICIENT
            + 1.0 / (NOMINAL_TEMP + 273.15);
  return 1.0 / s - 273.15;
}

void updateBreath() {
  unsigned long now = millis();
  float dt = (now - lastBreathSample) / 1000.0;
  lastBreathSample = now;

  float raw = readTempC();
  tempFiltered = SIGNAL_ALPHA * raw + (1.0 - SIGNAL_ALPHA) * tempFiltered;
  breathSlope = (tempFiltered - tempPrev) / dt;
  tempPrev = tempFiltered;

  bool atRest = (breathState == IDLE || breathState == INHALE_REST);
  if (atRest) {
    baseline = BASELINE_ALPHA * tempFiltered + (1.0 - BASELINE_ALPHA) * baseline;
    if (fabs(tempFiltered - baseline) > DEV_SETTLED)
      baseline = RESEED_ALPHA * tempFiltered + (1.0 - RESEED_ALPHA) * baseline;
  }
  breathDeviation = tempFiltered - baseline;

  // exhale-candidate arming
  if (!armed) {
    if (breathSlope <= SLOPE_ARM) {
      armed = true;
      armPeakTemp = tempPrev > tempFiltered ? tempPrev : tempFiltered;
      armTime = now;
    }
  } else {
    if (tempFiltered > armPeakTemp) armPeakTemp = tempFiltered;
    if (now - armTime > ARM_TIMEOUT_MS || breathSlope > 0.2) armed = false;
  }
  bool exhaleConfirmed = armed && (armPeakTemp - tempFiltered) >= DROP_COMMIT;

  switch (breathState) {
    case IDLE:
    case INHALE_REST:
      breathState = INHALE_REST;
      if (exhaleConfirmed) { registerExhale(now); armed = false; }
      break;
    case EXHALE:
      if (breathSlope > SLOPE_EXHALE_EXIT) { breathState = RECOVERY; recoveryStart = now; armed = false; }
      break;
    case RECOVERY:
      if (exhaleConfirmed) { registerExhale(now); armed = false; }
      else if (breathDeviation >= -DEV_SETTLED) breathState = INHALE_REST;
      else if (now - recoveryStart > RECOVERY_TIMEOUT_MS) { breathState = INHALE_REST; baseline = tempFiltered; }
      break;
  }

  // apnea heuristic: long gap with no exhale, then re-arm on next breath
  if (lastExhaleMs > 0 && (now - lastExhaleMs) > APNEA_GAP_MS && !apneaFlagged) {
    apneaFlagged = true;
    apneaCount++;
    sendEvent("apnea", (long)((now - lastExhaleMs) / 1000), 0);
  }
}

void registerExhale(unsigned long now) {
  breathState = EXHALE;
  exhaleCount++;
  if (lastExhaleMs > 0) {
    float interval = (now - lastExhaleMs) / 1000.0;
    if (interval > 0.5 && interval < 20.0) {
      float inst = 60.0 / interval;
      respRate = (respRate == 0) ? inst : 0.4 * inst + 0.6 * respRate;
    }
  }
  lastExhaleMs = now;
  apneaFlagged = false;
}

void calibrateMic() {
  // ~2 s of ambient -> noise floor -> event thresholds
  const int windows = 2000 / MIC_WIN_MS;
  uint32_t accBias = 0;
  for (int i = 0; i < 2000; i++) { accBias += analogRead(MIC_PIN); delayMicroseconds(150); }
  micDcBias = accBias / 2000.0;

  double rmsAcc = 0; int got = 0;
  for (int w = 0; w < windows; w++) {
    double ss = 0;
    for (uint32_t i = 0; i < MIC_WIN_N; i++) {
      float x = analogRead(MIC_PIN) - micDcBias;
      ss += (double)x * x;
      delayMicroseconds(MIC_PERIOD_US);
    }
    rmsAcc += sqrt(ss / MIC_WIN_N); got++;
  }
  float floorRms = got ? rmsAcc / got : MIC_MIN_THRESH;
  micEnterThresh = max(floorRms * MIC_THRESH_MULT, MIC_MIN_THRESH);
  micExitThresh  = micEnterThresh * MIC_HYST;
  Serial.printf("Mic floor=%.1f enter=%.1f exit=%.1f\n", floorRms, micEnterThresh, micExitThresh);
}

void sampleMic() {
  float x = analogRead(MIC_PIN) - micDcBias;
  micSumSq += (double)x * x;
  micSumLin += (analogRead(MIC_PIN));   // for slow DC tracking
  micCount++;
  if (micCount < MIC_WIN_N) return;

  micRms = sqrt(micSumSq / micCount);
  micDcBias = 0.995 * micDcBias + 0.005 * (micSumLin / micCount);
  micSumSq = 0; micSumLin = 0; micCount = 0;

  unsigned long now = millis();
  if (!micLoud) {
    if (micRms > micEnterThresh && (now - micLastEnd) >= MIC_REARM_MS) {
      micLoud = true; micLoudStart = now; micPeak = micRms; micCounted = false;
    }
  } else {
    if (micRms > micPeak) micPeak = micRms;
    if (!micCounted && (now - micLoudStart) >= MIC_MIN_EVENT_MS) {
      micCounted = true; soundEventCount++;
    }
    if (micRms < micExitThresh) {
      unsigned long dur = now - micLoudStart;
      micLoud = false; micLastEnd = now;
      if (micCounted) {
        const char* kind = (dur < 500) ? "gasp" : "snore";   // crude split
        sendEvent(kind, (long)dur, (long)micPeak);
      }
    }
  }
}

void pollHeart() {
  irValue = sensor.getIR();
  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    float bpm = 60.0 / (delta / 1000.0);
    if (bpm > 20 && bpm < 255) {
      beatsPerMinute = bpm;
      rates[rateSpot++] = (byte)bpm; rateSpot %= RATE_SIZE;
      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
      sendBeat();                      // <-- real-time push to the phone
    }
  }
}

float readBatteryVoltage() {
  const int N = 16; uint32_t acc = 0;
  for (int i = 0; i < N; i++) acc += analogReadMilliVolts(VBAT_PIN);
  return (acc / (float)N) / 1000.0 * BAT_DIVIDER;
}

int batteryPercent(float v) {
  struct { float v; int p; } lut[] = {
    {4.20,100},{4.10,90},{4.00,80},{3.90,65},{3.80,55},
    {3.70,40},{3.60,25},{3.50,15},{3.40,8},{3.30,5},{3.00,0}};
  const int n = sizeof(lut) / sizeof(lut[0]);
  if (v >= lut[0].v) return 100;
  if (v <= lut[n-1].v) return 0;
  for (int i = 0; i < n-1; i++)
    if (v <= lut[i].v && v > lut[i+1].v) {
      float f = (v - lut[i+1].v) / (lut[i].v - lut[i+1].v);
      return lut[i+1].p + f * (lut[i].p - lut[i+1].p);
    }
  return 0;
}

void maybeSleepLowBattery() {
#if ENABLE_LOW_BATT_SLEEP
  if (vbat <= LOW_BATTERY_THRESHOLD && vbat > 1.0) {   // >1V guards an unread pin
    Serial.println("Battery low -> deep sleep.");
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_SEC * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
#endif
}

// ======================================================================
//  LED proof-of-life (non-blocking)
// ======================================================================
void updateLed(bool wifiUp) {
  static unsigned long t0 = 0; static int phase = 0;
  unsigned long now = millis();
  if (!wifiUp) {                       // fast blink while offline / connecting
    if (now - t0 > 120) { t0 = now; phase ^= 1; ledWrite(phase); }
    return;
  }
  // "alive" double-blink every ~2.5 s when connected
  unsigned long c = now % 2500;
  bool on = (c < 90) || (c > 200 && c < 290);
  ledWrite(on);
}

// ======================================================================
//  Setup / loop
// ======================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);          // credentials live in secrets.h
  Serial.print("Joining WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    updateLed(false); Serial.print("."); delay(150);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("apnea")) Serial.println("Open http://apnea.local on your phone");
  } else {
    Serial.println("\nWiFi failed — running offline; will retry.");
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  ledWrite(true);                      // instant proof of life on power-up
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Apnea monitor (test) ===");

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);      // full-scale ~2.6V for all 3 ADC1 pins

  // --- MAX30102 ---
  Wire.begin();                        // XIAO S3 default SDA=GPIO5 SCL=GPIO6
  maxOk = sensor.begin(Wire, I2C_SPEED_FAST);
  if (maxOk) {
    sensor.setup(0x1F, 4, 2, 100, 411, 4096);
    sensor.enableDIETEMPRDY();
    Serial.println("MAX30102 ready.");
  } else {
    Serial.println("WARN: MAX30102 not found — HR will read 0.");
  }

  // --- mic baseline (stay quiet ~2s) ---
  Serial.println("Calibrating mic — keep quiet...");
  calibrateMic();

  // --- thermistor seed ---
  float t = readTempC();
  tempFiltered = tempPrev = baseline = t;
  lastBreathSample = millis();

  // --- battery ---
  vbat = readBatteryVoltage(); batPct = batteryPercent(vbat);
  Serial.printf("Battery: %.2f V (%d%%)\n", vbat, batPct);

  connectWiFi();

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html", INDEX_HTML);
  });
  server.begin();
  Serial.println("HTTP server up.\n");
}

void loop() {
  unsigned long now = millis();
  uint32_t nowUs = micros();

  // 1) Heart: poll fast for accurate beat timing (pushes beats live)
  if (maxOk) pollHeart();

  // 2) Mic: sample at ~2 kHz, compute envelope + event detection
  if ((uint32_t)(nowUs - micSampleUs) >= MIC_PERIOD_US) {
    micSampleUs = nowUs;
    sampleMic();
  }

  // 3) Breathing: 10 Hz
  if (now - lastBreathSample >= BREATH_PERIOD_MS) updateBreath();

  // 4) HR die-temp + finger status: 1 Hz (temp read is slow)
  if (now - lastHrTempMs >= 1000) {
    lastHrTempMs = now;
    if (maxOk) { dieTempC = sensor.readTemperature(); }
    fingerPresent = irValue > 50000;
  }

  // 5) Battery: every 10 s
  if (now - lastBattMs >= 10000) {
    lastBattMs = now;
    vbat = readBatteryVoltage(); batPct = batteryPercent(vbat);
    maybeSleepLowBattery();
  }

  // 6) Telemetry push: 4 Hz
  if (now - lastTelemMs >= 250) { lastTelemMs = now; sendTelemetry(); }

  // 7) Housekeeping
  if (now - lastWsCleanMs >= 500) { lastWsCleanMs = now; ws.cleanupClients(); }
  if (WiFi.status() != WL_CONNECTED && now - lastWsCleanMs == 0) { /* see below */ }

  updateLed(WiFi.status() == WL_CONNECTED);
}
