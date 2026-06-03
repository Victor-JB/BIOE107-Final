/*
 * Thermistor breath detector v4 — Arduino Pro Micro / Leonardo, pin A0
 *
 * EXHALE = fast air jet -> convective COOLING -> temp drops.
 * The warm-up afterward is the bead re-equilibrating -> RECOVERY,
 * never counted. INHALE is too weak to detect -> merged into REST.
 *
 * v4 changes vs v3:
 *  - Exhale committed on CUMULATIVE DROP, not instantaneous slope.
 *    Lets the arm-threshold drop for sensitivity while rejecting
 *    recovery wobbles and noise ripples (fixes inflated count).
 *  - RECOVERY has a TIMEOUT: once cooling has clearly stopped, the
 *    breath is declared over and state returns to REST even if the
 *    bead is still far below the (stale) baseline. Fixes "stuck in
 *    recovery forever."
 *  - Baseline re-seeds toward the settled rest temperature so a
 *    one-time offset (cold start, sensor moved) self-corrects.
 *
 * Wiring (voltage divider):
 *   5V ---[ 10k fixed resistor ]---+---[ 10k NTC thermistor ]--- GND
 *                                  |
 *                                 A0
 */

#define THERMISTOR_PIN   A
#define SERIES_RESISTOR  10000.0
#define NOMINAL_RES      10000.0
#define NOMINAL_TEMP     25.0
#define B_COEFFICIENT    3950.0   // check your datasheet
#define ADC_MAX          1023.0
#define SAMPLES          5

#define SAMPLE_PERIOD_MS 100      // 10 Hz

// --- exhale detection ---
// ARM on a modest cooling slope (more sensitive than v3's -0.7).
// COMMIT only once temperature has fallen a cumulative amount from
// where cooling began -- this is what separates a real soft exhale
// from a noise ripple or a recovery wobble.
#define SLOPE_ARM          -0.5   // cooling faster than this arms a candidate
#define DROP_COMMIT         0.8   // cumulative C drop required to confirm exhale
#define ARM_TIMEOUT_MS      1200  // candidate must commit within this or it's dropped

// EXHALE ends when cooling clearly stops.
#define SLOPE_EXHALE_EXIT  -0.25  // slope above this => cooling has stopped

// --- recovery handling ---
// RECOVERY = warming back toward baseline after an exhale. Suppressed.
// Exits to REST when EITHER:
//   (a) bead has settled near baseline, OR
//   (b) timeout: cooling stopped long enough ago that the breath is
//       over, regardless of how far below the stale baseline we are.
#define DEV_SETTLED          0.20  // |deviation| under this => at rest
#define RECOVERY_TIMEOUT_MS  3000  // max time held in RECOVERY before forcing REST

// Baseline tracker
#define BASELINE_ALPHA       0.01  // slow ambient follow, only while at rest
#define RESEED_ALPHA         0.05  // faster pull used to re-seed after a breath

// Signal smoothing
#define SIGNAL_ALPHA         0.35

enum BreathState { IDLE, EXHALE, RECOVERY, INHALE_REST };

float tempFiltered = NAN;
float tempPrev     = NAN;
float baseline     = NAN;
BreathState state  = IDLE;
unsigned long lastSample = 0;
unsigned long exhaleCount = 0;

// exhale-candidate tracking
bool  armed          = false;
float armPeakTemp    = 0;     // highest temp since the candidate armed
unsigned long armTime = 0;
unsigned long recoveryStart = 0;

float readTempC() {
  float adc = 0;
  for (int i = 0; i < SAMPLES; i++) adc += analogRead(THERMISTOR_PIN);
  adc /= SAMPLES;
  float resistance = SERIES_RESISTOR * adc / (ADC_MAX - adc);
  float s = resistance / NOMINAL_RES;
  s = log(s);
  s /= B_COEFFICIENT;
  s += 1.0 / (NOMINAL_TEMP + 273.15);
  s = 1.0 / s;
  return s - 273.15;
}

const char* stateName(BreathState s) {
  switch (s) {
    case EXHALE:      return "EXHALE";
    case RECOVERY:    return "RECOVERY";
    case INHALE_REST: return "INHALE/REST";
    default:          return "IDLE";
  }
}

void setup() {
  Serial.begin(9600);
  while (!Serial) { ; }
  analogReference(DEFAULT);
  float t = readTempC();
  tempFiltered = t;
  tempPrev     = t;
  baseline     = t;
  lastSample   = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - lastSample < SAMPLE_PERIOD_MS) return;
  float dt = (now - lastSample) / 1000.0;
  lastSample = now;

  // --- read & smooth ---
  float raw = readTempC();
  tempFiltered = SIGNAL_ALPHA * raw + (1.0 - SIGNAL_ALPHA) * tempFiltered;

  // --- slope ---
  float slope = (tempFiltered - tempPrev) / dt;
  tempPrev = tempFiltered;

  // --- baseline: only track while genuinely at rest ---
  bool atRest = (state == IDLE || state == INHALE_REST);
  if (atRest) {
    // slow ambient follow
    baseline = BASELINE_ALPHA * tempFiltered + (1.0 - BASELINE_ALPHA) * baseline;
    // if we're at rest but baseline is still far from the actual
    // resting temp (stale offset from a cold start or a moved
    // sensor), pull it in faster so future deviations are correct
    if (fabs(tempFiltered - baseline) > DEV_SETTLED) {
      baseline = RESEED_ALPHA * tempFiltered + (1.0 - RESEED_ALPHA) * baseline;
    }
  }
  float deviation = tempFiltered - baseline;

  // --- exhale candidate arming (runs in REST and RECOVERY) ---
  // Track the peak temp so we can measure cumulative drop from it.
  if (!armed) {
    if (slope <= SLOPE_ARM) {
      armed       = true;
      armPeakTemp = tempPrev > tempFiltered ? tempPrev : tempFiltered;
      armTime     = now;
    }
  } else {
    // keep peak as the highest temp seen while armed (in case it
    // rose slightly before the real cool-down)
    if (tempFiltered > armPeakTemp) armPeakTemp = tempFiltered;
    // candidate expires if it doesn't commit in time, or if temp
    // climbs back up (cooling reversed -> it was just a wobble)
    if (now - armTime > ARM_TIMEOUT_MS || slope > 0.2) {
      armed = false;
    }
  }

  bool exhaleConfirmed =
      armed && (armPeakTemp - tempFiltered) >= DROP_COMMIT;

  // --- state machine ---
  switch (state) {

    case IDLE:
    case INHALE_REST:
      state = INHALE_REST;
      if (exhaleConfirmed) {
        state = EXHALE;
        exhaleCount++;
        armed = false;            // consume the candidate
      }
      break;

    case EXHALE:
      // stay in EXHALE while still cooling
      if (slope > SLOPE_EXHALE_EXIT) {
        state = RECOVERY;
        recoveryStart = now;
        armed = false;            // clear any stale candidate
      }
      break;

    case RECOVERY:
      // a genuine NEW exhale during recovery must clear the same
      // cumulative-drop bar -> wobbles can't re-trigger the count
      if (exhaleConfirmed) {
        state = EXHALE;
        exhaleCount++;
        armed = false;
      }
      // otherwise: exit on settle OR on timeout
      else if (deviation >= -DEV_SETTLED) {
        state = INHALE_REST;      // bead returned to baseline
      }
      else if (now - recoveryStart > RECOVERY_TIMEOUT_MS) {
        state = INHALE_REST;      // breath is over; stop waiting
        // re-seed baseline toward where we actually settled so the
        // next deviation reference isn't the stale pre-session value
        baseline = tempFiltered;
      }
      break;
  }

  // --- output ---
  Serial.print("T=");        Serial.print(tempFiltered, 2);
  Serial.print("C base=");   Serial.print(baseline, 2);
  Serial.print(" dev=");     Serial.print(deviation, 2);
  Serial.print(" slope=");   Serial.print(slope, 2);
  Serial.print("C/s  -> ");  Serial.print(stateName(state));
  Serial.print("   exhales="); Serial.println(exhaleCount);
}