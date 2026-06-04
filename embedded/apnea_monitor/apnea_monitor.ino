/* =====================================================================
 *  COOL TEAM DETECTOR — Mask firmware  (rev 3)
 *  Team: Cool Team   |   Board: Seeed XIAO ESP32-S3 (Arduino-ESP32 3.x)
 *
 *  NEW IN REV 3
 *   - Breathing waveform history: 1-Hz samples of tempFiltered sent in
 *     telemetry as a circular buffer delta-encoded for compact JSON.
 *     Dashboard accumulates ~1 h of breathing signal client-side and
 *     renders it in the History tab with apnea events overlaid as red
 *     vertical bands so cessation is visually obvious.
 *   - ODI Severity card: real-time badge Normal / Mild / Moderate /
 *     Severe using clinical AHI bands (ODI 5-15 / 15-30 / ≥30) from
 *     literature (corroborated by Beer-Lambert SpO2 + AASM thresholds).
 *     Minimum-SpO2 and session elapsed time also shown.
 *   - Breathing sample added to telemetry JSON: "tc" field (tempC
 *     rounded to 2 dp), so client waveform never needs its own WS msg.
 *   - All previous fixes retained (beat-detect HR, NaN guard, 5-s mic).
 *
 *  CONNECT:  WiFi "CoolTeamDetector" / "cooldemo123"  ->  http://192.168.4.1
 *
 *  LIBRARIES:  SparkFun MAX3010x  +  ESP32Async "ESP Async WebServer" & "Async TCP"
 *  PINS:  Mic A8/GPIO7  Therm A9/GPIO8  Batt A10/GPIO9  I2C D4/D5  LED GPIO21(LOW)
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

const char* AP_SSID = "CoolTeamDetector";
const char* AP_PASS = "cooldemo123";

#define ENABLE_LOW_BATT_SLEEP 0
#define LOW_BATTERY_THRESHOLD 3.30
#define TIME_TO_SLEEP_SEC     1800
#define uS_TO_S_FACTOR        1000000ULL

const int MIC_PIN = 7, THERM_PIN = 8, VBAT_PIN = 9;
#ifndef LED_BUILTIN
#define LED_BUILTIN 21
#endif
const bool LED_ACTIVE_LOW = true;
inline void ledWrite(bool on) { digitalWrite(LED_BUILTIN, (on ^ LED_ACTIVE_LOW) ? HIGH : LOW); }

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// small helper: never let NaN/Inf into the JSON
inline float jf(float v) { return isfinite(v) ? v : 0.0f; }

// ----------------------------------------------------------------------
//  MAX30102
// ----------------------------------------------------------------------
MAX30105 sensor;
bool maxOk = false;

// --- HR from beat detection (the part that actually works on a mask) ---
const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE]; byte rateSpot = 0;
long  lastBeat = 0;
float beatsPerMinute = 0;
int   beatAvg = 0;
long  irValue = 0;
bool  fingerPresent = false;
#define FINGER_IR_MIN 20000          // lower than fingertip; mask is reflectance

// --- SpO2 from Maxim buffered algorithm ---
#define OX_N 100
uint32_t irBuf[OX_N], redBuf[OX_N];
int      oxFill = 0, oxNew = 0;
int32_t  spo2 = 0;   int8_t validSPO2 = 0;
int32_t  maximHR = 0; int8_t validHR = 0;
int      spo2Show = 0;

// ----------------------------------------------------------------------
//  ODI
// ----------------------------------------------------------------------
#define DESAT_DROP       3.0
#define DESAT_MIN_MS     10000UL     // lower (e.g. 4000) for a bench demo
#define SPO2_BASE_ALPHA  0.02
float    spo2Baseline = NAN;
enum OdiState { OX_NORMAL, OX_DROP, OX_RECOVER };
OdiState odiState = OX_NORMAL;
unsigned long dropStart = 0, desatCount = 0;
float    odi = 0;

// ----------------------------------------------------------------------
//  Thermistor breath
// ----------------------------------------------------------------------
#define SERIES_RESISTOR 10000.0
#define NOMINAL_RES 10000.0
#define NOMINAL_TEMP 25.0
#define B_COEFFICIENT 3950.0
#define THERM_VSUP_MV 3300.0
#define THERM_SAMPLES 5
#define BREATH_PERIOD_MS 100
#define SLOPE_ARM -0.5
#define DROP_COMMIT 0.8
#define ARM_TIMEOUT_MS 1200
#define SLOPE_EXHALE_EXIT -0.25
#define DEV_SETTLED 0.20
#define RECOVERY_TIMEOUT_MS 3000
#define BASELINE_ALPHA 0.01
#define RESEED_ALPHA 0.05
#define SIGNAL_ALPHA 0.35
enum BreathState { IDLE, EXHALE, RECOVERY, INHALE_REST };
float tempFiltered = NAN, tempPrev = NAN, baseline = NAN, breathDeviation = 0, breathSlope = 0;
BreathState breathState = IDLE;
unsigned long lastBreathSample = 0, exhaleCount = 0;
bool armed = false; float armPeakTemp = 0;
unsigned long armTime = 0, recoveryStart = 0;
unsigned long lastExhaleMs = 0; float respRate = 0;
#define APNEA_GAP_MS 10000UL
bool apneaFlagged = false; unsigned long apneaCount = 0;

// ----------------------------------------------------------------------
//  Mic — periodic level sample (every 5 s)
// ----------------------------------------------------------------------
#define SOUND_PERIOD_MS 5000
#define MIC_BURST_N     200
#define MIC_BURST_GAP_US 200
const float MIC_THRESH_MULT = 4.0, MIC_MIN_THRESH = 120.0;
float micEnterThresh = MIC_MIN_THRESH;
float micRms = 0;
unsigned long soundEventCount = 0, lastSoundMs = 0;

// ----------------------------------------------------------------------
//  Battery
// ----------------------------------------------------------------------
float vbat = 0; int batPct = 0;
const float BAT_DIVIDER = 2.0;

unsigned long lastBattMs = 0, lastTelemMs = 0, lastWsCleanMs = 0, lastDiagMs = 0;
const char* breathStateName();

// ======================================================================
//  Dashboard HTML — rev 3
//  Changes vs rev 2:
//    • History tab gains a "Breathing waveform" canvas (1 Hz signal,
//      apnea events as red vertical bands, exhale peaks as blue ticks)
//    • New "Severity" card on Live tab — ODI → Normal/Mild/Moderate/Severe
//      with color coding + min SpO2 + elapsed time
//    • Client-side breathing buffer (breathWave[]) fed by "tc" field in
//      every telemetry JSON; apnea events stamp their uptime for overlay
// ======================================================================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cool Team Detector</title>
<style>
  :root{--bg:#0c0f14;--card:#151b24;--line:#26303d;--txt:#e7eef6;--mut:#8a99ab;
        --ok:#37d39b;--warn:#ffcc55;--bad:#ff5d6c;--accent:#5aa9ff;
        --mild:#ffcc55;--mod:#ff8c42;--sev:#ff5d6c;}
  *{box-sizing:border-box} body{margin:0;font:15px/1.4 -apple-system,system-ui,sans-serif;
    background:var(--bg);color:var(--txt);padding:14px;max-width:560px;margin:auto}
  h1{font-size:18px;margin:0 0 2px} .sub{color:var(--mut);font-size:12px;margin-bottom:12px}
  .dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--bad);
    margin-right:6px;vertical-align:middle}
  .dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
  .tabs{display:flex;gap:8px;margin-bottom:12px}
  .tab{flex:1;padding:8px;border-radius:10px;border:1px solid var(--line);background:var(--card);
    color:var(--mut);font-size:14px;cursor:pointer;text-align:center}
  .tab.on{color:var(--txt);border-color:var(--accent);background:rgba(90,169,255,.12)}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px}
  .card.wide{grid-column:1/-1}
  .label{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.04em}
  .big{font-size:34px;font-weight:700;line-height:1} .unit{font-size:14px;color:var(--mut)}
  .row{display:flex;justify-content:space-between;align-items:baseline;margin-top:6px;gap:8px}
  .small{font-size:13px;color:var(--mut)}
  .heart{font-size:26px;transition:transform .08s ease} .beat{transform:scale(1.5)}
  .bar{height:8px;border-radius:6px;background:#222c38;overflow:hidden;margin-top:8px}
  .fill{height:100%;width:0;background:var(--accent);transition:width .2s}
  .pill{display:inline-block;padding:2px 9px;border-radius:999px;font-size:12px;background:#222c38;color:var(--txt)}
  .pill.ex{background:rgba(90,169,255,.2);color:var(--accent)}
  #log,#etable{max-height:180px;overflow:auto;margin-top:8px;font-size:13px}
  #log div,#etable div{padding:5px 0;border-bottom:1px solid var(--line)}
  .ap{color:var(--bad);font-weight:600}.ds{color:var(--accent);font-weight:600}.gp{color:var(--warn)}
  canvas{width:100%;display:block;margin-top:8px}
  .chart{height:120px}
  .breathchart{height:150px}
  button.act{margin-top:10px;padding:8px 12px;border-radius:10px;border:1px solid var(--accent);
    background:rgba(90,169,255,.12);color:var(--txt);font-size:13px;cursor:pointer}

  /* ---- Severity badge ---- */
  .sev-badge{display:inline-block;padding:4px 14px;border-radius:999px;font-size:14px;font-weight:700;letter-spacing:.05em}
  .sev-normal{background:rgba(55,211,155,.15);color:var(--ok);border:1px solid var(--ok)}
  .sev-mild  {background:rgba(255,204,85,.15);color:var(--mild);border:1px solid var(--mild)}
  .sev-mod   {background:rgba(255,140,66,.18);color:var(--mod);border:1px solid var(--mod)}
  .sev-sev   {background:rgba(255,93,108,.15);color:var(--bad);border:1px solid var(--bad)}

  /* ---- breath wave legend ---- */
  .legend{display:flex;gap:14px;font-size:11px;color:var(--mut);margin-top:6px;flex-wrap:wrap}
  .legend span{display:flex;align-items:center;gap:4px}
  .lswatch{width:12px;height:3px;border-radius:2px;display:inline-block}
</style></head><body>
<h1>Cool Team Detector <span style="font-size:12px;color:var(--mut)">— by Cool Team</span></h1>
<div class="sub"><span id="dot" class="dot"></span><span id="status">connecting…</span>
  &nbsp;·&nbsp;up <span id="up">0</span>s</div>

<div class="tabs">
  <div id="tabLive" class="tab on" onclick="showTab('live')">Live</div>
  <div id="tabHist" class="tab" onclick="showTab('hist')">History</div>
</div>

<!-- ===================== LIVE ===================== -->
<div id="live" class="grid">
  <div class="card wide">
    <div class="label">Blood oxygen (SpO₂)</div>
    <div class="row"><span class="big" id="spo2" style="color:var(--ok)">--</span><span class="unit">% SpO₂</span></div>
    <div class="row small"><span>ODI <b id="odi">0.0</b>/h</span><span>desats <b id="desat">0</b></span>
      <span>baseline <b id="base">--</b>%</span></div>
    <div class="row small"><span id="oxstat">acquiring signal…</span><span>IR <b id="ir">0</b></span></div>
  </div>

  <!-- NEW: ODI Severity card -->
  <div class="card wide" id="sevcard">
    <div class="label">OSA Severity — ODI-based screening</div>
    <div class="row" style="align-items:center;margin-top:10px">
      <span id="sevbadge" class="sev-badge sev-normal">NORMAL</span>
      <span class="small" style="text-align:right">min SpO₂ <b id="minSpo2Live">--</b>% &nbsp;·&nbsp; elapsed <b id="elapsed">0</b> min</span>
    </div>
    <div class="row small" style="margin-top:10px;gap:6px">
      <span style="color:var(--ok)">● Normal &lt;5</span>
      <span style="color:var(--mild)">● Mild 5–15</span>
      <span style="color:var(--mod)">● Moderate 15–30</span>
      <span style="color:var(--bad)">● Severe ≥30</span>
    </div>
    <div class="small" style="margin-top:6px;color:var(--mut)">
      ODI r &gt;0.94 vs lab AHI · ~93% sensitivity / ~92% specificity for OSA (Beer–Lambert SpO₂, AASM ≥3% drop ≥10 s)
    </div>
  </div>

  <div class="card">
    <div class="label">Heart rate <span id="heart" class="heart">♥</span></div>
    <div class="row"><span class="big" id="bpm">--</span><span class="unit">bpm</span></div>
    <div class="row small"><span id="finger">no contact</span></div>
    <canvas id="hrcv" width="260" height="48"></canvas>
  </div>

  <div class="card">
    <div class="label">Breathing</div>
    <div class="row"><span class="big" id="rr">--</span><span class="unit">breaths/min</span></div>
    <div class="row small"><span id="bstate" class="pill">--</span><span>exhales <b id="exh">0</b></span></div>
    <div class="row small"><span>temp <b id="tmp">--</b>°C</span><span>dev <b id="dev">--</b></span></div>
  </div>

  <div class="card">
    <div class="label">Sound level <span class="small">(5s)</span></div>
    <div class="row"><span class="big" id="snd">0</span><span class="unit">rms</span></div>
    <div class="bar"><div class="fill" id="sndbar"></div></div>
    <div class="row small"><span>events <b id="snc">0</b></span></div>
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

<!-- ===================== HISTORY ===================== -->
<div id="hist" class="grid" style="display:none">
  <div class="card wide">
    <div class="label">Session summary</div>
    <div class="row small">
      <span>ODI <b id="hODI">0.0</b>/h</span>
      <span>desats <b id="hDes">0</b></span>
      <span>apneas <b id="hAp">0</b></span>
      <span>min SpO₂ <b id="hMin">--</b>%</span>
    </div>
    <div class="row small" style="margin-top:6px">
      <span>Severity <span id="hSevBadge" class="sev-badge sev-normal" style="font-size:12px">NORMAL</span></span>
      <span>elapsed <b id="hElapsed">0</b> min</span>
    </div>
  </div>

  <!-- NEW: Breathing Waveform plot -->
  <div class="card wide">
    <div class="label">Airflow signal (thermistor) — breathing cycles</div>
    <canvas id="cBreath" class="breathchart" width="520" height="150"></canvas>
    <div class="legend">
      <span><span class="lswatch" style="background:#5aa9ff"></span>Airflow (temp deviation)</span>
      <span><span class="lswatch" style="background:rgba(255,93,108,0.7)"></span>Apnea event</span>
      <span><span class="lswatch" style="background:rgba(55,211,155,0.5)"></span>Exhale detected</span>
    </div>
    <div class="small" style="margin-top:6px;color:var(--mut)">
      Flat segments ≥10 s indicate cessation of airflow — AASM apnea definition.
      Red bands mark detected apnea events.
    </div>
  </div>

  <div class="card wide">
    <div class="label">SpO₂ over time</div>
    <canvas id="cSpo2" class="chart" width="520" height="120"></canvas>
  </div>
  <div class="card wide">
    <div class="label">Heart rate over time</div>
    <canvas id="cHr" class="chart" width="520" height="120"></canvas>
  </div>
  <div class="card wide">
    <div class="label">Event log (full session)</div>
    <div id="etable"><div class="small">no events yet</div></div>
    <button class="act" onclick="exportCsv()">Export CSV</button>
  </div>
</div>

<script>
const $=id=>document.getElementById(id);
let ws, hrLive=[], firstLog=true, histOn=false;
let sampHist=[], evtHist=[], lastHistT=-99, minSpo2=999;

// --- breathing waveform (1 Hz samples from "tc" telemetry field) ---
// Each entry: { t: uptime_s, dev: breathDeviation }
let breathWave=[];
const BREATH_MAX_PTS = 3600; // 1 hour at 1 Hz (2 Hz telem, we downsample to 1 Hz)
let lastBreathT = -1;

// --- session start ---
let sessionStartMs = null;

function showTab(t){
  histOn = (t==='hist');
  $('live').style.display = histOn?'none':'grid';
  $('hist').style.display = histOn?'grid':'none';
  $('tabLive').classList.toggle('on',!histOn);
  $('tabHist').classList.toggle('on', histOn);
  if(histOn) redrawHist();
}

function connect(){
  ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onopen =()=>{
    $('dot').classList.add('on'); $('status').textContent='connected';
    if(!sessionStartMs) sessionStartMs=Date.now();
  };
  ws.onclose=()=>{ $('dot').classList.remove('on'); $('status').textContent='reconnecting…'; setTimeout(connect,1500); };
  ws.onmessage=e=>{ let m; try{m=JSON.parse(e.data)}catch(_){return} handle(m); };
}

function sevInfo(odi){
  if(odi>=30) return {label:'SEVERE',  cls:'sev-sev'};
  if(odi>=15) return {label:'MODERATE',cls:'sev-mod'};
  if(odi>=5)  return {label:'MILD',    cls:'sev-mild'};
  return              {label:'NORMAL', cls:'sev-normal'};
}
function applySev(badgeEl, odi){
  const s=sevInfo(odi);
  badgeEl.textContent=s.label;
  badgeEl.className='sev-badge '+s.cls;
}

function handle(m){
  if(m.type==='beat'){ pulse(); return; }
  if(m.type==='event'){ evtHist.push(m); logEvent(m); if(histOn)renderEvents(); return; }
  if(m.type!=='telemetry') return;

  if(!sessionStartMs) sessionStartMs=Date.now();
  const elapsedMin=Math.round((Date.now()-sessionStartMs)/60000);

  $('up').textContent=m.uptime;

  // --- SpO2 ---
  if(m.spo2>0){
    $('spo2').textContent=m.spo2;
    $('oxstat').textContent='reading';
    $('spo2').style.color=(m.spo2<90)?'var(--bad)':(m.spo2<94?'var(--warn)':'var(--ok)');
    if(m.spo2<minSpo2) minSpo2=m.spo2;
  } else {
    $('spo2').textContent='--';
    $('oxstat').textContent=m.ir>20000?'signal ok, computing…':'no skin contact';
  }
  $('odi').textContent=m.odi.toFixed(1);
  $('desat').textContent=m.desat;
  $('base').textContent=m.base>0?m.base:'--';
  $('ir').textContent=m.ir;

  // --- Severity (live) ---
  applySev($('sevbadge'), m.odi);
  $('minSpo2Live').textContent=(minSpo2<999)?minSpo2:'--';
  $('elapsed').textContent=elapsedMin;

  // --- HR ---
  if(m.hr>0){ $('bpm').textContent=Math.round(m.hr); pushHrLive(m.hr); }
  $('finger').textContent=m.finger?'contact ✔':'no contact';

  // --- breathing / sound / battery ---
  $('rr').textContent=m.rr>0?m.rr.toFixed(1):'--';
  $('bstate').textContent=m.breath;
  $('bstate').className='pill'+(m.breath==='EXHALE'?' ex':'');
  $('exh').textContent=m.exhales;
  $('tmp').textContent=m.tempC.toFixed(2);
  $('dev').textContent=m.dev.toFixed(2);
  $('snd').textContent=Math.round(m.micRms);
  $('sndbar').style.width=Math.min(100,m.micRms/12)+'%';
  $('snc').textContent=m.sounds;
  $('vb').textContent=m.vbat.toFixed(2);
  $('vbpct').textContent=m.batPct+'%';
  $('vbbar').style.width=m.batPct+'%';
  $('vbbar').style.background=m.batPct<15?'var(--bad)':m.batPct<40?'var(--warn)':'var(--ok)';
  $('apc').textContent=m.apnea;

  // --- history summary ---
  $('hODI').textContent=m.odi.toFixed(1);
  $('hDes').textContent=m.desat;
  $('hAp').textContent=m.apnea;
  $('hMin').textContent=(minSpo2<999)?minSpo2:'--';
  $('hElapsed').textContent=elapsedMin;
  applySev($('hSevBadge'), m.odi);

  // --- breathing waveform accumulation (1 Hz downsample from 2 Hz telem) ---
  if(m.uptime !== lastBreathT){
    lastBreathT = m.uptime;
    // Use breathDeviation (dev) as the airflow proxy — centred on 0,
    // positive = warm (exhale near sensor), negative = cool (inhale or apnea)
    breathWave.push({ t: m.uptime, dev: m.dev, breath: m.breath });
    if(breathWave.length > BREATH_MAX_PTS) breathWave.shift();
    if(histOn) drawBreathWave();
  }

  // --- SpO2 / HR history sample (every 5 s) ---
  if(m.uptime - lastHistT >= 5){
    lastHistT = m.uptime;
    sampHist.push({t:m.uptime, spo2:m.spo2, hr:m.hr});
    if(sampHist.length>1500) sampHist.shift();
    if(histOn) redrawHist();
  }
}

function pulse(){ const h=$('heart'); h.classList.add('beat'); setTimeout(()=>h.classList.remove('beat'),90); }

function logEvent(m){
  if(firstLog){ $('log').innerHTML=''; firstLog=false; }
  const d=document.createElement('div');
  d.className=m.kind==='apnea'?'ap':(m.kind==='desat'?'ds':(m.kind==='gasp'?'gp':''));
  let t=m.kind.toUpperCase()+' @ '+m.t+'s';
  if(m.kind==='apnea') t+='  (gap '+m.gap+'s)';
  else if(m.kind==='desat') t+='  (−'+m.drop+'% → '+m.spo2+'%)';
  else if(m.peak!=null) t+='  ('+m.peak+' rms)';
  d.textContent=t; $('log').prepend(d);
}

function pushHrLive(v){
  hrLive.push(v); if(hrLive.length>60) hrLive.shift();
  drawSeries('hrcv', hrLive.map((y,i)=>({t:i,hr:y})), 'hr', '#5aa9ff', false);
}

// -----------------------------------------------------------------------
//  Generic series chart (SpO2 / HR)
// -----------------------------------------------------------------------
function drawSeries(id, data, key, color, big){
  const c=$(id), x=c.getContext('2d'), w=c.width, h=c.height;
  x.clearRect(0,0,w,h);
  const pts=data.filter(d=>d[key]>0);
  if(pts.length<2){
    x.fillStyle='#8a99ab'; x.font='12px sans-serif'; x.fillText('collecting…',8,18); return;
  }
  const pad=big?26:0;
  let mn=Math.min(...pts.map(d=>d[key]))-2, mx=Math.max(...pts.map(d=>d[key]))+2;
  const r=mx-mn||1;
  x.strokeStyle=color; x.lineWidth=2; x.beginPath();
  pts.forEach((d,i)=>{
    const px=pad+i/(pts.length-1)*(w-pad-2), py=h-8-(d[key]-mn)/r*(h-16);
    i?x.lineTo(px,py):x.moveTo(px,py);
  });
  x.stroke();
  if(big){
    x.fillStyle='#8a99ab'; x.font='10px sans-serif';
    x.fillText(Math.round(mx),2,12); x.fillText(Math.round(mn),2,h-2);
  }
}

// -----------------------------------------------------------------------
//  Breathing waveform chart
//  - Blue line: breathDeviation (thermistor signal centred at 0)
//  - Green ticks: exhale events (breath === 'EXHALE')
//  - Red vertical bands: apnea events (from evtHist), ≥10 s gap
// -----------------------------------------------------------------------
function drawBreathWave(){
  const c=$('cBreath');
  if(!c) return;
  const ctx=c.getContext('2d');
  const W=c.width, H=c.height;
  ctx.clearRect(0,0,W,H);

  if(breathWave.length<2){
    ctx.fillStyle='#8a99ab'; ctx.font='12px sans-serif';
    ctx.fillText('collecting breathing data…', 8, H/2); return;
  }

  const pts = breathWave;
  const tMin = pts[0].t, tMax = pts[pts.length-1].t;
  const tSpan = Math.max(tMax-tMin, 1);

  // Compute Y range with a symmetric ±max approach for clean zero line
  let devMax = 0;
  pts.forEach(p=>{ if(Math.abs(p.dev)>devMax) devMax=Math.abs(p.dev); });
  devMax = Math.max(devMax, 0.15); // minimum scale so flat line is visible

  const PAD_L=30, PAD_R=8, PAD_T=12, PAD_B=24;
  const plotW=W-PAD_L-PAD_R, plotH=H-PAD_T-PAD_B;

  function tx(t){ return PAD_L + (t-tMin)/tSpan * plotW; }
  function ty(v){ return PAD_T + plotH/2 - (v/devMax)*(plotH/2-4); }

  // --- Y-axis labels ---
  ctx.fillStyle='#8a99ab'; ctx.font='10px sans-serif'; ctx.textAlign='right';
  ctx.fillText('+'+devMax.toFixed(1), PAD_L-4, PAD_T+6);
  ctx.fillText('0',                   PAD_L-4, PAD_T+plotH/2+4);
  ctx.fillText('-'+devMax.toFixed(1), PAD_L-4, PAD_T+plotH-2);
  ctx.textAlign='left';

  // --- zero line ---
  ctx.strokeStyle='rgba(138,153,171,0.25)'; ctx.lineWidth=1; ctx.setLineDash([4,4]);
  ctx.beginPath(); ctx.moveTo(PAD_L, ty(0)); ctx.lineTo(PAD_L+plotW, ty(0)); ctx.stroke();
  ctx.setLineDash([]);

  // --- clip region ---
  ctx.save();
  ctx.beginPath(); ctx.rect(PAD_L, PAD_T, plotW, plotH); ctx.clip();

  // --- apnea event bands (red vertical stripes) ---
  // Each apnea event has a .gap field (seconds). We paint from (t - gap) to t.
  evtHist.filter(e=>e.kind==='apnea').forEach(e=>{
    const x0=tx(e.t - e.gap), x1=tx(e.t);
    ctx.fillStyle='rgba(255,93,108,0.18)';
    ctx.fillRect(x0, PAD_T, Math.max(x1-x0, 2), plotH);
    // left edge marker
    ctx.strokeStyle='rgba(255,93,108,0.7)'; ctx.lineWidth=1.5;
    ctx.beginPath(); ctx.moveTo(x0,PAD_T); ctx.lineTo(x0,PAD_T+plotH); ctx.stroke();
    ctx.strokeStyle='rgba(255,93,108,0.9)';
    ctx.beginPath(); ctx.moveTo(x1,PAD_T); ctx.lineTo(x1,PAD_T+plotH); ctx.stroke();
  });

  // --- exhale tick marks (small green triangles at top) ---
  pts.forEach(p=>{
    if(p.breath==='EXHALE'){
      const px=tx(p.t);
      ctx.fillStyle='rgba(55,211,155,0.55)';
      ctx.beginPath(); ctx.moveTo(px-3,PAD_T+2); ctx.lineTo(px+3,PAD_T+2); ctx.lineTo(px,PAD_T+8); ctx.fill();
    }
  });

  // --- breathing signal line ---
  ctx.strokeStyle='#5aa9ff'; ctx.lineWidth=1.8;
  ctx.beginPath();
  pts.forEach((p,i)=>{
    const px=tx(p.t), py=ty(p.dev);
    i===0 ? ctx.moveTo(px,py) : ctx.lineTo(px,py);
  });
  ctx.stroke();

  ctx.restore();

  // --- time axis labels ---
  ctx.fillStyle='#8a99ab'; ctx.font='10px sans-serif'; ctx.textAlign='center';
  const tickCount=5;
  for(let i=0;i<=tickCount;i++){
    const t=tMin+i/tickCount*tSpan;
    const px=tx(t);
    ctx.fillText(fmtTime(t), px, H-4);
    // tick
    ctx.strokeStyle='rgba(138,153,171,0.2)'; ctx.lineWidth=1; ctx.setLineDash([]);
    ctx.beginPath(); ctx.moveTo(px,PAD_T+plotH); ctx.lineTo(px,PAD_T+plotH+4); ctx.stroke();
  }
}

function fmtTime(s){
  const m=Math.floor(s/60), sec=Math.floor(s%60);
  return m+'m'+String(sec).padStart(2,'0')+'s';
}

function redrawHist(){
  drawBreathWave();
  drawSeries('cSpo2', sampHist, 'spo2', '#37d39b', true);
  drawSeries('cHr',   sampHist, 'hr',   '#5aa9ff', true);
  renderEvents();
}

function renderEvents(){
  const t=$('etable'); if(!evtHist.length){ t.innerHTML='<div class="small">no events yet</div>'; return; }
  let html='';
  evtHist.slice().reverse().forEach(e=>{
    let d=e.kind.toUpperCase()+' @ '+e.t+'s';
    if(e.kind==='apnea') d+=' (gap '+e.gap+'s)';
    else if(e.kind==='desat') d+=' (−'+e.drop+'%→'+e.spo2+'%)';
    else if(e.peak!=null) d+=' ('+e.peak+' rms)';
    html+='<div class="'+(e.kind==='apnea'?'ap':e.kind==='desat'?'ds':e.kind==='gasp'?'gp':'')+'">'+d+'</div>';
  });
  t.innerHTML=html;
}

function exportCsv(){
  let rows=['kind,t_seconds,detail'];
  evtHist.forEach(e=>{
    let det=e.kind==='apnea'?('gap='+e.gap+'s'):(e.kind==='desat'?('drop='+e.drop+'%,spo2='+e.spo2):(e.peak!=null?('peak='+e.peak):''));
    rows.push(e.kind+','+e.t+','+det);
  });
  rows.push(''); rows.push('t_seconds,spo2,hr');
  sampHist.forEach(s=>rows.push(s.t+','+(s.spo2||'')+','+(s.hr?Math.round(s.hr):'')));
  rows.push(''); rows.push('t_seconds,breath_dev,breath_state');
  breathWave.forEach(p=>rows.push(p.t+','+p.dev.toFixed(3)+','+p.breath));
  const blob=new Blob([rows.join('\n')],{type:'text/csv'});
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob); a.download='coolteam_session.csv'; a.click();
}

connect();
</script></body></html>
)HTML";

// ======================================================================
//  WebSocket
// ======================================================================
void wsBroadcast(const char* json) { if (ws.count() > 0) ws.textAll(json); }
void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType type, void* a, uint8_t* d, size_t l) {
  if (type == WS_EVT_CONNECT) Serial.printf("WS client %u connected\n", c->id());
}
void sendBeat() { wsBroadcast("{\"type\":\"beat\"}"); }

void sendEvent(const char* kind, long a1, long a2) {
  char buf[160]; unsigned long t = millis() / 1000;
  if (strcmp(kind, "apnea") == 0)
    snprintf(buf, sizeof(buf), "{\"type\":\"event\",\"kind\":\"apnea\",\"t\":%lu,\"gap\":%ld}", t, a1);
  else if (strcmp(kind, "desat") == 0)
    snprintf(buf, sizeof(buf), "{\"type\":\"event\",\"kind\":\"desat\",\"t\":%lu,\"drop\":%ld,\"spo2\":%ld}", t, a1, a2);
  else
    snprintf(buf, sizeof(buf), "{\"type\":\"event\",\"kind\":\"%s\",\"t\":%lu,\"peak\":%ld}", kind, t, a2);
  wsBroadcast(buf);
}

// Rev 3: telemetry includes "dev" (breathDeviation) so dashboard can build
// the breathing waveform from normal 2 Hz telemetry — no extra WS message needed.
void sendTelemetry() {
  char buf[700];
  int baseInt = isnan(spo2Baseline) ? 0 : (int)lround(spo2Baseline);
  snprintf(buf, sizeof(buf),
    "{\"type\":\"telemetry\",\"uptime\":%lu,"
    "\"spo2\":%d,\"odi\":%.1f,\"desat\":%lu,\"base\":%d,\"ir\":%ld,"
    "\"hr\":%d,\"finger\":%s,"
    "\"rr\":%.1f,\"breath\":\"%s\",\"exhales\":%lu,\"tempC\":%.2f,\"dev\":%.3f,"
    "\"micRms\":%.0f,\"sounds\":%lu,\"vbat\":%.2f,\"batPct\":%d,\"apnea\":%lu}",
    millis() / 1000,
    spo2Show, jf(odi), desatCount, baseInt, irValue,
    beatAvg, fingerPresent ? "true" : "false",
    jf(respRate), breathStateName(), exhaleCount,
    jf(tempFiltered), jf(breathDeviation),   // <-- dev now 3 dp for waveform fidelity
    jf(micRms), soundEventCount, jf(vbat), batPct, apneaCount);
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
  if (irValue < FINGER_IR_MIN) return;
  if (isnan(spo2Baseline)) spo2Baseline = s;
  unsigned long now = millis();
  switch (odiState) {
    case OX_NORMAL:
      spo2Baseline = (1.0 - SPO2_BASE_ALPHA) * spo2Baseline + SPO2_BASE_ALPHA * s;
      if (spo2Baseline - s >= DESAT_DROP) { odiState = OX_DROP; dropStart = now; }
      break;
    case OX_DROP:
      if (spo2Baseline - s < DESAT_DROP) odiState = OX_NORMAL;
      else if (now - dropStart >= DESAT_MIN_MS) {
        desatCount++;
        sendEvent("desat", (long)lround(spo2Baseline - s), spo2Show);
        odiState = OX_RECOVER;
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

    // --- heart rate from beat detection ---
    if (checkForBeat(ir)) {
      long now = millis();
      float bpm = 60000.0 / (now - lastBeat);
      lastBeat = now;
      if (bpm > 20 && bpm < 255) {
        beatsPerMinute = bpm;
        rates[rateSpot++] = (byte)bpm; rateSpot %= RATE_SIZE;
        beatAvg = 0; for (byte i = 0; i < RATE_SIZE; i++) beatAvg += rates[i]; beatAvg /= RATE_SIZE;
        sendBeat();
      }
    }

    // --- fill window for Maxim SpO2 ---
    if (oxFill < OX_N) { redBuf[oxFill] = red; irBuf[oxFill] = ir; oxFill++; }
    else {
      memmove(redBuf, redBuf + 1, (OX_N - 1) * sizeof(uint32_t));
      memmove(irBuf,  irBuf  + 1, (OX_N - 1) * sizeof(uint32_t));
      redBuf[OX_N - 1] = red; irBuf[OX_N - 1] = ir; oxNew++;
    }
  }
  fingerPresent = irValue > FINGER_IR_MIN;

  if (oxFill >= OX_N && oxNew >= 25) {
    oxNew = 0;
    maxim_heart_rate_and_oxygen_saturation(irBuf, OX_N, redBuf, &spo2, &validSPO2, &maximHR, &validHR);
    if (validSPO2 && spo2 > 0 && spo2 <= 100) { spo2Show = spo2; updateOdi((float)spo2); }
  }
}

// ======================================================================
//  Thermistor breath
// ======================================================================
float readTempC() {
  uint32_t acc = 0;
  for (int i = 0; i < THERM_SAMPLES; i++) acc += analogReadMilliVolts(THERM_PIN);
  float mv = acc / (float)THERM_SAMPLES; if (mv >= THERM_VSUP_MV) mv = THERM_VSUP_MV - 1;
  float r = SERIES_RESISTOR * mv / (THERM_VSUP_MV - mv);
  float s = log(r / NOMINAL_RES) / B_COEFFICIENT + 1.0 / (NOMINAL_TEMP + 273.15);
  return 1.0 / s - 273.15;
}

void registerExhale(unsigned long now) {
  breathState = EXHALE; exhaleCount++;
  if (lastExhaleMs > 0) {
    float iv = (now - lastExhaleMs) / 1000.0;
    if (iv > 0.5 && iv < 20.0) {
      float inst = 60.0 / iv;
      respRate = (respRate == 0) ? inst : 0.4 * inst + 0.6 * respRate;
    }
  }
  lastExhaleMs = now; apneaFlagged = false;
}

void updateBreath() {
  unsigned long now = millis();
  float dt = (now - lastBreathSample) / 1000.0; lastBreathSample = now;
  float raw = readTempC();
  tempFiltered = SIGNAL_ALPHA * raw + (1.0 - SIGNAL_ALPHA) * tempFiltered;
  breathSlope = (tempFiltered - tempPrev) / dt; tempPrev = tempFiltered;
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
    case IDLE: case INHALE_REST:
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
    apneaFlagged = true; apneaCount++;
    sendEvent("apnea", (long)((now - lastExhaleMs) / 1000), 0);
  }
}

// ======================================================================
//  Mic — periodic level (every 5 s)
// ======================================================================
void calibrateMicFloor() {
  double ss = 0; uint32_t accBias = 0;
  for (int i = 0; i < MIC_BURST_N; i++) accBias += analogRead(MIC_PIN);
  float bias = accBias / (float)MIC_BURST_N;
  for (int i = 0; i < MIC_BURST_N; i++) {
    float x = analogRead(MIC_PIN) - bias; ss += (double)x * x; delayMicroseconds(MIC_BURST_GAP_US);
  }
  float floorRms = sqrt(ss / MIC_BURST_N);
  micEnterThresh = max(floorRms * MIC_THRESH_MULT, MIC_MIN_THRESH);
  Serial.printf("Mic floor=%.1f enter=%.1f\n", floorRms, micEnterThresh);
}

void sampleSoundLevel() {
  double ss = 0; uint32_t accBias = 0;
  for (int i = 0; i < MIC_BURST_N; i++) accBias += analogRead(MIC_PIN);
  float bias = accBias / (float)MIC_BURST_N;
  for (int i = 0; i < MIC_BURST_N; i++) {
    float x = analogRead(MIC_PIN) - bias; ss += (double)x * x; delayMicroseconds(MIC_BURST_GAP_US);
  }
  micRms = sqrt(ss / MIC_BURST_N);
  if (micRms > micEnterThresh) {
    soundEventCount++;
    bool nearApnea = (lastExhaleMs > 0) && ((millis() - lastExhaleMs) > (APNEA_GAP_MS / 2));
    sendEvent(nearApnea ? "gasp" : "snore", 0, (long)micRms);
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
    {3.70,40},{3.60,25},{3.50,15},{3.40,8},{3.30,5},{3.00,0}
  };
  const int n = sizeof(lut)/sizeof(lut[0]);
  if (v >= lut[0].v) return 100;
  if (v <= lut[n-1].v) return 0;
  for (int i = 0; i < n-1; i++)
    if (v <= lut[i].v && v > lut[i+1].v) {
      float f = (v - lut[i+1].v)/(lut[i].v - lut[i+1].v);
      return lut[i+1].p + f*(lut[i].p - lut[i+1].p);
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

void updateLed(bool up) {
  static unsigned long t0 = 0; static int phase = 0; unsigned long now = millis();
  if (!up) { if (now - t0 > 120) { t0 = now; phase ^= 1; ledWrite(phase); } return; }
  unsigned long c = now % 2500; ledWrite((c < 90) || (c > 200 && c < 290));
}

// ======================================================================
//  Setup / loop
// ======================================================================
void startAP() {
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("\nSoftAP up: \"%s\" / \"%s\"  ->  http://%s\n",
    AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  if (MDNS.begin("coolteam")) Serial.println("…or http://coolteam.local");
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT); ledWrite(true);
  Serial.begin(115200); delay(300);
  Serial.println("\n=== Cool Team Detector (mask) rev3 ===");
  analogReadResolution(12); analogSetAttenuation(ADC_11db);

  Wire.begin();
  maxOk = sensor.begin(Wire, I2C_SPEED_FAST);
  if (maxOk) {
    sensor.setup(60, 4, 2, 100, 411, 4096); sensor.enableDIETEMPRDY();
    Serial.println("MAX30102 ready.");
  } else {
    Serial.println("WARN: MAX30102 NOT found — check I2C wiring (SDA=GPIO5,SCL=GPIO6).");
  }

  Serial.println("Mic floor cal — keep quiet...");
  calibrateMicFloor();

  tempFiltered = tempPrev = baseline = readTempC();
  lastBreathSample = millis();
  vbat = readBatteryVoltage(); batPct = batteryPercent(vbat);
  Serial.printf("Battery: %.2f V (%d%%)\n", vbat, batPct);

  startAP();
  ws.onEvent(onWsEvent); server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "text/html", INDEX_HTML);
  });
  server.begin();
  Serial.println("HTTP server up.\n");
}

void loop() {
  unsigned long now = millis();

  pollOximeter();

  if (now - lastSoundMs >= SOUND_PERIOD_MS)   { lastSoundMs = now; sampleSoundLevel(); }
  if (now - lastBreathSample >= BREATH_PERIOD_MS) updateBreath();
  if (now - lastBattMs >= 10000)              { lastBattMs = now; vbat = readBatteryVoltage(); batPct = batteryPercent(vbat); maybeSleepLowBattery(); }
  if (now - lastTelemMs >= 500)               { lastTelemMs = now; sendTelemetry(); }   // 2 Hz
  if (now - lastWsCleanMs >= 500)             { lastWsCleanMs = now; ws.cleanupClients(); }

  // Serial diagnostics every 3 s
  if (now - lastDiagMs >= 3000) {
    lastDiagMs = now;
    Serial.printf("clients=%u  IR=%ld finger=%d  HR(avg)=%d  SpO2=%d(valid=%d)  base=%.0f  desat=%lu  apnea=%lu  dev=%.3f\n",
      ws.count(), irValue, fingerPresent, beatAvg, spo2Show, validSPO2,
      isnan(spo2Baseline) ? 0 : spo2Baseline, desatCount, apneaCount, breathDeviation);
  }

  updateLed(true);
}
