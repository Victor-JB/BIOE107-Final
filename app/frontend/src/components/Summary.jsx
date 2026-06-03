import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import RiskIndicator from './RiskIndicator.jsx';

function StatCard({ label, value, unit, sub, color }) {
  return (
    <div className="rounded-lg p-4" style={{ background: '#161b22', border: '1px solid #21262d' }}>
      <div className="text-xs text-gray-500 mb-1">{label}</div>
      <div className="flex items-baseline gap-1">
        <span className="text-2xl font-bold" style={{ color: color || '#e6edf3' }}>{value ?? '—'}</span>
        {unit && <span className="text-xs text-gray-500">{unit}</span>}
      </div>
      {sub && <div className="text-xs text-gray-600 mt-0.5">{sub}</div>}
    </div>
  );
}

function PositionBar({ breakdown }) {
  const colors = { supine: '#60a5fa', left: '#34d399', right: '#a78bfa', prone: '#f87171', upright: '#fbbf24' };
  const entries = Object.entries(breakdown || {}).filter(([, v]) => v > 0);
  if (!entries.length) return null;
  return (
    <div>
      <div className="text-xs text-gray-500 mb-2">Sleep position</div>
      <div className="flex rounded overflow-hidden h-5">
        {entries.map(([pos, pct]) => (
          <div key={pos} style={{ width: `${pct}%`, background: colors[pos] || '#888', minWidth: 2 }} title={`${pos}: ${pct}%`} />
        ))}
      </div>
      <div className="flex flex-wrap gap-3 mt-2">
        {entries.map(([pos, pct]) => (
          <div key={pos} className="flex items-center gap-1 text-xs text-gray-400">
            <span className="w-2 h-2 rounded-sm inline-block" style={{ background: colors[pos] || '#888' }} />
            {pos} {pct}%
          </div>
        ))}
      </div>
    </div>
  );
}

function fmtDuration(seconds) {
  if (!seconds) return '—';
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return `${h}h ${m}m`;
}

export default function Summary() {
  const [data, setData] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);
  const navigate = useNavigate();

  useEffect(() => {
    fetch('/api/nights/latest')
      .then(r => r.ok ? r.json() : Promise.reject('No data'))
      .then(setData)
      .catch(e => setError(String(e)))
      .finally(() => setLoading(false));
  }, []);

  if (loading) return <div className="text-gray-500 text-sm py-12 text-center">Loading last night's data…</div>;
  if (error) return <div className="text-red-400 text-sm py-12 text-center">{error}</div>;
  if (!data) return null;

  const { summary } = data;
  const posBreakdown = typeof summary.position_breakdown === 'string'
    ? JSON.parse(summary.position_breakdown)
    : summary.position_breakdown || {};
  const signalOk = summary.signal_quality_pct >= 80;

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-lg font-semibold text-white">Last night</h1>
        <p className="text-xs text-gray-500">{summary.date}</p>
      </div>

      <RiskIndicator
        level={summary.risk_level}
        label={summary.risk_label || 'Screening estimate unavailable'}
        guidance={summary.risk_guidance || 'Consult a healthcare provider with any concerns.'}
        ahi={summary.estimated_ahi}
      />

      {!signalOk && (
        <div className="rounded-lg p-3 flex gap-2 text-xs" style={{ background: 'rgba(245,158,11,0.08)', border: '1px solid rgba(245,158,11,0.2)' }}>
          <span className="text-yellow-400 shrink-0">⚠</span>
          <span className="text-yellow-200">Signal quality was reduced last night ({summary.signal_quality_pct}% valid readings). Results may be less accurate.</span>
        </div>
      )}

      <div className="grid grid-cols-2 sm:grid-cols-3 gap-3">
        <StatCard label="Sleep duration" value={fmtDuration(summary.total_seconds)} sub="estimated" />
        <StatCard
          label="Events per hour"
          value={summary.estimated_ahi != null ? `~${summary.estimated_ahi}` : '—'}
          unit="events/hr"
          sub="estimated AHI"
          color={summary.estimated_ahi >= 15 ? '#ef4444' : summary.estimated_ahi >= 5 ? '#f59e0b' : '#22c55e'}
        />
        <StatCard label="Lowest SpO₂" value={summary.min_spo2} unit="%" color={summary.min_spo2 < 90 ? '#ef4444' : summary.min_spo2 < 94 ? '#f59e0b' : '#e6edf3'} />
        <StatCard label="Avg SpO₂" value={summary.avg_spo2} unit="%" />
        <StatCard label="Snoring" value={summary.snore_pct} unit="%" sub="of sleep time" />
        <StatCard label="Signal quality" value={summary.signal_quality_pct} unit="%" sub="valid readings" color={signalOk ? '#22c55e' : '#f59e0b'} />
      </div>

      <div className="rounded-lg p-4" style={{ background: '#161b22', border: '1px solid #21262d' }}>
        <PositionBar breakdown={posBreakdown} />
      </div>

      <button
        onClick={() => navigate(`/timeline?date=${summary.date}`)}
        className="w-full py-2.5 rounded-lg text-sm font-medium text-white transition-colors"
        style={{ background: '#1f6feb', border: '1px solid #388bfd' }}
      >
        View full night timeline →
      </button>
    </div>
  );
}
