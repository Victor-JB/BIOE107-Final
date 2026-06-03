import React, { useEffect, useState, useRef } from 'react';

function BigStat({ label, value, unit, color, quality }) {
  return (
    <div className="rounded-xl p-5 flex flex-col" style={{ background: '#161b22', border: '1px solid #21262d' }}>
      <div className="text-xs text-gray-500 mb-2">{label}</div>
      <div className="flex items-baseline gap-1 flex-1">
        <span className="text-4xl font-bold tabular-nums" style={{ color: color || '#e6edf3' }}>
          {value ?? '—'}
        </span>
        {unit && <span className="text-sm text-gray-500">{unit}</span>}
      </div>
      {quality && (
        <div className="mt-2 text-xs" style={{ color: quality === 'good' ? '#22c55e' : quality === 'fair' ? '#f59e0b' : '#ef4444' }}>
          {quality} signal
        </div>
      )}
    </div>
  );
}

const POSITION_ICONS = { supine: '🔵', left: '🟢', right: '🟣', prone: '🔴', upright: '🟡' };

export default function LiveView() {
  const [latest, setLatest] = useState(null);
  const [connected, setConnected] = useState(false);
  const [history, setHistory] = useState([]);
  const wsRef = useRef(null);

  useEffect(() => {
    const ws = new WebSocket(`ws://${window.location.host}/live`);
    wsRef.current = ws;

    ws.onopen = () => setConnected(true);
    ws.onclose = () => setConnected(false);
    ws.onerror = () => setConnected(false);
    ws.onmessage = (e) => {
      try {
        const data = JSON.parse(e.data);
        setLatest(data);
        setHistory(prev => [...prev.slice(-120), data]);
      } catch {}
    };

    return () => ws.close();
  }, []);

  const spo2Color = !latest?.spo2 ? '#888'
    : latest.spo2 < 90 ? '#ef4444'
    : latest.spo2 < 94 ? '#f59e0b'
    : '#60a5fa';

  return (
    <div className="space-y-5">
      <div className="flex items-center justify-between">
        <h1 className="text-lg font-semibold text-white">Live monitoring</h1>
        <div className="flex items-center gap-1.5 text-xs">
          <span
            className="w-2 h-2 rounded-full"
            style={{ background: connected ? '#22c55e' : '#ef4444', boxShadow: connected ? '0 0 4px #22c55e' : 'none' }}
          />
          <span className={connected ? 'text-green-400' : 'text-red-400'}>
            {connected ? 'Connected' : 'Disconnected'}
          </span>
        </div>
      </div>

      {!connected && (
        <div className="rounded-lg p-3 flex gap-2 text-xs" style={{ background: 'rgba(239,68,68,0.08)', border: '1px solid rgba(239,68,68,0.2)' }}>
          <span className="text-red-400 shrink-0">⚠</span>
          <span className="text-red-300">Not connected to device. Make sure the backend is running and the device is connected.</span>
        </div>
      )}

      <div className="grid grid-cols-2 gap-3">
        <BigStat
          label="Blood oxygen (SpO₂)"
          value={latest?.spo2}
          unit="%"
          color={spo2Color}
          quality={latest?.signal_quality}
        />
        <BigStat
          label="Heart rate"
          value={latest?.hr != null ? Math.round(latest.hr) : null}
          unit="bpm"
          color="#34d399"
        />
        <div className="rounded-xl p-5" style={{ background: '#161b22', border: '1px solid #21262d' }}>
          <div className="text-xs text-gray-500 mb-2">Position</div>
          <div className="flex items-center gap-2">
            <span className="text-2xl">{POSITION_ICONS[latest?.position] || '⬜'}</span>
            <span className="text-lg font-medium text-white capitalize">{latest?.position || '—'}</span>
          </div>
        </div>
        <div className="rounded-xl p-5" style={{ background: '#161b22', border: '1px solid #21262d' }}>
          <div className="text-xs text-gray-500 mb-2">Snore level</div>
          <div className="mt-1">
            <div className="flex justify-between text-xs mb-1">
              <span className="text-gray-500">0</span>
              <span className="text-gray-500">100</span>
            </div>
            <div className="rounded-full overflow-hidden h-2.5" style={{ background: '#21262d' }}>
              <div
                className="h-full rounded-full transition-all duration-500"
                style={{
                  width: `${latest?.snore || 0}%`,
                  background: latest?.snore > 60 ? '#ef4444' : latest?.snore > 30 ? '#f59e0b' : '#22c55e',
                }}
              />
            </div>
            <div className="text-right text-sm font-bold text-white mt-1">
              {latest?.snore != null ? Math.round(latest.snore) : '—'}
            </div>
          </div>
        </div>
      </div>

      <div className="rounded-lg p-3 flex gap-2 text-xs" style={{ background: 'rgba(255,255,255,0.03)', border: '1px solid #21262d' }}>
        <span className="text-yellow-500 shrink-0">ⓘ</span>
        <span className="text-gray-500">
          Live view is for device verification and demo only. Real-time readings should not be used to make health decisions. This is a screening device, not a medical monitor.
        </span>
      </div>

      {/* Mini SpO2 sparkline */}
      {history.length > 1 && (
        <div>
          <div className="text-xs text-gray-400 mb-1 font-medium">SpO₂ — last {history.length}s</div>
          <svg width="100%" height="60" className="block">
            {history.map((d, i) => {
              const x = (i / (history.length - 1)) * 100;
              const y = d.spo2 != null ? 60 - ((d.spo2 - 88) / 12) * 56 : null;
              if (y == null) return null;
              return (
                <circle key={i} cx={`${x}%`} cy={y} r={1.5} fill={spo2Color} opacity={0.6 + 0.4 * (i / history.length)} />
              );
            })}
          </svg>
        </div>
      )}
    </div>
  );
}
