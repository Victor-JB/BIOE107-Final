import React from 'react';

const RISK_CONFIG = {
  low: {
    color: '#22c55e',
    bg: 'rgba(34,197,94,0.1)',
    border: 'rgba(34,197,94,0.3)',
    icon: '◎',
  },
  moderate: {
    color: '#f59e0b',
    bg: 'rgba(245,158,11,0.1)',
    border: 'rgba(245,158,11,0.3)',
    icon: '◐',
  },
  high: {
    color: '#ef4444',
    bg: 'rgba(239,68,68,0.1)',
    border: 'rgba(239,68,68,0.3)',
    icon: '●',
  },
};

export default function RiskIndicator({ level, label, guidance, ahi }) {
  const cfg = RISK_CONFIG[level] || RISK_CONFIG.low;
  const ahiDisplay = ahi != null ? `~${ahi} events/hr` : null;

  return (
    <div
      className="rounded-xl p-5"
      style={{ background: cfg.bg, border: `1px solid ${cfg.border}` }}
    >
      <div className="flex items-start gap-4">
        <span style={{ color: cfg.color, fontSize: 28, lineHeight: 1 }}>{cfg.icon}</span>
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-3 flex-wrap">
            <span
              className="text-xs font-bold uppercase tracking-widest px-2 py-0.5 rounded"
              style={{ color: cfg.color, background: cfg.bg, border: `1px solid ${cfg.border}` }}
            >
              {level} indication
            </span>
            {ahiDisplay && (
              <span className="text-xs text-gray-400">{ahiDisplay}</span>
            )}
          </div>
          <p className="mt-1.5 text-white font-medium text-sm leading-snug">{label}</p>
          <p className="mt-1 text-gray-400 text-xs leading-relaxed">{guidance}</p>
        </div>
      </div>

      <div
        className="mt-4 pt-3 flex items-start gap-2 text-xs text-gray-500"
        style={{ borderTop: '1px solid rgba(255,255,255,0.06)' }}
      >
        <span className="text-yellow-500 mt-0.5 shrink-0">ⓘ</span>
        <span>
          This is a <strong className="text-gray-400">screening estimate</strong>, not a medical diagnosis. This device cannot diagnose sleep apnea. Consult a healthcare provider for any concerns about your sleep health.
        </span>
      </div>

      <a
        href="https://www.sleepfoundation.org/sleep-apnea"
        target="_blank"
        rel="noopener noreferrer"
        className="mt-3 inline-flex items-center gap-1.5 text-xs text-blue-400 hover:text-blue-300 transition-colors"
      >
        Talk to a healthcare provider about sleep studies →
      </a>
    </div>
  );
}
