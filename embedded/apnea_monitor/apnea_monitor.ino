/* =====================================================================
 *  COOL TEAM DETECTOR — Mask firmware
 *  Team: Cool Team
 *  Board: Seeed Studio XIAO ESP32-S3   (Arduino-ESP32 core 3.x)
 *
 *  WHAT THIS NODE DOES
 *    - SpO2 + heart rate  (MAX30102, Maxim ratio-of-ratios algorithm)
 *    - Oxygen Desaturation Index (ODI): SpO2 drop >=3% sustained >10 s
 *    - Airflow apnea       (NTC thermistor breath state machine)
 *    - Snore / gasp        (MAX9814 envelope)
 *    - Battery monitor     (LiPo divider)
 *  It is now the NETWORK HUB: it makes its OWN WiFi (SoftAP), so the
 *  phone (and later the haptic node) connect straight to it.  All
 *  history lives on the phone — this device only streams + counts.
 *
 *  CONNECT:  WiFi  "CoolTeamDetector"  /  pass "cooldemo123"
 *            then open  http://192.168.4.1
 *
 *  LIBRARIES (Library Manager)
 *    - "SparkFun MAX3010x Pulse and Proximity Sensor Library"
 *        (provides MAX30105.h, heartRate.h, spo2_algorithm.h)
 *    - "ESP Async WebServer" by ESP32Async   (+ "Async TCP" by ESP32Async)
 *
 *  PINS (unchanged)
 *    Mic OUT  -> A8/GPIO7   Thermistor -> A9/GPIO8   Battery -> A10/GPIO9
 *    I2C SDA/SCL -> D4/D5 = GPIO5/GPIO6  (MAX30102)   LED -> GPIO21 (active LOW)
 *
 *  NOT a medical device. Thresholds are screening heuristics.
 * ===================================================================== */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

// ----------------------------------------------------------------------
//  Access Point (this device IS the network now — no secrets.h needed)
// ----------------------------------------------------------------------
const char* AP_SSID = "CoolTeamDetector";
const char* AP_PASS = "cooldemo123";   // must be >= 8 chars

// ----------------------------------------------------------------------
//  Config
// ----------------------------------------------------------------------
#define LOW_BATTERY_THRESHOLD 3.30
#define ENABLE_LOW_BATT_SLEEP 0
#define TIME_TO_SLEEP_SEC     1800
#define uS_TO_S_FACTOR        1000000ULL

// Pins
const int MIC_PIN   = 7;
const int THERM_PIN = 8;
const int VBAT_PIN  = 9;

#ifndef LED_BUILTIN
#define LED_BUILTIN 21
#endif
const bool LED_ACTIVE_LOW = true;
inline void ledWrite(bool on) { digitalWrite(LED_BUILTIN, (on ^ LED_ACTIVE_LOW) ? HIGH : LOW); }

// ----------------------------------------------------------------------
//  Web server + WebSocket
// ----------------------------------------------------------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ----------------------------------------------------------------------
//  MAX30102  — SpO2 + HR via Maxim buffered algorithm
// ----------------------------------------------------------------------
MAX30105 sensor;
bool maxOk = false;

#define OX_N 100                       // algorithm needs a 100-sample window
uint32_t irBuf[OX_N], redBuf[OX_N];
int      oxFill = 0;                   // samples collected so far
int      oxNew  = 0;                   // new samples since last compute
int32_t  spo2 = 0;   int8_t validSPO2 = 0;
int32_t  maximHR = 0; int8_t validHR  = 0;
int      spo2Show = 0;                 // last valid SpO2 (%)
float    hrShow   = 0;                 // last valid HR (bpm)
long     irValue  = 0;
bool     fingerPresent = false;

// beat-pulse animation (cosmetic only; numbers come from Maxim)
long  lastBeat = 0;

// ----------------------------------------------------------------------
//  ODI — oxygen desaturation index
// ----------------------------------------------------------------------
#define DESAT_DROP       3.0           // % below baseline = a desaturation
#define DESAT_MIN_MS     10000UL       // must last this long (LOWER for a bench demo, e.g. 4000)
#define SPO2_BASE_ALPHA  0.02          // baseline tracking speed
float    spo2Baseline = NAN;
enum OdiState { OX_NORMAL, OX_DROP, OX_RECOVER };
OdiState odiState = OX_NORMAL;
unsigned long dropStart = 0;
unsigned long desatCount = 0;          // <- counter the phone reads
float    odi = 0;                      // events / hour

// ----------------------------------------------------------------------
//  Thermistor breath detector  (unchanged logic)
// ----------------------------------------------------------------------
#define SERIES_RESISTOR  10000.0
#define NOMINAL_RES      10000.0
#define NOMINAL_TEMP     25.0
#define B_COEFFICIENT    3950.0
#define THERM_VSUP_MV    3300.0
#define THERM_SAMPLES    5
#define BREATH_PERIOD_MS 100

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

unsigned long lastExhaleMs = 0;
float respRate = 0;
#define APNEA_GAP_MS 10000UL
bool  apneaFlagged = false;
unsigned long apneaCount = 0;

// ----------------------------------------------------------------------
//  Mic snore / gasp  (unchanged logic)
// ----------------------------------------------------------------------
const uint32_t MIC_PERIOD_US = 500;
const uint32_t MIC_WIN_MS    = 50;
const uint32_t MIC_WIN_N     = (1000000UL / MIC_PERIOD_US) * MIC_WIN_MS / 1000;
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
float vbat = 0;  int batPct = 0;
const float BAT_DIVIDER = 2.0;

// ----------------------------------------------------------------------
//  Scheduler
// ----------------------------------------------------------------------
unsigned long lastBattMs = 0, lastTelemMs = 0, lastWsCleanMs = 0;

const char* breathStateName();

// ======================================================================
//  Dashboard  — same dark theme, rebranded Cool Team Detector
// ======================================================================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sleep Apnea Detection</title>
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
  #log .ds{color:var(--accent);font-weight:600}
  #log .gp{color:var(--warn)} canvas{width:100%;height:48px;display:block;margin-top:8px}
</style></head><body>
<h1>Sleep Apnea Screener <span style="font-size:12px;color:var(--mut)">— by Cool Team</span></h1>
<div class="sub"><span id="dot" class="dot"></span><span id="status">connecting…</span>
  &nbsp;·&nbsp;up <span id="up">0</span>s</div>

<div class="grid">
  <div class="card wide">
    <div class="label">Blood oxygen (SpO₂)</div>
    <div class="row"><span class="big" id="spo2" style="color:var(--ok)">--</span><span class="unit">% SpO₂</span></div>
    <div class="row small">
      <span>ODI <b id="odi">0.0</b>/h</span>
      <span>desats <b id="desat">0</b></span>
      <span>baseline <b id="base">--</b>%</span>
    </div>
  </div>

  <div class="card">
    <div class="label">Heart rate <span id="heart" class="heart">♥</span></div>
    <div class="row"><span class="big" id="bpm">--</span><span class="unit">bpm</span></div>
    <div class="row small"><span id="finger">no finger</span></div>
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
    <div class="label">Events — apnea <span id="apc" class="pill">0</span></div>
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
  if(m.type==='beat'){ pulse(); return; }            // animation only
  if(m.type==='event'){ logEvent(m); return; }
  if(m.type==='telemetry'){
    $('up').textContent=m.uptime;
    if(m.spo2>0){ $('spo2').textContent=m.spo2; } else { $('spo2').textContent='--'; }
    $('odi').textContent=m.odi.toFixed(1);
    $('desat').textContent=m.desat;
    $('base').textContent=m.base>0?m.base:'--';
    if(m.hr>0){ $('bpm').textContent=Math.round(m.hr); pushHr(m.hr); }
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
    // spo2 color cue
    $('spo2').style.color = (m.spo2>0 && m.spo2<90)?'var(--bad)':(m.spo2<94?'var(--warn)':'var(--ok)');
  }
}
function pulse(){ const h=$('heart'); h.classList.add('beat'); setTimeout(()=>h.classList.remove('beat'),90); }
function logEvent(m){
  if(firstLog){ $('log').innerHTML=''; firstLog=false; }
  const d=document.createElement('div');
  const cls=m.kind==='apnea'?'ap':(m.kind==='desat'?'ds':(m.kind==='gasp'?'gp':''));
  d.className=cls;
  let txt=m.kind.toUpperCase()+' @ '+m.t+'s';
  if(m.kind==='apnea')      txt+='  (gap '+m.gap+'s)';
  else if(m.kind==='desat') txt+='  (−'+m.drop+'% → '+m.spo2+'%)';
  else if(m.dur!=null)      txt+='  ('+m.dur+'ms, peak '+m.peak+')';
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
void wsBroadcast(const char* json) { if (ws.count() > 0) ws.textAll(json); }

void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT)      Serial.printf("WS client %u connected\n", c->id());
  else if (type == WS_EVT_DISCONNECT) Serial.printf("WS client %u left\n", c->id());
}

void sendBeat() { wsBroadcast("{\"type\":\"beat\"}"); }

// generic event sender: apnea / desat / snore / gasp
void sendEvent(const char* kind, long a1, long a2) {
  char buf[160];
  unsigned long t = millis() / 1000;
  if (strcmp(kind, "apnea") == 0)
    snprintf(buf, sizeof(buf),
      "{\"type\":\"event\",\"kind\":\"apnea\",\"t\":%lu,\"gap\":%ld}", t, a1);
  else if (strcmp(kind, "desat") == 0)
    snprintf(buf, sizeof(buf),
      "{\"type\":\"event\",\"kind\":\"desat\",\"t\":%lu,\"drop\":%ld,\"spo2\":%ld}", t, a1, a2);
  else
    snprintf(buf, sizeof(buf),
      "{\"type\":\"event\",\"kind\":\"%s\",\"t\":%lu,\"dur\":%ld,\"peak\":%ld}", kind, t, a1, a2);
  wsBroadcast(buf);
}

void sendTelemetry() {
  char buf[640];
  int baseInt = isnan(spo2Baseline) ? 0 : (int)lround(spo2Baseline);
  snprintf(buf, sizeof(buf),
    "{\"type\":\"telemetry\",\"uptime\":%lu,"
    "\"spo2\":%d,\"odi\":%.1f,\"desat\":%lu,\"base\":%d,"
    "\"hr\":%.1f,\"finger\":%s,"
    "\"rr\":%.1f,\"breath\":\"%s\",\"exhales\":%lu,\"tempC\":%.2f,\"dev\":%.2f,"
    "\"micRms\":%.0f,\"sounds\":%lu,"
    "\"vbat\":%.2f,\"batPct\":%d,\"apnea\":%lu}",
    millis() / 1000,
    spo2Show, odi, desatCount, baseInt,
    hrShow, fingerPresent ? "true" : "false",
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
//  SpO2 / HR / ODI
// ======================================================================
void updateOdi(float s) {
  if (!fingerPresent) return;
  if (isnan(spo2Baseline)) spo2Baseline = s;
  unsigned long now = millis();
  switch (odiState) {
    case OX_NORMAL:
      spo2Baseline = (1.0 - SPO2_BASE_ALPHA) * spo2Baseline + SPO2_BASE_ALPHA * s;
      if (spo2Baseline - s >= DESAT_DROP) { odiState = OX_DROP; dropStart = now; }
      break;
    case OX_DROP:
      if (spo2Baseline - s < DESAT_DROP) {            // recovered too fast — not a desat
        odiState = OX_NORMAL;
      } else if (now - dropStart >= DESAT_MIN_MS) {   // sustained -> count one event
        desatCount++;
        sendEvent("desat", (long)lround(spo2Baseline - s), spo2Show);
        odiState = OX_RECOVER;                        // wait for recovery before re-arming
      }
      break;
    case OX_RECOVER:
      if (s >= spo2Baseline - 1.0) odiState = OX_NORMAL;
      break;
  }
  float hrs = millis() / 3600000.0;
  if (hrs > 1e-4) odi = desatCount / hrs;
}

void pollOximeter() {
  if (!maxOk) return;
  sensor.check();
  while (sensor.available()) {
    uint32_t red = sensor.getRed();
    uint32_t ir  = sensor.getIR();
    sensor.nextSample();
    irValue = ir;

    if (checkForBeat(ir)) { lastBeat = millis(); sendBeat(); }   // cosmetic pulse

    if (oxFill < OX_N) { redBuf[oxFill] = red; irBuf[oxFill] = ir; oxFill++; }
    else {
      memmove(redBuf, redBuf + 1, (OX_N - 1) * sizeof(uint32_t));
      memmove(irBuf,  irBuf  + 1, (OX_N - 1) * sizeof(uint32_t));
      redBuf[OX_N - 1] = red; irBuf[OX_N - 1] = ir;
      oxNew++;
    }
  }
  fingerPresent = irValue > 50000;

  if (oxFill >= OX_N && oxNew >= 25) {                // recompute ~1 Hz
    oxNew = 0;
    maxim_heart_rate_and_oxygen_saturation(irBuf, OX_N, redBuf,
                                           &spo2, &validSPO2, &maximHR, &validHR);
    if (validHR  && maximHR > 20 && maximHR < 255) hrShow   = maximHR;
    if (validSPO2 && spo2 > 0 && spo2 <= 100) { spo2Show = spo2; updateOdi((float)spo2); }
  }
}

// ======================================================================
//  Thermistor breath  (unchanged)
// ======================================================================
float readTempC() {
  uint32_t acc = 0;
  for (int i = 0; i < THERM_SAMPLES; i++) acc += analogReadMilliVolts(THERM_PIN);
  float mv = acc / (float)THERM_SAMPLES;
  if (mv >= THERM_VSUP_MV) mv = THERM_VSUP_MV - 1;
  float resistance = SERIES_RESISTOR * mv / (THERM_VSUP_MV - mv);
  float s = log(resistance / NOMINAL_RES) / B_COEFFICIENT + 1.0 / (NOMINAL_TEMP + 273.15);
  return 1.0 / s - 273.15;
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

  if (!armed) {
    if (breathSlope <= SLOPE_ARM) { armed = true; armPeakTemp = max(tempPrev, tempFiltered); armTime = now; }
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

  if (lastExhaleMs > 0 && (now - lastExhaleMs) > APNEA_GAP_MS && !apneaFlagged) {
    apneaFlagged = true;
    apneaCount++;
    sendEvent("apnea", (long)((now - lastExhaleMs) / 1000), 0);
  }
}

// ======================================================================
//  Mic  (unchanged)
// ======================================================================
void calibrateMic() {
  const int windows = 2000 / MIC_WIN_MS;
  uint32_t accBias = 0;
  for (int i = 0; i < 2000; i++) { accBias += analogRead(MIC_PIN); delayMicroseconds(150); }
  micDcBias = accBias / 2000.0;

  double rmsAcc = 0; int got = 0;
  for (int w = 0; w < windows; w++) {
    double ss = 0;
    for (uint32_t i = 0; i < MIC_WIN_N; i++) {
      float x = analogRead(MIC_PIN) - micDcBias; ss += (double)x * x; delayMicroseconds(MIC_PERIOD_US);
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
  micSumLin += analogRead(MIC_PIN);
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
    if (!micCounted && (now - micLoudStart) >= MIC_MIN_EVENT_MS) { micCounted = true; soundEventCount++; }
    if (micRms < micExitThresh) {
      unsigned long dur = now - micLoudStart;
      micLoud = false; micLastEnd = now;
      if (micCounted) {
        // gasp if it lands right after an apnea gap (arousal), else snore
        bool nearApnea = (lastExhaleMs > 0) && ((now - lastExhaleMs) > (APNEA_GAP_MS / 2));
        const char* kind = (dur < 500 || nearApnea) ? "gasp" : "snore";
        sendEvent(kind, (long)dur, (long)micPeak);
      }
    }
  }
}

// ======================================================================
//  Battery
// ======================================================================
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
  if (vbat <= LOW_BATTERY_THRESHOLD && vbat > 1.0) {
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_SEC * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
#endif
}

// ======================================================================
//  LED proof-of-life
// ======================================================================
void updateLed(bool up) {
  static unsigned long t0 = 0; static int phase = 0;
  unsigned long now = millis();
  if (!up) { if (now - t0 > 120) { t0 = now; phase ^= 1; ledWrite(phase); } return; }
  unsigned long c = now % 2500;
  ledWrite((c < 90) || (c > 200 && c < 290));
}

// ======================================================================
//  Setup / loop
// ======================================================================
void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("\nSoftAP up:  SSID \"%s\"  pass \"%s\"\n", AP_SSID, AP_PASS);
  Serial.printf("Open  http://%s\n", ip.toString().c_str());
  if (MDNS.begin("coolteam")) Serial.println("…or http://coolteam.local");
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  ledWrite(true);
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Cool Team Detector (mask) ===");

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin();
  maxOk = sensor.begin(Wire, I2C_SPEED_FAST);
  if (maxOk) {
    // ledBrightness=60 (forehead reflectance needs more than a fingertip),
    // sampleAvg=4, mode=2 (RED+IR), rate=100Hz, pulseWidth=411, adcRange=4096
    sensor.setup(60, 4, 2, 100, 411, 4096);
    sensor.enableDIETEMPRDY();
    Serial.println("MAX30102 ready (SpO2+HR).");
  } else {
    Serial.println("WARN: MAX30102 not found — SpO2/HR will read 0.");
  }

  Serial.println("Calibrating mic — keep quiet...");
  calibrateMic();

  float t = readTempC();
  tempFiltered = tempPrev = baseline = t;
  lastBreathSample = millis();

  vbat = readBatteryVoltage(); batPct = batteryPercent(vbat);
  Serial.printf("Battery: %.2f V (%d%%)\n", vbat, batPct);

  startAP();

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { r->send_P(200, "text/html", INDEX_HTML); });
  server.begin();
  Serial.println("HTTP server up.\n");
}

void loop() {
  unsigned long now = millis();
  uint32_t nowUs = micros();

  pollOximeter();                                       // SpO2 + HR + ODI

  if ((uint32_t)(nowUs - micSampleUs) >= MIC_PERIOD_US) { micSampleUs = nowUs; sampleMic(); }

  if (now - lastBreathSample >= BREATH_PERIOD_MS) updateBreath();

  if (now - lastBattMs >= 10000) {
    lastBattMs = now; vbat = readBatteryVoltage(); batPct = batteryPercent(vbat); maybeSleepLowBattery();
  }

  if (now - lastTelemMs >= 250) { lastTelemMs = now; sendTelemetry(); }   // 4 Hz

  if (now - lastWsCleanMs >= 500) { lastWsCleanMs = now; ws.cleanupClients(); }

  updateLed(true);                                      // AP is always "up"
}
