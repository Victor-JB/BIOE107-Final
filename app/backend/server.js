'use strict';
const express = require('express');
const { WebSocketServer } = require('ws');
const http = require('http');
const cors = require('cors');
const path = require('path');

const db = require('./db');
const { generateNight } = require('./mockDevice');
const { analyzeNight, computeRiskLabel } = require('./nightAnalyzer');

const app = express();
app.use(cors());
app.use(express.json());

// ── Seed mock data if no nights exist ──────────────────────────────────────
function seedMockData() {
  const nights = db.getNights();
  if (nights.length > 0) return;

  console.log('Seeding mock historical data...');
  const scenarios = ['moderate', 'low', 'moderate', 'high', 'moderate', 'low', 'moderate'];
  for (let i = 6; i >= 0; i--) {
    const date = new Date();
    date.setDate(date.getDate() - i);
    date.setHours(23, 0, 0, 0);
    const dateStr = date.toISOString().slice(0, 10);
    const scenario = scenarios[6 - i];
    const { samples, events } = generateNight(date, 7.0 + Math.random() * 0.8, scenario);
    const summary = analyzeNight(samples, events);
    summary.date = dateStr;
    const { level, label, guidance } = computeRiskLabel(summary.estimated_ahi);
    summary.risk_level = level;
    summary.risk_label = label;
    summary.risk_guidance = guidance;
    db.saveNight(dateStr, samples, events, summary);
    console.log(`  Seeded ${dateStr} (${scenario}): AHI=${summary.estimated_ahi}`);
  }
}
seedMockData();

// ── Helpers ───────────────────────────────────────────────────────────────

function expandSummary(row) {
  if (!row) return row;
  const extra = row.summary_json ? JSON.parse(row.summary_json) : {};
  return { ...extra, ...row };
}

// ── REST API ──────────────────────────────────────────────────────────────

app.get('/api/nights', (req, res) => {
  res.json(db.getNights().map(expandSummary));
});

app.get('/api/nights/latest', (req, res) => {
  const nights = db.getNights();
  if (!nights.length) return res.status(404).json({ error: 'No data' });
  const latest = expandSummary(nights[0]);
  const samples = db.getNightSamples(latest.date);
  const events = db.getNightEvents(latest.date);
  res.json({ summary: latest, samples, events });
});

app.get('/api/nights/:date', (req, res) => {
  const { date } = req.params;
  const summary = expandSummary(db.getNightSummary(date));
  if (!summary) return res.status(404).json({ error: 'Night not found' });
  const samples = db.getNightSamples(date);
  const events = db.getNightEvents(date);
  res.json({ summary, samples, events });
});

app.post('/api/generate', (req, res) => {
  const scenario = req.body.scenario || 'moderate';
  const dateStr = new Date().toISOString().slice(0, 10);
  const startDate = new Date();
  startDate.setHours(23, 0, 0, 0);
  const { samples, events } = generateNight(startDate, 7.5, scenario);
  const summary = analyzeNight(samples, events);
  summary.date = dateStr;
  const { level, label, guidance } = computeRiskLabel(summary.estimated_ahi);
  summary.risk_level = level;
  summary.risk_label = label;
  summary.risk_guidance = guidance;
  db.saveNight(dateStr, samples, events, summary);
  res.json({ date: dateStr, summary });
});

app.get('/api/settings', (req, res) => {
  res.json(db.getSettings());
});

app.put('/api/settings', (req, res) => {
  db.updateSettings(req.body);
  res.json(db.getSettings());
});

// ── WebSocket live stream ─────────────────────────────────────────────────
const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/live' });

wss.on('connection', (ws) => {
  console.log('Live view client connected');
  let sec = 0;
  let spo2 = 97, hr = 62, snore = 10;
  let phase = 0;

  const interval = setInterval(() => {
    if (ws.readyState !== ws.OPEN) { clearInterval(interval); return; }

    // Gentle oscillation to keep the live view interesting
    phase += 0.02;
    spo2 += (97 - spo2) * 0.05 + (Math.random() - 0.5) * 0.4;
    hr += (62 - hr) * 0.02 + (Math.random() - 0.5) * 0.5;
    snore += (12 - snore) * 0.03 + (Math.random() - 0.5) * 2;
    spo2 = Math.max(90, Math.min(100, spo2));
    hr = Math.max(45, Math.min(100, hr));
    snore = Math.max(0, Math.min(100, snore));

    const sample = {
      type: 'sample',
      timestamp: Date.now(),
      spo2: Math.round(spo2 * 10) / 10,
      hr: Math.round(hr * 10) / 10,
      snore: Math.round(snore * 10) / 10,
      position: sec % 200 < 120 ? 'supine' : 'left',
      movement: Math.round(Math.abs((Math.random() - 0.5) * 0.4) * 100) / 100,
      airflow_state: sec % 4 < 2 ? 'inhale' : 'exhale',
      signal_quality: 'good',
    };
    ws.send(JSON.stringify(sample));
    sec++;
  }, 1000);

  ws.on('close', () => { clearInterval(interval); console.log('Live view client disconnected'); });
});

const PORT = process.env.PORT || 3001;
server.listen(PORT, () => console.log(`Backend running on http://localhost:${PORT}`));
