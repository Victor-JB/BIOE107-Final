'use strict';
const Database = require('better-sqlite3');
const path = require('path');
const fs = require('fs');

const DATA_DIR = path.join(__dirname, 'data');
if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR);

const db = new Database(path.join(DATA_DIR, 'sleep.db'));

db.exec(`
  CREATE TABLE IF NOT EXISTS nights (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    date TEXT UNIQUE NOT NULL,
    start_ts INTEGER,
    end_ts INTEGER,
    total_seconds INTEGER,
    estimated_ahi REAL,
    risk_level TEXT,
    min_spo2 REAL,
    avg_spo2 REAL,
    snore_pct REAL,
    position_json TEXT,
    event_count INTEGER,
    signal_quality_pct REAL,
    summary_json TEXT
  );

  CREATE TABLE IF NOT EXISTS samples (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    night_date TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    spo2 REAL,
    hr REAL,
    snore REAL,
    position TEXT,
    movement REAL,
    airflow_state TEXT,
    signal_quality TEXT
  );

  CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    night_date TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    event_type TEXT,
    severity TEXT,
    details TEXT
  );

  CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT
  );
`);

// Default settings
const defaultSettings = {
  haptic_enabled: 'true',
  haptic_intensity: '2',
};
for (const [key, value] of Object.entries(defaultSettings)) {
  db.prepare('INSERT OR IGNORE INTO settings (key, value) VALUES (?, ?)').run(key, value);
}

const insertNight = db.prepare(`
  INSERT OR REPLACE INTO nights (date, start_ts, end_ts, total_seconds, estimated_ahi, risk_level, min_spo2, avg_spo2, snore_pct, position_json, event_count, signal_quality_pct, summary_json)
  VALUES (@date, @start_ts, @end_ts, @total_seconds, @estimated_ahi, @risk_level, @min_spo2, @avg_spo2, @snore_pct, @position_json, @event_count, @signal_quality_pct, @summary_json)
`);

const insertSample = db.prepare(`
  INSERT INTO samples (night_date, timestamp, spo2, hr, snore, position, movement, airflow_state, signal_quality)
  VALUES (@night_date, @timestamp, @spo2, @hr, @snore, @position, @movement, @airflow_state, @signal_quality)
`);

const insertEvent = db.prepare(`
  INSERT INTO events (night_date, timestamp, event_type, severity, details)
  VALUES (@night_date, @timestamp, @event_type, @severity, @details)
`);

function saveNight(date, samples, events, summary) {
  db.transaction(() => {
    // Clear existing data for this date
    db.prepare('DELETE FROM samples WHERE night_date = ?').run(date);
    db.prepare('DELETE FROM events WHERE night_date = ?').run(date);

    for (const s of samples) insertSample.run({ night_date: date, ...s });
    for (const e of events) insertEvent.run({ night_date: date, ...e });
    insertNight.run({
      date,
      ...summary,
      position_json: JSON.stringify(summary.position_breakdown),
      summary_json: JSON.stringify(summary),
    });
  })();
}

function getNights() {
  return db.prepare('SELECT * FROM nights ORDER BY date DESC').all().map(row => ({
    ...row,
    position_breakdown: JSON.parse(row.position_json || '{}'),
  }));
}

function getNightSummary(date) {
  return db.prepare('SELECT * FROM nights WHERE date = ?').get(date);
}

function getNightSamples(date) {
  return db.prepare('SELECT * FROM samples WHERE night_date = ? ORDER BY timestamp ASC').all(date);
}

function getNightEvents(date) {
  return db.prepare('SELECT * FROM events WHERE night_date = ? ORDER BY timestamp ASC').all(date);
}

function getSettings() {
  const rows = db.prepare('SELECT * FROM settings').all();
  return Object.fromEntries(rows.map(r => [r.key, r.value]));
}

function updateSettings(updates) {
  const stmt = db.prepare('INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)');
  for (const [key, value] of Object.entries(updates)) {
    stmt.run(key, String(value));
  }
}

module.exports = { saveNight, getNights, getNightSummary, getNightSamples, getNightEvents, getSettings, updateSettings };
