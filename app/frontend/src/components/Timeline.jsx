import React, { useEffect, useState, useCallback, useMemo } from 'react';
import { useSearchParams } from 'react-router-dom';
import {
  ComposedChart, Line, Area, XAxis, YAxis, CartesianGrid, Tooltip,
  ResponsiveContainer, ReferenceLine, ReferenceArea, Scatter, ScatterChart
} from 'recharts';

const POSITION_COLORS = {
  supine: '#60a5fa',
  left: '#34d399',
  right: '#a78bfa',
  prone: '#f87171',
  upright: '#fbbf24',
};

const EVENT_COLORS = {
  desat: '#ef4444',
  apnea_suspected: '#f97316',
  gasp: '#fbbf24',
  snore_burst: '#a78bfa',
  haptic_fired: '#34d399',
  position_change: '#60a5fa',
};

function fmtTime(ms) {
  if (!ms) return '';
  const d = new Date(ms);
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function downsample(arr, maxPoints) {
  if (arr.length <= maxPoints) return arr;
  const step = Math.ceil(arr.length / maxPoints);
  return arr.filter((_, i) => i % step === 0);
}

function CustomTooltip({ active, payload, label }) {
  if (!active || !payload?.length) return null;
  return (
    <div className="text-xs rounded-lg p-2.5 shadow-xl" style={{ background: '#161b22', border: '1px solid #30363d' }}>
      <div className="text-gray-400 mb-1">{fmtTime(label)}</div>
      {payload.map(p => (
        <div key={p.dataKey} className="flex justify-between gap-3" style={{ color: p.color }}>
          <span>{p.name}</span>
          <span className="font-mono">{p.value != null ? p.value : '—'}</span>
        </div>
      ))}
    </div>
  );
}

export default function Timeline() {
  const [searchParams] = useSearchParams();
  const date = searchParams.get('date');
  const [data, setData] = useState(null);
  const [loading, setLoading] = useState(true);
  const [selectedEvent, setSelectedEvent] = useState(null);

  useEffect(() => {
    const url = date ? `/api/nights/${date}` : '/api/nights/latest';
    fetch(url)
      .then(r => r.json())
      .then(setData)
      .catch(console.error)
      .finally(() => setLoading(false));
  }, [date]);

  const { chartData, gapRanges, eventMarkers } = useMemo(() => {
    if (!data) return { chartData: [], gapRanges: [], eventMarkers: [] };

    const samples = data.samples || [];
    const ds = downsample(samples, 1200);

    // Identify contiguous gap ranges for ReferenceArea
    const gaps = [];
    let gapStart = null;
    for (let i = 0; i < samples.length; i++) {
      if (samples[i].spo2 === null && gapStart === null) gapStart = samples[i].timestamp;
      if (samples[i].spo2 !== null && gapStart !== null) {
        gaps.push({ x1: gapStart, x2: samples[i].timestamp });
        gapStart = null;
      }
    }
    if (gapStart !== null && samples.length) gaps.push({ x1: gapStart, x2: samples[samples.length - 1].timestamp });

    const chartData = ds.map(s => ({
      ts: s.timestamp,
      spo2: s.spo2,
      hr: s.hr,
      snore: s.snore,
      position: s.position,
      movement: s.movement,
    }));

    const eventMarkers = (data.events || []).filter(e =>
      ['desat', 'apnea_suspected', 'gasp', 'snore_burst'].includes(e.event_type)
    );

    return { chartData, gapRanges: gaps, eventMarkers };
  }, [data]);

  const handleEventClick = useCallback((event) => {
    // Find the closest sample to this event
    const samples = data?.samples || [];
    const closest = samples.reduce((best, s) =>
      Math.abs(s.timestamp - event.timestamp) < Math.abs((best?.timestamp ?? Infinity) - event.timestamp) ? s : best
    , null);
    setSelectedEvent({ event, sample: closest });
  }, [data]);

  if (loading) return <div className="text-gray-500 text-sm py-12 text-center">Loading timeline…</div>;
  if (!data) return <div className="text-gray-500 text-sm py-12 text-center">No data</div>;

  const { summary } = data;
  const dateLabel = summary?.date || date || 'Last night';

  return (
    <div className="space-y-4">
      <div>
        <h1 className="text-lg font-semibold text-white">Night timeline</h1>
        <p className="text-xs text-gray-500">{dateLabel} · {chartData.length} data points shown</p>
      </div>

      {/* Signal quality notice */}
      {gapRanges.length > 0 && (
        <div className="rounded-lg p-2.5 flex gap-2 text-xs" style={{ background: 'rgba(100,100,100,0.1)', border: '1px solid rgba(100,100,100,0.2)' }}>
          <span className="text-gray-400 shrink-0">ⓘ</span>
          <span className="text-gray-400">Gray bands indicate periods where SpO₂ signal was unavailable (motion artifact or sensor contact loss). These are shown as gaps, not estimated values.</span>
        </div>
      )}

      {/* Legend */}
      <div className="flex flex-wrap gap-3 text-xs text-gray-400">
        {Object.entries(EVENT_COLORS).filter(([k]) => ['desat','apnea_suspected','gasp'].includes(k)).map(([k, c]) => (
          <div key={k} className="flex items-center gap-1">
            <span className="w-2 h-2 rounded-full" style={{ background: c }} />
            {k.replace('_', ' ')}
          </div>
        ))}
        <div className="flex items-center gap-1">
          <span className="w-4 h-2 rounded" style={{ background: 'rgba(120,120,120,0.3)' }} />
          signal gap
        </div>
      </div>

      {/* SpO2 Chart */}
      <div>
        <div className="text-xs text-gray-400 mb-1 font-medium">Blood oxygen (SpO₂)</div>
        <ResponsiveContainer width="100%" height={160}>
          <ComposedChart data={chartData} syncId="night" margin={{ top: 4, right: 10, bottom: 0, left: 0 }}>
            <CartesianGrid strokeDasharray="3 3" />
            <XAxis dataKey="ts" tickFormatter={fmtTime} tick={{ fontSize: 10 }} tickLine={false} axisLine={false} minTickGap={80} />
            <YAxis domain={[80, 100]} tickCount={5} tick={{ fontSize: 10 }} tickLine={false} axisLine={false} width={32} unit="%" />
            <Tooltip content={<CustomTooltip />} />
            {gapRanges.map((g, i) => (
              <ReferenceArea key={i} x1={g.x1} x2={g.x2} fill="rgba(150,150,150,0.15)" stroke="none" />
            ))}
            {eventMarkers.map((e, i) => (
              <ReferenceLine
                key={i}
                x={e.timestamp}
                stroke={EVENT_COLORS[e.event_type] || '#888'}
                strokeWidth={1.5}
                strokeDasharray="3 3"
                strokeOpacity={0.7}
              />
            ))}
            <Line
              dataKey="spo2"
              stroke="#60a5fa"
              strokeWidth={1.5}
              dot={false}
              connectNulls={false}
              name="SpO₂"
              unit="%"
            />
          </ComposedChart>
        </ResponsiveContainer>
      </div>

      {/* HR Chart */}
      <div>
        <div className="text-xs text-gray-400 mb-1 font-medium">Heart rate</div>
        <ResponsiveContainer width="100%" height={120}>
          <ComposedChart data={chartData} syncId="night" margin={{ top: 4, right: 10, bottom: 0, left: 0 }}>
            <CartesianGrid strokeDasharray="3 3" />
            <XAxis dataKey="ts" tickFormatter={fmtTime} tick={{ fontSize: 10 }} tickLine={false} axisLine={false} minTickGap={80} />
            <YAxis domain={[40, 100]} tickCount={4} tick={{ fontSize: 10 }} tickLine={false} axisLine={false} width={32} unit=" bpm" />
            <Tooltip content={<CustomTooltip />} />
            <Line dataKey="hr" stroke="#34d399" strokeWidth={1.5} dot={false} name="HR" unit=" bpm" />
          </ComposedChart>
        </ResponsiveContainer>
      </div>

      {/* Snore Chart */}
      <div>
        <div className="text-xs text-gray-400 mb-1 font-medium">Snore intensity</div>
        <ResponsiveContainer width="100%" height={90}>
          <ComposedChart data={chartData} syncId="night" margin={{ top: 4, right: 10, bottom: 0, left: 0 }}>
            <CartesianGrid strokeDasharray="3 3" />
            <XAxis dataKey="ts" tickFormatter={fmtTime} tick={{ fontSize: 10 }} tickLine={false} axisLine={false} minTickGap={80} />
            <YAxis domain={[0, 100]} tickCount={3} tick={{ fontSize: 10 }} tickLine={false} axisLine={false} width={32} />
            <Tooltip content={<CustomTooltip />} />
            <Area dataKey="snore" stroke="#a78bfa" fill="rgba(167,139,250,0.15)" strokeWidth={1} name="Snore" />
          </ComposedChart>
        </ResponsiveContainer>
      </div>

      {/* Position Band */}
      <div>
        <div className="text-xs text-gray-400 mb-1 font-medium">Sleep position</div>
        <div className="rounded overflow-hidden h-5 flex">
          {chartData.map((d, i) => (
            <div
              key={i}
              style={{
                flex: 1,
                background: POSITION_COLORS[d.position] || '#444',
                minWidth: 0,
              }}
              title={`${fmtTime(d.ts)}: ${d.position}`}
            />
          ))}
        </div>
        <div className="flex flex-wrap gap-3 mt-1.5">
          {Object.entries(POSITION_COLORS).map(([pos, color]) => (
            <div key={pos} className="flex items-center gap-1 text-xs text-gray-500">
              <span className="w-2 h-2 rounded-sm" style={{ background: color }} />
              {pos}
            </div>
          ))}
        </div>
      </div>

      {/* Event List */}
      <div>
        <div className="text-xs text-gray-400 mb-2 font-medium">Detected events ({eventMarkers.length})</div>
        <div className="space-y-1.5 max-h-64 overflow-y-auto">
          {eventMarkers.length === 0 && (
            <div className="text-xs text-gray-600 py-4 text-center">No significant events detected</div>
          )}
          {eventMarkers.map((e, i) => (
            <button
              key={i}
              onClick={() => handleEventClick(e)}
              className="w-full text-left rounded-lg p-2.5 flex items-start gap-2.5 text-xs transition-colors hover:opacity-80"
              style={{ background: '#161b22', border: `1px solid ${EVENT_COLORS[e.event_type]}33` }}
            >
              <span className="w-1.5 h-1.5 rounded-full mt-1 shrink-0" style={{ background: EVENT_COLORS[e.event_type] }} />
              <div className="flex-1 min-w-0">
                <div className="flex items-center justify-between gap-2">
                  <span className="font-medium" style={{ color: EVENT_COLORS[e.event_type] }}>
                    {e.event_type.replace('_', ' ')}
                  </span>
                  <span className="text-gray-500">{fmtTime(e.timestamp)}</span>
                </div>
                <div className="text-gray-500 truncate">{e.details}</div>
              </div>
            </button>
          ))}
        </div>
      </div>

      {/* Event Detail Modal */}
      {selectedEvent && (
        <div
          className="fixed inset-0 flex items-center justify-center z-50 p-4"
          style={{ background: 'rgba(0,0,0,0.7)' }}
          onClick={() => setSelectedEvent(null)}
        >
          <div
            className="rounded-xl p-5 max-w-sm w-full shadow-2xl"
            style={{ background: '#161b22', border: '1px solid #30363d' }}
            onClick={e => e.stopPropagation()}
          >
            <div className="flex items-center justify-between mb-3">
              <span
                className="text-sm font-semibold capitalize"
                style={{ color: EVENT_COLORS[selectedEvent.event.event_type] }}
              >
                {selectedEvent.event.event_type.replace('_', ' ')}
              </span>
              <button onClick={() => setSelectedEvent(null)} className="text-gray-500 hover:text-white text-lg leading-none">×</button>
            </div>
            <div className="space-y-2 text-xs">
              <div className="flex justify-between">
                <span className="text-gray-500">Time</span>
                <span className="text-gray-300">{fmtTime(selectedEvent.event.timestamp)}</span>
              </div>
              <div className="flex justify-between">
                <span className="text-gray-500">Severity</span>
                <span className="text-gray-300 capitalize">{selectedEvent.event.severity}</span>
              </div>
              <div className="flex justify-between">
                <span className="text-gray-500">Details</span>
                <span className="text-gray-300">{selectedEvent.event.details}</span>
              </div>
              {selectedEvent.sample && (
                <>
                  <div style={{ borderTop: '1px solid #21262d', paddingTop: 8, marginTop: 8 }}>
                    <div className="text-gray-500 mb-1.5">Sensor readings at this time</div>
                    {selectedEvent.sample.spo2 != null && (
                      <div className="flex justify-between">
                        <span className="text-gray-500">SpO₂</span>
                        <span className="text-blue-400">{selectedEvent.sample.spo2}%</span>
                      </div>
                    )}
                    <div className="flex justify-between">
                      <span className="text-gray-500">Heart rate</span>
                      <span className="text-green-400">{selectedEvent.sample.hr} bpm</span>
                    </div>
                    <div className="flex justify-between">
                      <span className="text-gray-500">Position</span>
                      <span className="text-gray-300 capitalize">{selectedEvent.sample.position}</span>
                    </div>
                    <div className="flex justify-between">
                      <span className="text-gray-500">Snore intensity</span>
                      <span className="text-gray-300">{selectedEvent.sample.snore}</span>
                    </div>
                  </div>
                </>
              )}
            </div>
            <p className="mt-4 text-xs text-gray-600 leading-relaxed">
              This event was detected by the device's screening algorithm. It does not constitute a medical finding. Consult a healthcare provider for clinical evaluation.
            </p>
          </div>
        </div>
      )}
    </div>
  );
}
