/* =====================================================================
 *  COOL TEAM DETECTOR — Haptic node
 *  Team: Cool Team   |   Board: Seeed XIAO ESP32-C6
 *
 *  WHAT IT DOES
 *   1) On boot, tries to join the mask's WiFi for up to 20 s.
 *        - connected  -> buzz TWICE
 *        - failed      -> buzz THREE times   (then keeps running anyway)
 *   2) Reads the MPU-6050 and detects the supine ("on your back") posture.
 *   3) Drives the DRV2605L in real-time mode so the vibration gets
 *        STRONGER the more squarely you're on your back (and ramps up a
 *        little the longer you stay there). No buzz when off your back.
 *   4) If connected, POSTs each posture change to the mask so the events
 *        show up in the phone dashboard's history.  (Buzzing is fully
 *        local — it does NOT depend on the connection.)
 *
 *  LIBRARIES:  Adafruit MPU6050, Adafruit DRV2605, Adafruit Unified Sensor
 *  I2C:  MPU-6050 @0x68 , DRV2605 @0x5A on the default Wire bus
 * ===================================================================== */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_DRV2605.h>

// ---- mask network (must match the mask's SoftAP) ----
const char* MASK_SSID = "CoolTeamDetector";
const char* MASK_PASS = "cooldemo123";
const char* MASK_HOST = "192.168.4.1";
#define CONNECT_TIMEOUT_MS 20000UL

// ---- supine detection ----
// cosZ = component of gravity along the board's +Z axis.
//   +1.0  => +Z points straight up  (lying flat, face-up = ON BACK)
//    0    => on your side / upright
// If your board is mounted upside-down, set this to -1.
#define SUPINE_AXIS_SIGN   (+1.0f)
#define SUPINE_ENTER        0.70f      // ~45 deg from flat -> counts as on-back
#define SUPINE_EXIT         0.50f      // hysteresis so it doesn't chatter
#define SUPINE_HOLD_MS      1500UL     // must hold this long before buzzing (use ~30000 for real use)

// ---- buzz intensity (DRV2605 real-time value, 0..127) ----
#define AMP_MIN     45                 // enough to overcome ERM start-up stall
#define AMP_MAX     127
#define ESCALATE_MS 8000.0f            // ramp from ~60% up to full over this long on back

Adafruit_MPU6050 mpu;
Adafruit_DRV2605 drv;

bool wifiUp = false;

// supine state
bool   onBack = false;
bool   candidate = false;
unsigned long candidateStart = 0, onBackStart = 0, lastPost = 0, lastDiag = 0, lastUpdate = 0;
float  supineScore = 0;

// ----------------------------------------------------------------------
//  DRV2605 real-time buzzing helpers
// ----------------------------------------------------------------------
void setBuzz(uint8_t amp) { drv.setRealtimeValue(amp); }   // 0 = off

void buzzPulses(int n) {                                   // countable feedback
  for (int i = 0; i < n; i++) {
    setBuzz(120); delay(250);
    setBuzz(0);   delay(220);
  }
}

// ----------------------------------------------------------------------
//  Report a posture change to the mask (optional; only if connected)
// ----------------------------------------------------------------------
void notifyMask(const char* kind, long sec) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "http://" + String(MASK_HOST) + "/haptic?kind=" + kind + "&sec=" + String(sec);
  http.begin(url);
  http.setConnectTimeout(400);        // don't stall the buzz loop if mask is slow
  http.GET();
  http.end();
}

// ----------------------------------------------------------------------
//  Setup
// ----------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Cool Team Detector (haptic node) ===");

  Wire.begin();

  if (!mpu.begin()) { Serial.println("MPU6050 not found — check wiring."); }
  else {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050 ready.");
  }

  if (!drv.begin()) { Serial.println("DRV2605 not found — check wiring."); }
  else {
    drv.selectLibrary(1);
    drv.useERM();
    drv.setMode(DRV2605_MODE_REALTIME);   // continuous, amplitude-controlled
    setBuzz(0);
    Serial.println("DRV2605 ready (real-time mode).");
  }

  // --- try to join the mask for up to 20 s ---
  Serial.printf("Joining \"%s\" (up to %lus)…", MASK_SSID, CONNECT_TIMEOUT_MS / 1000);
  WiFi.mode(WIFI_STA);
  WiFi.begin(MASK_SSID, MASK_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < CONNECT_TIMEOUT_MS) {
    delay(200); Serial.print('.');
  }
  wifiUp = (WiFi.status() == WL_CONNECTED);
  if (wifiUp) Serial.printf("\nConnected. IP %s — events will be tracked.\n", WiFi.localIP().toString().c_str());
  else        Serial.println("\nNo connection — running standalone (buzzing still works).");

  // feedback: 2 buzzes if connected, 3 if not
  buzzPulses(wifiUp ? 2 : 3);
}

// ----------------------------------------------------------------------
//  Loop  — read posture, drive intensity
// ----------------------------------------------------------------------
void loop() {
  unsigned long now = millis();
  if (now - lastUpdate < 50) return;     // ~20 Hz update
  lastUpdate = now;

  // --- orientation ---
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  float ax = a.acceleration.x, ay = a.acceleration.y, az = a.acceleration.z;
  float amag = sqrtf(ax * ax + ay * ay + az * az);
  if (amag < 1e-3f) amag = 1e-3f;
  supineScore = SUPINE_AXIS_SIGN * (az / amag);   // 1.0 = flat on back

  // --- enter/exit with a hold + hysteresis ---
  if (!onBack) {
    if (supineScore > SUPINE_ENTER) {
      if (!candidate) { candidate = true; candidateStart = now; }
      else if (now - candidateStart >= SUPINE_HOLD_MS) {
        onBack = true; onBackStart = now; candidate = false;
        Serial.println("-> ON BACK (buzzing)");
        notifyMask("supine", 0);
      }
    } else candidate = false;
  } else {
    if (supineScore < SUPINE_EXIT) {
      long sec = (long)((now - onBackStart) / 1000);
      onBack = false; setBuzz(0);
      Serial.printf("-> OFF BACK (was %lds)\n", sec);
      notifyMask("upright", sec);
    }
  }

  // --- intensity while on back ---
  if (onBack) {
    // base: how squarely on your back  (SUPINE_ENTER..1.0 -> AMP_MIN..AMP_MAX)
    float f = (supineScore - SUPINE_ENTER) / (1.0f - SUPINE_ENTER);
    if (f < 0) f = 0; if (f > 1) f = 1;
    int amp = AMP_MIN + (int)(f * (AMP_MAX - AMP_MIN));
    // escalate a little the longer you stay (clinical-style nudge)
    float dwell = (now - onBackStart) / ESCALATE_MS;
    float dfac = 0.6f + 0.4f * (dwell > 1 ? 1 : dwell);
    amp = (int)(amp * dfac);
    if (amp < AMP_MIN) amp = AMP_MIN; if (amp > AMP_MAX) amp = AMP_MAX;
    setBuzz((uint8_t)amp);
  }

  // light reconnect attempt if the link dropped (non-blocking)
  if (!onBack && WiFi.status() != WL_CONNECTED && now - lastPost > 30000) {
    lastPost = now; WiFi.begin(MASK_SSID, MASK_PASS);
  }

  // diagnostics every 2 s
  if (now - lastDiag >= 2000) {
    lastDiag = now;
    Serial.printf("score=%.2f onBack=%d wifi=%d\n", supineScore, onBack, WiFi.status() == WL_CONNECTED);
  }
}
