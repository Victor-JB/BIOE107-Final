'use strict';

function computeRiskLevel(ahi) {
  if (ahi < 5) return 'low';
  if (ahi < 15) return 'moderate';
  return 'high';
}

function computeRiskLabel(ahi) {
  if (ahi < 5) return { level: 'low', label: 'Low indication of sleep-disordered breathing', guidance: 'No significant patterns were detected last night. If you have concerns, consider discussing with a healthcare provider.' };
  if (ahi < 15) return { level: 'moderate', label: 'Moderate indication of sleep-disordered breathing', guidance: 'Patterns consistent with occasional breathing disruptions were detected. Worth discussing with a healthcare provider.' };
  if (ahi < 30) return { level: 'high', label: 'Elevated indication of sleep-disordered breathing', guidance: 'Patterns consistent with frequent breathing disruptions were detected. We recommend consulting a sleep physician.' };
  return { level: 'high', label: 'High indication of sleep-disordered breathing', guidance: 'Significant patterns were detected. We recommend consulting a sleep physician for a comprehensive evaluation.' };
}

function analyzeNight(samples, events) {
  if (!samples.length) return null;

  const totalSeconds = (samples[samples.length - 1].timestamp - samples[0].timestamp) / 1000;
  const totalHours = totalSeconds / 3600;

  // SpO2 stats (exclude nulls = signal dropout)
  const validSpo2 = samples.filter(s => s.spo2 !== null && s.spo2 > 70 && s.spo2 <= 100).map(s => s.spo2);
  const minSpo2 = validSpo2.length ? Math.min(...validSpo2) : null;
  const avgSpo2 = validSpo2.length ? validSpo2.reduce((a, b) => a + b, 0) / validSpo2.length : null;
  const signalQualityPct = validSpo2.length / samples.length * 100;

  // Snore percentage (snore > 25 = snoring)
  const snoringSamples = samples.filter(s => s.snore > 25).length;
  const snorePct = snoringSamples / samples.length * 100;

  // Position breakdown
  const positionCounts = {};
  for (const s of samples) {
    positionCounts[s.position] = (positionCounts[s.position] || 0) + 1;
  }
  const positionBreakdown = {};
  for (const [pos, count] of Object.entries(positionCounts)) {
    positionBreakdown[pos] = Math.round(count / samples.length * 100);
  }

  // AHI: count apnea_suspected + desat events per hour
  const apneaEvents = events.filter(e => ['apnea_suspected', 'desat'].includes(e.event_type));
  const estimatedAhi = totalHours > 0 ? apneaEvents.length / totalHours : 0;

  const risk = computeRiskLabel(estimatedAhi);

  return {
    date: null, // filled by caller
    start_ts: samples[0].timestamp,
    end_ts: samples[samples.length - 1].timestamp,
    total_seconds: Math.round(totalSeconds),
    estimated_ahi: Math.round(estimatedAhi * 10) / 10,
    risk_level: risk.level,
    risk_label: risk.label,
    risk_guidance: risk.guidance,
    min_spo2: minSpo2 ? Math.round(minSpo2 * 10) / 10 : null,
    avg_spo2: avgSpo2 ? Math.round(avgSpo2 * 10) / 10 : null,
    snore_pct: Math.round(snorePct),
    position_breakdown: positionBreakdown,
    event_count: events.length,
    signal_quality_pct: Math.round(signalQualityPct),
  };
}

module.exports = { analyzeNight, computeRiskLabel };
