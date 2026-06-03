'use strict';

const POSITIONS = ['supine', 'left', 'supine', 'right', 'supine', 'left', 'supine'];
const AIRFLOW = ['inhale', 'exhale'];

function gaussianNoise(mean, std) {
  // Box-Muller transform
  const u1 = Math.random();
  const u2 = Math.random();
  const z = Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
  return mean + z * std;
}

function clamp(val, min, max) {
  return Math.max(min, Math.min(max, val));
}

/**
 * Generate a full night of mock data.
 * @param {Date} startDate - start of the night (e.g., 11pm)
 * @param {number} durationHours - default 7.5
 * @param {string} scenario - 'low'|'moderate'|'high' risk
 */
function generateNight(startDate = new Date(), durationHours = 7.5, scenario = 'moderate') {
  const samples = [];
  const events = [];
  const totalSeconds = Math.round(durationHours * 3600);
  const startMs = startDate.getTime();

  // Scenario configs
  const configs = {
    low:      { clusterCount: 1, clusterIntensity: 0.4, baseSnore: 8,  clusterSnore: 40 },
    moderate: { clusterCount: 3, clusterIntensity: 0.7, baseSnore: 15, clusterSnore: 65 },
    high:     { clusterCount: 5, clusterIntensity: 1.0, baseSnore: 25, clusterSnore: 85 },
  };
  const cfg = configs[scenario] || configs.moderate;

  // Apnea cluster centers (fraction of total duration)
  const clusterCenters = [];
  if (cfg.clusterCount >= 1) clusterCenters.push(0.20);
  if (cfg.clusterCount >= 2) clusterCenters.push(0.45);
  if (cfg.clusterCount >= 3) clusterCenters.push(0.70);
  if (cfg.clusterCount >= 4) clusterCenters.push(0.55);
  if (cfg.clusterCount >= 5) clusterCenters.push(0.85);

  // Signal quality gaps (index ranges where spo2 should be null)
  const gapRanges = [
    [Math.floor(totalSeconds * 0.12), Math.floor(totalSeconds * 0.12) + 90],
    [Math.floor(totalSeconds * 0.58), Math.floor(totalSeconds * 0.58) + 45],
    [Math.floor(totalSeconds * 0.82), Math.floor(totalSeconds * 0.82) + 60],
  ];

  function isInGap(sec) {
    return gapRanges.some(([a, b]) => sec >= a && sec < b);
  }

  // Position schedule
  function getPosition(sec) {
    const frac = sec / totalSeconds;
    if (frac < 0.15) return 'supine';
    if (frac < 0.30) return 'left';
    if (frac < 0.55) return 'supine';
    if (frac < 0.70) return 'right';
    if (frac < 0.85) return 'supine';
    return 'left';
  }

  // Is this second inside an apnea cluster?
  function getClusterIntensity(sec) {
    const frac = sec / totalSeconds;
    let maxIntensity = 0;
    for (const center of clusterCenters) {
      // Cluster lasts ~20 minutes, shaped like a bell
      const dist = Math.abs(frac - center);
      const clusterWidth = 0.045; // ~20 min
      if (dist < clusterWidth) {
        const intensity = (1 - dist / clusterWidth) * cfg.clusterIntensity;
        maxIntensity = Math.max(maxIntensity, intensity);
      }
    }
    return maxIntensity;
  }

  let lastPosition = 'supine';
  let spo2 = 97;
  let hr = 60;
  let snore = 5;

  // Track active apnea event state
  let apneaEventActive = false;
  let apneaEventStart = 0;
  let apneaEventStartSpo2 = 97;
  let postApneaRecovery = 0;

  for (let sec = 0; sec < totalSeconds; sec++) {
    const ts = startMs + sec * 1000;
    const pos = getPosition(sec);
    const clusterI = getClusterIntensity(sec);
    const inGap = isInGap(sec);

    // Position change events
    if (pos !== lastPosition) {
      events.push({
        event_type: 'position_change',
        severity: 'info',
        timestamp: ts,
        details: `from=${lastPosition},to=${pos}`,
      });
      lastPosition = pos;
    }

    // Movement: high during position changes, low otherwise
    const posChanging = sec > 0 && getPosition(sec - 1) !== pos;
    const movement = posChanging
      ? gaussianNoise(2.5, 0.5)
      : clamp(gaussianNoise(0.2, 0.1), 0, 0.8);

    // Apnea cluster: cause periodic SpO2 dips and events
    // Each "event" is a ~15-20 second window where spo2 drops then recovers
    const eventPeriod = 120 - Math.round(clusterI * 60); // more frequent in high clusters
    const inEvent = clusterI > 0.2 && (sec % eventPeriod < 20);
    const eventPhase = inEvent ? (sec % eventPeriod) / 20 : 0; // 0..1 during event

    // SpO2 target
    let spo2Target;
    if (inGap) {
      spo2Target = null;
    } else if (inEvent && clusterI > 0.2) {
      // During event: drop from baseline, nadir at 50%, recover
      const dropDepth = 3 + clusterI * 8; // 3-11% drop
      if (eventPhase < 0.5) {
        spo2Target = 97 - dropDepth * (eventPhase / 0.5);
      } else {
        spo2Target = (97 - dropDepth) + dropDepth * ((eventPhase - 0.5) / 0.5);
      }
      spo2Target = clamp(spo2Target, 82, 99);
    } else {
      // Normal: slight drift around 96-98
      spo2Target = clamp(gaussianNoise(97, 0.5), 93, 100);
    }

    // Smooth SpO2 (can't change instantly)
    if (spo2Target !== null) {
      spo2 = spo2 + (spo2Target - spo2) * 0.1 + gaussianNoise(0, 0.15);
      spo2 = clamp(spo2, 80, 100);
    }

    // HR: baseline 58-62, spike after events
    const hrBase = 60 + clusterI * 5;
    if (inEvent && eventPhase > 0.6) {
      hr = hr + (75 - hr) * 0.05 + gaussianNoise(0, 0.5);
    } else {
      hr = hr + (hrBase - hr) * 0.02 + gaussianNoise(0, 0.5);
    }
    hr = clamp(hr, 45, 100);

    // Snore: higher when supine or in cluster
    const snoreBase = pos === 'supine' ? cfg.baseSnore * 1.8 : cfg.baseSnore;
    const snoreTarget = inEvent ? cfg.clusterSnore : snoreBase + gaussianNoise(0, 5);
    snore = snore + (snoreTarget - snore) * 0.05 + gaussianNoise(0, 2);
    snore = clamp(snore, 0, 100);

    // Generate events at key moments
    if (inEvent && clusterI > 0.3 && sec % eventPeriod === 5) {
      const dropAmt = Math.round(clusterI * 8 * 10) / 10;
      const duration = 14 + Math.round(clusterI * 8);
      const severity = dropAmt >= 5 ? 'moderate' : 'mild';

      // desat event
      events.push({
        event_type: 'desat',
        severity,
        timestamp: ts,
        details: `drop=${dropAmt}%,duration=${duration}s`,
      });

      // apnea_suspected if serious enough
      if (dropAmt >= 4) {
        events.push({
          event_type: 'apnea_suspected',
          severity: dropAmt >= 6 ? 'moderate' : 'mild',
          timestamp: ts + 5000,
          details: `position=${pos},spo2_nadir=${Math.round(spo2 * 10) / 10}`,
        });
      }
    }

    // Gasp events: at event recovery phase
    if (inEvent && clusterI > 0.4 && sec % eventPeriod === 18) {
      events.push({
        event_type: 'gasp',
        severity: 'mild',
        timestamp: ts,
        details: `position=${pos}`,
      });
    }

    // Snore burst events
    if (snore > 60 && sec % 45 === 0 && clusterI > 0.1) {
      events.push({
        event_type: 'snore_burst',
        severity: 'mild',
        timestamp: ts,
        details: `intensity=${Math.round(snore)}`,
      });
    }

    // Haptic fired (when apnea suspected + haptic enabled)
    if (inEvent && clusterI > 0.5 && sec % eventPeriod === 12) {
      events.push({
        event_type: 'haptic_fired',
        severity: 'info',
        timestamp: ts,
        details: 'triggered_by=apnea_suspected',
      });
    }

    const signalQuality = inGap ? 'poor' : movement > 1.5 ? 'fair' : 'good';

    samples.push({
      signal_quality: signalQuality,
      timestamp: ts,
      spo2: inGap ? null : Math.round(spo2 * 10) / 10,
      hr: Math.round(hr * 10) / 10,
      snore: Math.round(snore * 10) / 10,
      position: pos,
      movement: Math.round(movement * 100) / 100,
      airflow_state: sec % 4 < 2 ? 'inhale' : 'exhale',
    });
  }

  return { samples, events };
}

module.exports = { generateNight };
