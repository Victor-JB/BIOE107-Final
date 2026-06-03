'use strict';

function parseLine(line) {
  line = line.trim();
  if (!line) return null;

  const parts = line.split(',');
  if (parts[0] === 'S' && parts.length === 8) {
    const spo2 = parseFloat(parts[2]);
    return {
      type: 'sample',
      timestamp: parseInt(parts[1], 10),
      spo2: isNaN(spo2) ? null : spo2,
      hr: parseFloat(parts[3]),
      snore: parseFloat(parts[4]),
      position: parts[5],
      movement: parseFloat(parts[6]),
      airflow_state: parts[7],
    };
  }
  if (parts[0] === 'E' && parts.length >= 4) {
    return {
      type: 'event',
      timestamp: parseInt(parts[1], 10),
      event_type: parts[2],
      severity: parts[3],
      details: parts.slice(4).join(','),
    };
  }
  return null;
}

module.exports = { parseLine };
