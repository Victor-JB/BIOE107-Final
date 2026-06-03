import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts';

const RISK_COLORS = { low: '#22c55e', moderate: '#f59e0b', high: '#ef4444' };

function fmtDuration(s) {
  if (!s) return '—';
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
}

export default function History() {
  const [nights, setNights] = useState([]);
  const [loading, setLoading] = useState(true);
  const navigate = useNavigate();

  useEffect(() => {
    fetch('/api/nights')
      .then(r => r.json())
      .then(setNights)
      .catch(console.error)
      .finally(() => setLoading(false));
  }, []);

  if (loading) return <div className="text-gray-500 text-sm py-12 text-center">Loading history…</div>;

  const chartData = [...nights].reverse().map(n => ({
    date: n.date?.slice(5), // MM-DD
    ahi: n.estimated_ahi,
    risk: n.risk_level,
  }));

  // Position correlation: nights with high supine % vs AHI
  const supineNights = nights.filter(n => {
    const pb = typeof n.position_json === 'string' ? JSON.parse(n.position_json || '{}') : (n.position_breakdown || {});
    return (pb.supine || 0) > 60;
  });
  const nonSupineNights = nights.filter(n => {
    const pb = typeof n.position_json === 'string' ? JSON.parse(n.position_json || '{}') : (n.position_breakdown || {});
    return (pb.supine || 0) <= 60;
  });
  const avgAhiSupine = supineNights.length
    ? supineNights.reduce((s, n) => s + (n.estimated_ahi || 0), 0) / supineNights.length
    : null;
  const avgAhiNonSupine = nonSupineNights.length
    ? nonSupineNights.reduce((s, n) => s + (n.estimated_ahi || 0), 0) / nonSupineNights.length
    : null;
  const positionInsight = avgAhiSupine && avgAhiNonSupine && avgAhiSupine > avgAhiNonSupine * 1.3
    ? `Events appear ~${Math.round(avgAhiSupine / avgAhiNonSupine)}× more frequent during back-sleeping nights.`
    : null;

  return (
    <div className="space-y-5">
      <h1 className="text-lg font-semibold text-white">History</h1>

      {/* Trend chart */}
      <div className="rounded-lg p-4" style={{ background: '#161b22', border: '1px solid #21262d' }}>
        <div className="text-xs text-gray-400 mb-3 font-medium">Estimated events per hour — 7-night trend</div>
        <ResponsiveContainer width="100%" height={140}>
          <LineChart data={chartData} margin={{ top: 4, right: 10, bottom: 0, left: 0 }}>
            <CartesianGrid strokeDasharray="3 3" />
            <XAxis dataKey="date" tick={{ fontSize: 10 }} tickLine={false} axisLine={false} />
            <YAxis tick={{ fontSize: 10 }} tickLine={false} axisLine={false} width={28} />
            <Tooltip
              contentStyle={{ background: '#0d1117', border: '1px solid #30363d', borderRadius: 6 }}
              labelStyle={{ color: '#8b949e', fontSize: 11 }}
              itemStyle={{ color: '#60a5fa', fontSize: 11 }}
              formatter={v => [`~${v}`, 'Events/hr']}
            />
            <Line dataKey="ahi" stroke="#60a5fa" strokeWidth={2} dot={{ r: 3, fill: '#60a5fa' }} connectNulls />
          </LineChart>
        </ResponsiveContainer>
        <p className="text-xs text-gray-600 mt-2">Estimated AHI is a screening metric only. Clinical diagnosis requires a polysomnogram evaluated by a sleep physician.</p>
      </div>

      {/* Position insight */}
      {positionInsight && (
        <div className="rounded-lg p-3 flex gap-2 text-xs" style={{ background: 'rgba(96,165,250,0.08)', border: '1px solid rgba(96,165,250,0.2)' }}>
          <span className="text-blue-400 shrink-0">◎</span>
          <span className="text-blue-200">{positionInsight} Positional patterns may be worth discussing with a healthcare provider.</span>
        </div>
      )}

      {/* Night list */}
      <div className="space-y-2">
        {nights.map(n => (
          <button
            key={n.date}
            onClick={() => navigate(`/timeline?date=${n.date}`)}
            className="w-full text-left rounded-lg p-3.5 flex items-center gap-3 transition-colors hover:opacity-80"
            style={{ background: '#161b22', border: '1px solid #21262d' }}
          >
            <div
              className="w-2 h-8 rounded-full shrink-0"
              style={{ background: RISK_COLORS[n.risk_level] || '#888' }}
            />
            <div className="flex-1 min-w-0">
              <div className="flex items-center justify-between">
                <span className="text-sm font-medium text-white">{n.date}</span>
                <span className="text-xs text-gray-400">{fmtDuration(n.total_seconds)}</span>
              </div>
              <div className="flex gap-3 mt-0.5 text-xs text-gray-500">
                <span>~{n.estimated_ahi} events/hr</span>
                <span>min SpO₂: {n.min_spo2}%</span>
                <span className="capitalize">{n.risk_level} indication</span>
              </div>
            </div>
            <span className="text-gray-600 text-xs">→</span>
          </button>
        ))}
      </div>
    </div>
  );
}
